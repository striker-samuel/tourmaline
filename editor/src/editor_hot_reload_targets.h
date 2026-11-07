// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Pure source->derived-identity check. The editor's world/ref records
// normally bind .geometry/.mesh, not the original .gltf/.glb/.obj.
#include <filesystem>
#include <string>

namespace editor {

[[nodiscard]] inline bool IsPublishedMeshBound(const std::string& publishedSource,
                                               const std::string& boundPath) {
    namespace fs = std::filesystem;
    if (publishedSource.empty() || boundPath.empty())
        return false;
    std::error_code ec;
    auto source = fs::absolute(fs::path(publishedSource), ec);
    if (ec)
        return false;
    auto bound = fs::absolute(fs::path(boundPath), ec);
    if (ec)
        return false;
    source = source.lexically_normal();
    bound = bound.lexically_normal();
    if (source == bound)
        return true;
    source.replace_extension(".geometry");
    if (source == bound)
        return true;
    source.replace_extension(".mesh");
    return source == bound;
}

} // namespace editor
