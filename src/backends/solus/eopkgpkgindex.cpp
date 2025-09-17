/*
 * Copyright (C) 2025 Solus Developers <copyright@getsol.us>
 *
 * Licensed under the GNU Lesser General Public License Version 3
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the license, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this software.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "eopkgpkgindex.h"
#include "eopkgpkg.h"


#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <cstring>

#include <libxml/parser.h>

#include "../../config.h"
#include "../../logging.h"
#include "../../downloader.h"
#include "../../zarchive.h"
#include "../../datastore.h"

namespace ASGenerator
{

namespace fs = std::filesystem;

static std::string getXmlStrAttr(xmlNodePtr elem, const std::string &name)
{
    if (!elem || !elem->properties)
        return {};

    for (xmlAttrPtr attr = elem->properties; attr; attr = attr->next) {
        if (attr->name && std::strcmp(reinterpret_cast<const char *>(attr->name), name.c_str()) == 0) {
            if (attr->children && attr->children->content)
                return reinterpret_cast<const char *>(attr->children->content);
        }
    }
    return {};
}

static std::string getXmlElemText(xmlNodePtr elem)
{
    if (!elem)
        return {};

    for (xmlNodePtr child = elem->children; child; child = child->next) {
        if (child->type == XML_TEXT_NODE && child->content)
            return reinterpret_cast<const char *>(child->content);
    }
    return {};
}

EopkgPackageIndex::EopkgPackageIndex(const std::string &dir)
    : m_rootDir(dir)
{
    if (!Utils::isRemote(dir) && !fs::exists(dir)) {
        throw std::runtime_error("Directory '" + dir + "' does not exist.");
    }

    const auto &conf = Config::get();
    m_tmpRootDir = (conf.getTmpDir() / fs::path(dir).filename()).string();
}

void EopkgPackageIndex::release()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pkgCache.clear();
}

std::string EopkgPackageIndex::downloadIfNecessary(const std::string &fname, const std::string &tempDir)
{
    if (!Utils::isRemote(fname)) {
        return fname;
    }

    const auto &conf = Config::get();
    std::string actualTempDir = tempDir.empty() ? conf.getTmpDir().string() : tempDir;

    if (!fs::exists(actualTempDir)) {
        fs::create_directories(actualTempDir);
    }

    auto &dl = Downloader::get();
    fs::path path = fs::path(actualTempDir) / fs::path(fname).filename();

    try {
        dl.downloadFile(fname, path.string());
    } catch (const std::exception &e) {
        logError("Unable to download: {}", e.what());
        throw;
    }

    return path.string();
}

std::string EopkgPackageIndex::getIndexPath(const std::string &rootDir, const std::string &suite)
{
    std::string indexPath;

    if (Utils::isRemote(rootDir)) {
        // For remote repositories, prefer the compressed version to save bandwidth
        indexPath = rootDir + "/" + suite + "/eopkg-index.xml.xz";
    } else {
        // For local repositories, try the uncompressed version first
        indexPath = rootDir + "/" + suite + "/eopkg-index.xml";

        // If the uncompressed file doesn't exist locally, try the compressed version
        if (!fs::exists(indexPath)) {
            indexPath = rootDir + "/" + suite + "/eopkg-index.xml.xz";
        }
    }
    return indexPath;
}

std::string EopkgPackageIndex::getIndexContent(const std::string &indexFname)
{
    if (indexFname.ends_with(".xz")) {
        return decompressFile(indexFname);
    } else {
        std::ifstream file(indexFname, std::ios::binary);
        if (!file) {
            throw std::runtime_error("Cannot open index file: " + indexFname);
        }

        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
}

std::vector<std::shared_ptr<EopkgPackage>> EopkgPackageIndex::loadPackages(
    const std::string &suite,
    const std::string &section,
    const std::string &arch)
{
    auto indexPath = getIndexPath(m_rootDir, suite);

    std::string indexFname;
    {
        //std::lock_guard<std::mutex> lock(m_mutex);
        indexFname = downloadIfNecessary(indexPath, m_tmpRootDir);
    }

    auto indexContent = getIndexContent(indexFname);

    // Parse XML index file using libxml2
    xmlDocPtr doc = xmlParseMemory(indexContent.c_str(), static_cast<int>(indexContent.length()));
    if (!doc) {
        logError("Failed to parse repository index XML");
        return {};
    }

    std::vector<std::shared_ptr<EopkgPackage>> packages;

    // Find PISI root element
    xmlNodePtr pisiNode = xmlDocGetRootElement(doc);
    if (!pisiNode || std::strcmp(reinterpret_cast<const char *>(pisiNode->name), "PISI") != 0) {
        logError("Repository index does not contain a PISI root element");
        xmlFreeDoc(doc);
        return packages;
    }

    // Process all Package elements in the index
    for (xmlNodePtr packageNode = pisiNode->children; packageNode; packageNode = packageNode->next) {
        if (packageNode->type != XML_ELEMENT_NODE || std::strcmp(reinterpret_cast<const char *>(packageNode->name), "Package") != 0)
            continue;

        auto pkg = std::make_shared<EopkgPackage>();

        // Extract basic package information
        std::string currentPkgName;
        xmlNodePtr nameNode = nullptr;

        for (xmlNodePtr node = packageNode->children; node; node = node->next) {
            if (node->type == XML_ELEMENT_NODE && std::strcmp(reinterpret_cast<const char *>(node->name), "Name") == 0) {
                nameNode = node;
                break;
            }
        }

        if (nameNode) {
            currentPkgName = getXmlElemText(nameNode);
        }

        if (currentPkgName.empty()) {
            logWarning("Skipping package entry without a name.");
            continue; // Cannot process without a name
        }

        // Optimization: Skip -devel and -dbginfo subpackages, they'll never contain anything
        //               interesting.
        if (currentPkgName.ends_with("-devel") || currentPkgName.ends_with("-dbginfo")) {
            logDebug("Skipping development/debug package: {}", currentPkgName);
            continue;
        }

        // Set the package name if it's not skipped
        pkg->setName(currentPkgName);

        // Process all child elements of the package
        for (xmlNodePtr child = packageNode->children; child; child = child->next) {
            if (child->type != XML_ELEMENT_NODE)
                continue;

            const char* childName = reinterpret_cast<const char *>(child->name);

            if (std::strcmp(childName, "History") == 0) {
                // Find the first Update element
                for (xmlNodePtr updateNode = child->children; updateNode; updateNode = updateNode->next) {
                    if (updateNode->type == XML_ELEMENT_NODE && std::strcmp(reinterpret_cast<const char *>(updateNode->name), "Update") == 0) {
                        std::string release = getXmlStrAttr(updateNode, "release");
                        if (release.empty()) release = "1";

                        // Find Version element
                        for (xmlNodePtr versionNode = updateNode->children; versionNode; versionNode = versionNode->next) {
                            if (versionNode->type == XML_ELEMENT_NODE && std::strcmp(reinterpret_cast<const char *>(versionNode->name), "Version") == 0) {
                                std::string version = getXmlElemText(versionNode);
                                if (!version.empty()) {
                                    pkg->setVersion(version + "-" + release);
                                }
                                break;
                            }
                        }
                        break;
                    }
                }
            } else if (std::strcmp(childName, "Architecture") == 0) {
                std::string archStr = getXmlElemText(child);
                if (!archStr.empty()) {
                    pkg->setArch(archStr);
                }
            } else if (std::strcmp(childName, "PackageURI") == 0) {
                std::string packageURI = getXmlElemText(child);
                if (!packageURI.empty()) {
                    // The PackageURI in eopkg-index.xml contains the relative path to the package
                    // We need to preserve this path structure
                    std::string pkgPath = m_rootDir + "/" + suite + "/" + packageURI;
                    pkg->setFilename(pkgPath);
                    logDebug("Package path: {}", pkgPath);
                }
            } else if (std::strcmp(childName, "Summary") == 0) {
                std::string lang = getXmlStrAttr(child, "xml:lang");
                if (lang.empty()) lang = "en";
                std::string summary = getXmlElemText(child);
                if (!summary.empty()) {
                    pkg->setSummary(summary, lang);
                }
            } else if (std::strcmp(childName, "Description") == 0) {
                std::string lang = getXmlStrAttr(child, "xml:lang");
                if (lang.empty()) lang = "en";
                std::string description = getXmlElemText(child);
                if (!description.empty()) {
                    pkg->setDescription(description, lang);
                }
            } else if (std::strcmp(childName, "Source") == 0) {
                // Get maintainer information
                xmlNodePtr packagerNode = nullptr;
                for (xmlNodePtr node = child->children; node; node = node->next) {
                    if (node->type == XML_ELEMENT_NODE && std::strcmp(reinterpret_cast<const char *>(node->name), "Packager") == 0) {
                        packagerNode = node;
                        break;
                    }
                }
                if (packagerNode) {
                    for (xmlNodePtr node = packagerNode->children; node; node = node->next) {
                        if (node->type == XML_ELEMENT_NODE && std::strcmp(reinterpret_cast<const char *>(node->name), "Email") == 0) {
                            std::string email = getXmlElemText(node);
                            if (!email.empty()) {
                                pkg->setMaintainer(email);
                            }
                            break;
                        }
                    }
                }
            }
        }

        // Set default maintainer if not found
        if (pkg->maintainer().empty()) {
            pkg->setMaintainer("solus@getsol.us");
        }

        // Add the package to our list if it's valid
        if (pkg->isValid()) {
            packages.push_back(pkg);
        } else {
            logError("Found an invalid package entry for '{}' (name, architecture or version is missing). Skipping it.", pkg->name());
        }
    }

    xmlFreeDoc(doc);
    return packages;
}

std::vector<std::shared_ptr<Package>> EopkgPackageIndex::packagesFor(
    const std::string &suite,
    const std::string &section,
    const std::string &arch,
    bool withLongDescs)
{
    const std::string id = std::format("{}-{}-{}", suite, section, arch);

    // Thread-safe cache access
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_pkgCache.find(id);
    if (it == m_pkgCache.end()) {
        auto pkgs = loadPackages(suite, section, arch);

        std::vector<std::shared_ptr<Package>> packagePtrs;
        packagePtrs.reserve(pkgs.size());
        for (const auto &pkg : pkgs)
            packagePtrs.push_back(std::static_pointer_cast<Package>(pkg));
        m_pkgCache[id] = packagePtrs;

        return packagePtrs;
    }

    return it->second;
}

std::shared_ptr<Package> EopkgPackageIndex::packageForFile(
    const std::string &fname,
    const std::string &suite,
    const std::string &section)
{
    // Only handle .eopkg files
    if (!fname.ends_with(".eopkg")) {
        return nullptr;
    }

    if (!fs::exists(fname)) {
        return nullptr;
    }

    try {
        // Create a new package for this file
        auto pkg = std::make_shared<EopkgPackage>();
        pkg->setFilename(fname);

        // Extract metadata to populate fields
        pkg->extractMetadata();

        return std::static_pointer_cast<Package>(pkg);
    } catch (const std::exception &e) {
        logError("Failed to process package file '{}': {}", fname, e.what());
        return nullptr;
    }
}

bool EopkgPackageIndex::hasChanges(
    std::shared_ptr<DataStore> dstore,
    const std::string &suite,
    const std::string &section,
    const std::string &arch)
{
    auto indexPath = getIndexPath(m_rootDir, suite);

    std::string indexFname;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        indexFname = downloadIfNecessary(indexPath, m_tmpRootDir);
    }

    auto indexContent = getIndexContent(indexFname);

    // Get file modification time
    auto ftime = fs::last_write_time(indexFname);
    auto currentTime = std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();

    auto repoInfo = dstore->getRepoInfo(suite, section, arch);

    // Check if we have stored mtime information
    bool hasStoredMtime = false;
    int64_t pastTime = 0;

    try {
        auto mtimeIt = repoInfo.data.find("mtime");
        if (mtimeIt != repoInfo.data.end()) {
            if (std::holds_alternative<std::int64_t>(mtimeIt->second)) {
                pastTime = std::get<std::int64_t>(mtimeIt->second);
                hasStoredMtime = true;
            }
        }
    } catch (const std::exception &e) {
        logDebug("Failed to parse stored mtime: {}", e.what());
        hasStoredMtime = false;
    }

    // Update stored mtime
    repoInfo.data["mtime"] = currentTime;
    dstore->setRepoInfo(suite, section, arch, repoInfo);

    if (!hasStoredMtime) {
        m_indexChanged[indexFname] = true;
        return true;
    }

    if (pastTime != currentTime) {
        m_indexChanged[indexFname] = true;
        return true;
    }

    m_indexChanged[indexFname] = false;
    return false;
}

} // namespace ASGenerator
