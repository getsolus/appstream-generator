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

#include "../interfaces.h"
#include "../../utils.h"

namespace ASGenerator
{

class EopkgPackage;

/**
 * Index implementation for Solus eopkg packages.
 * The eopkg index is an XML file that lists all packages in a repository.
 */
class EopkgPackageIndex : public PackageIndex
{
public:
    explicit EopkgPackageIndex(const std::string &dir);
    ~EopkgPackageIndex() override = default;

    // PackageIndex interface implementation
    void release() override;

    std::vector<std::shared_ptr<Package>> packagesFor(
        const std::string &suite,
        const std::string &section,
        const std::string &arch,
        bool withLongDescs = true) override;

    std::shared_ptr<Package> packageForFile(
        const std::string &fname,
        const std::string &suite = "",
        const std::string &section = "") override;

    bool hasChanges(
        std::shared_ptr<DataStore> dstore,
        const std::string &suite,
        const std::string &section,
        const std::string &arch) override;

private:
    std::string m_rootDir;
    std::string m_tmpRootDir;
    std::unordered_map<std::string, std::vector<std::shared_ptr<Package>>> m_pkgCache;
    std::unordered_map<std::string, bool> m_indexChanged;

    mutable std::mutex m_mutex;

    /**
     * Download a file if it's remote, or return the local path if it's already local.
     */
    std::string downloadIfNecessary(const std::string &fname, const std::string &tempDir = "");

    /**
     * Get the path to the index file for a given suite
     */
    std::string getIndexPath(const std::string &rootDir, const std::string &suite);

    /**
     * Get the content of an index file, decompressing if necessary
     */
    std::string getIndexContent(const std::string &indexFname);

    /**
     * Load packages from the eopkg repository index.
     * In Solus, the index is contained in an eopkg-index.xml.xz file.
     */
    std::vector<std::shared_ptr<EopkgPackage>> loadPackages(
        const std::string &suite,
        const std::string &section,
        const std::string &arch);
};

} // namespace ASGenerator