// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Project-aware conversion at the editor -> tool boundary.
// Authored content paths are content-relative; the legacy tool VFS
// currently mounts at project root and requires a "content/" prefix.
// Keep those domains explicit instead of finding "content/" substrings.
#include <filesystem>
#include <string>

namespace smvk::project {

struct ResolvedProjectResource {
    std::string contentRelative; // e.g. models/bistro/bistro.geometry
    std::string projectVfs;      // legacy stage key: content/models/bistro/bistro.geometry
    std::string stageRoot;       // absolute project root for current tool adapters
};

inline bool ResolveProjectResource(const std::string& projectRoot,
                                   const std::string& input,
                                   ResolvedProjectResource& output,
                                   std::string& error) {
    namespace fs = std::filesystem;
    output = {};
    error.clear();
    if (projectRoot.empty() || input.empty()) {
        error = "project root and asset reference must not be empty";
        return false;
    }
    const fs::path root = fs::absolute(fs::path(projectRoot)).lexically_normal();
    const fs::path rawSource(input);
    // Refuse authored ".." components rather than normalizing them
    // away (e.g. content/../outside must not become a valid selector).
    for (const auto& component : rawSource) {
        if (component == "..") {
            error = "asset reference contains parent traversal: " + input;
            return false;
        }
    }
    const fs::path source = rawSource.lexically_normal();
    fs::path relative;
    if (source.is_absolute()) {
        relative = source.lexically_relative(root);
        if (relative.empty()) {
            error = "asset is not inside the project root: " + input;
            return false;
        }
    } else {
        relative = source;
    }
    // Never guess from a substring or accept traversal/absolute escapes.
    if (relative.empty() || relative == fs::path(".") || relative.is_absolute()) {
        error = "invalid asset reference: " + input;
        return false;
    }
    for (const auto& component : relative) {
        if (component == "..") {
            error = "asset reference escapes project root: " + input;
            return false;
        }
    }
    // Both content-relative and legacy project-relative *selectors* are
    // accepted at this adapter, but the returned logical path is unique.
    auto it = relative.begin();
    if (it != relative.end() && *it == "content") {
        ++it;
        if (it == relative.end() || *it == "content") {
            error = "duplicate or empty content/ prefix: " + input;
            return false;
        }
        fs::path tail;
        for (; it != relative.end(); ++it)
            tail /= *it;
        relative = tail;
    }
    if (relative.empty() || relative == fs::path(".")) {
        error = "missing content asset path: " + input;
        return false;
    }
    output.stageRoot = root.generic_string();
    output.contentRelative = relative.generic_string();
    output.projectVfs = (fs::path("content") / relative).generic_string();
    return true;
}

} // namespace smvk::project
