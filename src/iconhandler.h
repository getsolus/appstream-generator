/*
 * Copyright (C) 2016-2025 Matthias Klumpp <matthias@tenstral.net>
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
#include <generator>
#include <variant>
#include <mutex>
#include <appstream.h>
#include <appstream-compose.h>
#include <glib.h>

#include "utils.h"
#include "backends/interfaces.h"
#include "contentsstore.h"

namespace ASGenerator
{

class Config;
class GeneratorResult;

/**
 * Describes an icon theme as specified in the XDG theme spec.
 */
class Theme
{
public:
    explicit Theme(const std::string &name, const std::vector<std::uint8_t> &indexData, const std::string &prefix = {});
    explicit Theme(const std::string &name, std::shared_ptr<Package> pkg, const std::string &prefix = {});

    const std::string &name() const;
    const auto &directories() const
    {
        return m_directories;
    }

    /**
     * Check if a directory is suitable for the selected size.
     * If @assumeThresholdScalable is set to true, we will allow
     * downscaling of any higher-than-requested icon size, even if the
     * section is of "Threshold" type and would usually prohibit the scaling.
     */
    bool directoryMatchesSize(
        const std::unordered_map<std::string, std::variant<int, std::string>> &themedir,
        const ImageSize &size,
        bool assumeThresholdScalable = false) const;

    /**
     * Returns a generator of possible icon filenames that match @iconName and @size.
     * If @relaxedScalingRules is set to true, we scale down any bigger icon size, even
     * if the theme definition would usually prohibit that.
     */
    std::generator<std::string> matchingIconFilenames(
        const std::string &iconName,
        const ImageSize &size,
        bool relaxedScalingRules = false) const;

private:
    std::string m_name;
    std::string m_prefix;
    std::vector<std::unordered_map<std::string, std::variant<int, std::string>>> m_directories;
};

/**
 * Finds icons in a software archive and stores them in the
 * correct sizes for a given AppStream component.
 */
class IconHandler
{
public:
    IconHandler(
        ContentsStore &ccache,
        const fs::path &mediaPath,
        const std::unordered_map<std::string, std::shared_ptr<Package>> &pkgMap,
        const std::string &iconTheme = "",
        const std::string &extraPrefix = "");

    ~IconHandler();

    /**
     * Try to find & store icons for a selected component.
     */
    bool process(GeneratorResult &gres, AsComponent *cpt);

    static bool iconAllowed(const std::string &iconName);

    // Delete copy constructor and assignment operator
    IconHandler(const IconHandler &) = delete;
    IconHandler &operator=(const IconHandler &) = delete;

private:
    fs::path m_mediaExportPath;
    std::vector<std::unique_ptr<Theme>> m_themes;
    std::unordered_map<std::string, std::shared_ptr<Package>> m_iconFiles;
    std::vector<std::string> m_themeNames;
    std::string m_extraPrefix;

    AscIconPolicy *m_iconPolicy;
    ImageSize m_defaultIconSize;
    AscIconState m_defaultIconState;
    std::vector<ImageSize> m_enabledIconSizes;

    bool m_allowIconUpscaling;
    bool m_allowRemoteIcons;

    mutable std::mutex m_mutex;

    void updateEnabledIconSizeList();

    /**
     * Describes the icon a component requests to be processed.
     * Either a name (or absolute path) of a local icon, or the URL of a
     * remote icon declared in the metadata.
     */
    struct IconRequest {
        std::string name;
        std::string url;
        int width = -1;
        int height = -1;
    };

    IconRequest getIconRequestAndClear(AsComponent *cpt) const;

    /**
     * Download the icon data for a remote icon declared in the metadata.
     * Returns an empty vector if the icon could not be fetched or is not
     * usable (in which case a hint is emitted).
     */
    std::vector<std::uint8_t> fetchRemoteIcon(
        GeneratorResult &gres,
        AsComponent *cpt,
        const std::string &iconUrl) const;

    /**
     * Store a downloaded remote icon in the media export directory, in all
     * suitable sizes of the icon policy, and register the icon with the component.
     */
    bool storeRemoteIcon(
        GeneratorResult &gres,
        AsComponent *cpt,
        const fs::path &cptExportPath,
        const std::string &iconUrl,
        const std::vector<std::uint8_t> &iconData) const;

    /**
     * Generates potential filenames of the icon that is searched for in the
     * given size.
     */
    std::generator<std::string> possibleIconFilenames(
        const std::string &iconName,
        const ImageSize &size,
        bool relaxedScalingRules = false) const;

    /**
     * Helper structure for the findIcons method.
     */
    struct IconFindResult {
        std::shared_ptr<Package> pkg;
        std::string fname;

        IconFindResult() = default;
        IconFindResult(std::shared_ptr<Package> p, std::string f)
            : pkg(std::move(p)),
              fname(std::move(f))
        {
        }
    };

    /**
     * Looks up 'icon' with 'size' in popular icon themes according to the XDG
     * icon theme spec.
     */
    std::unordered_map<ImageSize, IconFindResult> findIcons(
        const std::string &iconName,
        const std::vector<ImageSize> &sizes,
        std::shared_ptr<Package> pkg = nullptr) const;

    /**
     * Strip file extension from icon.
     */
    static std::string stripIconExt(const std::string &iconName);

    /**
     * Extracts the icon from the package and stores it in the cache.
     * Ensures the stored icon always has the size given in "size", and renders
     * scalable vectorgraphics if necessary.
     */
    bool storeIcon(
        AsComponent *cpt,
        GeneratorResult &gres,
        const fs::path &cptExportPath,
        std::shared_ptr<Package> sourcePkg,
        const std::string &iconPath,
        const ImageSize &size,
        AscIconState targetState) const;

    /**
     * Add the cached and/or remote icon entries for an icon that is (already)
     * stored in the media export directory.
     */
    void addStoredIconEntries(
        AsComponent *cpt,
        GeneratorResult &gres,
        const std::string &iconName,
        const ImageSize &size,
        AscIconState targetState) const;

    /**
     * Render and store the given icon data in the media export directory and
     * register the icon with the component.
     */
    bool storeIconData(
        AsComponent *cpt,
        GeneratorResult &gres,
        const fs::path &cptExportPath,
        const std::string &iconName,
        const std::string &iconFname,
        const std::vector<std::uint8_t> &iconData,
        const ImageSize &size,
        AscIconState targetState,
        const std::string &sourceFname) const;

    /**
     * Helper function to try to find an icon that we can up- or downscale to the desired size.
     */
    IconFindResult findIconScalableToSize(
        const std::unordered_map<ImageSize, IconFindResult> &possibleIcons,
        const ImageSize &size) const;
};

} // namespace ASGenerator
