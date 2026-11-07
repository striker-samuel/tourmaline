// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace editor {

// Internal cubemap and GI-cook artifacts stay on disk/in the asset DB,
// but do not appear as browser tiles or collection results. Apply this
// policy to every listing lane (directory, collection and expanded views),
// never to the asset registry/cooker itself. Only call for files, not dirs.
inline bool HideAssetBrowserFile(const std::filesystem::path& path,
                                 std::string_view assetKind = {}) {
    auto lower = [](std::string text) {
        for (char& c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    const std::string ext = lower(path.extension().string());
    const std::string kind = lower(std::string(assetKind));
    return ext == ".cubemap" || ext == ".cubemanifest" || ext == ".gicook" || ext == ".dccsource" ||
           kind == "cubemap" || kind == "gicook";
}

// Root navigation is valid even though InProjectDir excludes the root
// itself (for operations that must target descendants).
inline bool AssetBrowserAtProjectRoot(const std::string& projectRoot,
                                      const std::string& currentDir) {
    return !projectRoot.empty() && currentDir == projectRoot;
}

// Collection folders are project-wide views, not children of every
// directory. Expanded glTF/tour views also have their own contents.
inline bool ShowAssetBrowserCollectionFolders(const std::string& projectRoot,
                                              const std::string& currentDir,
                                              bool gltfExpanded,
                                              bool tourExpanded) {
    return AssetBrowserAtProjectRoot(projectRoot, currentDir) && !gltfExpanded && !tourExpanded;
}

// A directory or collection change invalidates the complete selection,
// including shift-range anchors (not just the primary selected asset).
inline void ClearAssetBrowserSelection(std::string& primary,
                                       std::vector<std::string>& selection,
                                       std::string& anchor) {
    primary.clear();
    selection.clear();
    anchor.clear();
}

} // namespace editor
