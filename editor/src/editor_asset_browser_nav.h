// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <string>

namespace editor {

// Root navigation is valid even though InProjectDir excludes the root
// itself (for operations that must target descendants).
inline bool AssetBrowserAtProjectRoot(const std::string& projectRoot,
    const std::string& currentDir) {
    return !projectRoot.empty() && currentDir == projectRoot;
}

// Collection folders are project-wide views, not children of every
// directory. Expanded glTF/tour views also have their own contents.
inline bool ShowAssetBrowserCollectionFolders(
    const std::string& projectRoot, const std::string& currentDir,
    bool gltfExpanded, bool tourExpanded) {
    return AssetBrowserAtProjectRoot(projectRoot, currentDir)
        && !gltfExpanded && !tourExpanded;
}

} // namespace editor
