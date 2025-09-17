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

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <optional>

#include "../interfaces.h"
#include "../../utils.h"

namespace ASGenerator
{

class ArchiveDecompressor;

/**
 * Represents an eopkg package in the Solus distribution.
 * An eopkg package is a zip archive that contains metadata.xml and files.xml
 * describing the package, as well as an archive called install.tar.xz which contains the actual files.
 */
class EopkgPackage : public Package
{
public:
    EopkgPackage();
    ~EopkgPackage() override;

    // Package interface implementation
    std::string name() const override;
    std::string ver() const override;
    std::string arch() const override;
    std::string maintainer() const override;

    const std::unordered_map<std::string, std::string> &description() const override;
    const std::unordered_map<std::string, std::string> &summary() const override;

    std::string getFilename() override;
    const std::vector<std::string> &contents() override;
    std::vector<std::uint8_t> getFileData(const std::string &fname) override;

    void cleanupTemp() override;
    void finish() override;

    // Eopkg-specific methods
    void setName(const std::string &s);
    void setVersion(const std::string &s);
    void setArch(const std::string &s);
    void setMaintainer(const std::string &maint);
    void setFilename(const std::string &fname);
    void setDescription(const std::string &text, const std::string &locale);
    void setSummary(const std::string &text, const std::string &locale);
    void setContents(const std::vector<std::string> &contents);

    /**
     * Extract and parse metadata from the eopkg file.
     * In eopkg, the metadata is stored in metadata.xml and files.xml files in the root of the archive.
     */
    void extractMetadata();

private:
    std::string m_pkgname;
    std::string m_pkgver;
    std::string m_pkgarch;
    std::string m_pkgmaintainer;
    std::unordered_map<std::string, std::string> m_description;
    std::unordered_map<std::string, std::string> m_summary;

    std::string m_pkgFname;
    std::string m_localPkgFname;
    std::vector<std::string> m_contentsL;
    fs::path m_tmpDir;

    std::unique_ptr<ArchiveDecompressor> m_archive;
    std::unique_ptr<ArchiveDecompressor> m_installTarArchive;
    bool m_metadataExtracted;

    mutable std::mutex m_mutex;

    /**
     * Clean up a temporary directory, ignoring any errors
     */
    void cleanupTempDir(const fs::path &path);

    /**
     * Download package file if it's remote
     */
    std::string downloadIfNecessary();
};

} // namespace ASGenerator
