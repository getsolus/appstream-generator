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

#include "eopkgpkg.h"

#include <filesystem>
#include <cstring>

#include <libxml/parser.h>

#include "../../config.h"
#include "../../logging.h"
#include "../../zarchive.h"
#include "../../downloader.h"
#include "../../utils.h"

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

EopkgPackage::EopkgPackage()
    : m_metadataExtracted(false)
{
}

EopkgPackage::~EopkgPackage()
{
    finish();
}

std::string EopkgPackage::name() const
{
    return m_pkgname;
}

std::string EopkgPackage::ver() const
{
    return m_pkgver;
}

std::string EopkgPackage::arch() const
{
    return m_pkgarch;
}

std::string EopkgPackage::maintainer() const
{
    return m_pkgmaintainer;
}

const std::unordered_map<std::string, std::string> &EopkgPackage::description() const
{
    return m_description;
}

const std::unordered_map<std::string, std::string> &EopkgPackage::summary() const
{
    return m_summary;
}

void EopkgPackage::setName(const std::string &s)
{
    m_pkgname = s;
}

void EopkgPackage::setVersion(const std::string &s)
{
    m_pkgver = s;
}

void EopkgPackage::setArch(const std::string &s)
{
    m_pkgarch = s;
}

void EopkgPackage::setMaintainer(const std::string &maint)
{
    m_pkgmaintainer = maint;
}

void EopkgPackage::setFilename(const std::string &fname)
{
    m_pkgFname = fname;
}

void EopkgPackage::setDescription(const std::string &text, const std::string &locale)
{
    m_description[locale] = text;
}

void EopkgPackage::setSummary(const std::string &text, const std::string &locale)
{
    m_summary[locale] = text;
}

void EopkgPackage::setContents(const std::vector<std::string> &contents)
{
    m_contentsL = contents;
}

std::string EopkgPackage::downloadIfNecessary()
{
    if (!m_localPkgFname.empty())
        return m_localPkgFname;

    if (Utils::isRemote(m_pkgFname)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto &conf = Config::get();
        auto &dl = Downloader::get();

        // Handle eopkg format: name-version-release-distribution-arch.eopkg
        // Format a temporary filename that keeps the package identity but is unique
        fs::path path = fs::path(conf.getTmpDir()) / (m_pkgname + "-" + m_pkgver + "-1-1-" + m_pkgarch + ".eopkg.tmp");

        try {
            dl.downloadFile(m_pkgFname, path.string());
        } catch (const std::exception &e) {
            logError("Unable to download: {}, reason {}", m_pkgFname, e.what());
            throw;
        }
        m_localPkgFname = path.string();
        return m_localPkgFname;
    } else {
        m_localPkgFname = m_pkgFname;
        return m_pkgFname;
    }
}

std::string EopkgPackage::getFilename()
{
    return downloadIfNecessary();
}

void EopkgPackage::extractMetadata()
{
    if (m_metadataExtracted)
        return;

    if (!m_archive) {
        m_archive = std::make_unique<ArchiveDecompressor>();
        m_archive->open(getFilename());
    }

    try {
        // Get the metadata XML
        auto metadataXml = m_archive->readData("metadata.xml");
        auto filesXml = m_archive->readData("files.xml");

        // Parse the metadata using libxml2
        xmlDocPtr metadataDoc = xmlParseMemory(reinterpret_cast<const char*>(metadataXml.data()), static_cast<int>(metadataXml.size()));
        if (!metadataDoc) {
            throw std::runtime_error("Failed to parse metadata.xml");
        }

        xmlDocPtr filesDoc = xmlParseMemory(reinterpret_cast<const char*>(filesXml.data()), static_cast<int>(filesXml.size()));
        if (!filesDoc) {
            xmlFreeDoc(metadataDoc);
            throw std::runtime_error("Failed to parse files.xml");
        }

        // Check for PISI root element
        xmlNodePtr pisiNode = xmlDocGetRootElement(metadataDoc);
        if (!pisiNode || std::strcmp(reinterpret_cast<const char *>(pisiNode->name), "PISI") != 0) {
            xmlFreeDoc(metadataDoc);
            xmlFreeDoc(filesDoc);
            throw std::runtime_error("PISI root element not found in metadata.xml");
        }

        // Extract package information - find the Package node
        xmlNodePtr packageNode = nullptr;
        for (xmlNodePtr node = pisiNode->children; node; node = node->next) {
            if (node->type == XML_ELEMENT_NODE && std::strcmp(reinterpret_cast<const char *>(node->name), "Package") == 0) {
                packageNode = node;
                break;
            }
        }

        if (!packageNode) {
            xmlFreeDoc(metadataDoc);
            xmlFreeDoc(filesDoc);
            throw std::runtime_error("Package node not found in metadata.xml");
        }

        // Try to get the maintainer information from the package's Source section first
        xmlNodePtr sourceNode = nullptr;
        for (xmlNodePtr node = packageNode->children; node; node = node->next) {
            if (node->type == XML_ELEMENT_NODE && std::strcmp(reinterpret_cast<const char *>(node->name), "Source") == 0) {
                sourceNode = node;
                break;
            }
        }

        if (sourceNode) {
            xmlNodePtr packagerNode = nullptr;
            for (xmlNodePtr node = sourceNode->children; node; node = node->next) {
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
                            setMaintainer(email);
                        }
                        break;
                    }
                }
            }
        }

        // If maintainer wasn't found, try the top-level Source element
        if (m_pkgmaintainer.empty()) {
            for (xmlNodePtr node = pisiNode->children; node; node = node->next) {
                if (node->type == XML_ELEMENT_NODE && std::strcmp(reinterpret_cast<const char *>(node->name), "Source") == 0) {
                    sourceNode = node;
                    break;
                }
            }
            if (sourceNode) {
                xmlNodePtr packagerNode = nullptr;
                for (xmlNodePtr node = sourceNode->children; node; node = node->next) {
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
                                setMaintainer(email);
                            }
                            break;
                        }
                    }
                }
            }
        }

        // Default maintainer if still not found
        if (m_pkgmaintainer.empty()) {
            setMaintainer("solus@getsol.us");
        }

        // Get name, version, and architecture
        for (xmlNodePtr node = packageNode->children; node; node = node->next) {
            if (node->type != XML_ELEMENT_NODE)
                continue;

            const char* nodeName = reinterpret_cast<const char *>(node->name);
            if (std::strcmp(nodeName, "Name") == 0) {
                std::string name = getXmlElemText(node);
                if (!name.empty()) {
                    setName(name);
                }
            } else if (std::strcmp(nodeName, "History") == 0) {
                // Find the first Update element
                for (xmlNodePtr updateNode = node->children; updateNode; updateNode = updateNode->next) {
                    if (updateNode->type == XML_ELEMENT_NODE && std::strcmp(reinterpret_cast<const char *>(updateNode->name), "Update") == 0) {
                        std::string release = getXmlStrAttr(updateNode, "release");
                        if (release.empty()) release = "1";

                        // Find Version element
                        for (xmlNodePtr versionNode = updateNode->children; versionNode; versionNode = versionNode->next) {
                            if (versionNode->type == XML_ELEMENT_NODE && std::strcmp(reinterpret_cast<const char *>(versionNode->name), "Version") == 0) {
                                std::string version = getXmlElemText(versionNode);
                                if (!version.empty()) {
                                    setVersion(version + "-" + release);
                                }
                                break;
                            }
                        }
                        break;
                    }
                }
            } else if (std::strcmp(nodeName, "Architecture") == 0) {
                std::string arch = getXmlElemText(node);
                if (!arch.empty()) {
                    setArch(arch);
                }
            } else if (std::strcmp(nodeName, "Summary") == 0) {
                std::string lang = getXmlStrAttr(node, "xml:lang");
                if (lang.empty()) lang = "en";
                std::string summary = getXmlElemText(node);
                if (!summary.empty()) {
                    setSummary(summary, lang);
                }
            } else if (std::strcmp(nodeName, "Description") == 0) {
                std::string lang = getXmlStrAttr(node, "xml:lang");
                if (lang.empty()) lang = "en";
                std::string description = getXmlElemText(node);
                if (!description.empty()) {
                    setDescription(description, lang);
                }
            }
        }

        // Extract file list from files.xml
        xmlNodePtr filesRootNode = xmlDocGetRootElement(filesDoc);
        if (!filesRootNode || std::strcmp(reinterpret_cast<const char *>(filesRootNode->name), "Files") != 0) {
            xmlFreeDoc(metadataDoc);
            xmlFreeDoc(filesDoc);
            throw std::runtime_error("Files root element not found in files.xml");
        }

        std::vector<std::string> contents;
        for (xmlNodePtr fileNode = filesRootNode->children; fileNode; fileNode = fileNode->next) {
            if (fileNode->type == XML_ELEMENT_NODE && std::strcmp(reinterpret_cast<const char *>(fileNode->name), "File") == 0) {
                // Find Path element
                for (xmlNodePtr pathNode = fileNode->children; pathNode; pathNode = pathNode->next) {
                    if (pathNode->type == XML_ELEMENT_NODE && std::strcmp(reinterpret_cast<const char *>(pathNode->name), "Path") == 0) {
                        std::string path = getXmlElemText(pathNode);
                        if (!path.empty()) {
                            // Ensure path starts with a slash
                            if (path[0] != '/') {
                                path = "/" + path;
                            }
                            contents.push_back(path);
                        }
                        break;
                    }
                }
            }
        }

        setContents(contents);
        m_metadataExtracted = true;

        xmlFreeDoc(metadataDoc);
        xmlFreeDoc(filesDoc);

    } catch (const std::exception &e) {
        logError("Failed to parse eopkg metadata: {}", e.what());
        throw;
    }
}

const std::vector<std::string> &EopkgPackage::contents()
{
    if (m_contentsL.empty()) {
        extractMetadata();
    }
    return m_contentsL;
}

std::vector<std::uint8_t> EopkgPackage::getFileData(const std::string &fname)
{
    // Ensure the package file is available locally before taking the lock
    const auto localFname = getFilename();

    std::lock_guard<std::mutex> lock(m_mutex);

    // Ensure metadata is extracted first (reads metadata.xml/files.xml from the zip)
    if (!m_metadataExtracted) {
        // Open the eopkg zip to extract metadata
        if (!m_archive) {
            m_archive = std::make_unique<ArchiveDecompressor>();
            m_archive->open(localFname);
        }
        extractMetadata();
    }

    if (!m_installTarArchive || !m_installTarArchive->isOpen()) {
        // Re-open the eopkg zip if it was previously cleaned up
        if (!m_archive) {
            m_archive = std::make_unique<ArchiveDecompressor>();
            m_archive->open(localFname);
        }

        const auto &conf = Config::get();
        m_tmpDir = fs::path(conf.getTmpDir()) / ("eopkg-" + m_pkgname + "-" + m_pkgver);
        if (!fs::exists(m_tmpDir)) {
            fs::create_directories(m_tmpDir);
        }

        // Extract install.tar.xz to the temp directory
        fs::path tarPath = m_tmpDir / "install.tar.xz";
        bool extracted = m_archive->extractFileTo("install.tar.xz", tarPath.string());
        if (!extracted) {
            throw std::runtime_error("Failed to extract install.tar.xz from package");
        }

        // Open the extracted tarball
        m_installTarArchive = std::make_unique<ArchiveDecompressor>();
        m_installTarArchive->open(tarPath.string(), m_tmpDir / "data");
        m_installTarArchive->setOptimizeRepeatedReads(true);
    }

    // Remove any leading slash in the file path
    std::string actualPath = fname;
    if (!actualPath.empty() && actualPath[0] == '/') {
        actualPath = actualPath.substr(1);
    }

    try {
        return m_installTarArchive->readData(actualPath);
    } catch (const std::exception &e) {
        logWarning("File '{}' not found in install.tar.xz: {}", actualPath, e.what());
        return {};
    }
}

void EopkgPackage::cleanupTemp()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_installTarArchive) {
        m_installTarArchive.reset();
    }
    if (m_archive) {
        m_archive.reset();
    }
    if (!m_tmpDir.empty()) {
        cleanupTempDir(m_tmpDir);
        m_tmpDir.clear();
    }
}

void EopkgPackage::cleanupTempDir(const fs::path &path)
{
    try {
        if (fs::exists(path)) {
            fs::remove_all(path);
        }
    } catch (const std::exception &e) {
        // Ignore errors when cleaning up
        logDebug("Unable to remove temporary directory: {} ({})", path.string(), e.what());
    }
}

void EopkgPackage::finish()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_installTarArchive) {
        m_installTarArchive.reset();
    }
    if (m_archive) {
        m_archive.reset();
    }
    if (!m_tmpDir.empty()) {
        cleanupTempDir(m_tmpDir);
        m_tmpDir.clear();
    }

    try {
        if (Utils::isRemote(m_pkgFname) && !m_localPkgFname.empty() && fs::exists(m_localPkgFname)) {
            logDebug("Deleting temporary package file {}", m_localPkgFname);
            fs::remove(m_localPkgFname);
            m_localPkgFname.clear();
        }
    } catch (const std::exception &e) {
        // we ignore any error
        logDebug("Unable to remove temporary package: {} ({})", m_localPkgFname, e.what());
    }
}

} // namespace ASGenerator
