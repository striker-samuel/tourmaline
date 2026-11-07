// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// Asset-browser creation helpers: only author beneath the project's
// content/ tree. Canonicalize existing directories so a symlink from
// content/ to outside the project cannot escape the project boundary.
#include <filesystem>
#include <string>
#include <system_error>

namespace editor {
inline std::filesystem::path AssetCreateDirectory(const std::filesystem::path& project,
                                                  const std::filesystem::path& browsed,
                                                  const std::filesystem::path& defaultSubdir = {}) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path content = fs::weakly_canonical(project / "content", ec);
    if (ec || content.empty())
        return {};
    // operator/ with an EMPTY right-hand side is library-divergent: MSVC
    // appends a trailing separator (content/), libstdc++ returns the left
    // side unchanged - the create.fallback asset_ops checks compare against
    // the separator-less path, so guard the empty case explicitly.
    const fs::path fallback = defaultSubdir.empty() ? content : content / defaultSubdir;
    if (browsed.empty())
        return fallback;
    ec.clear();
    const fs::path candidate = fs::weakly_canonical(browsed, ec);
    if (ec || !fs::is_directory(candidate, ec))
        return fallback;
    const fs::path rel = candidate.lexically_relative(content);
    if (rel.empty())
        return fallback;
    for (const fs::path& part : rel)
        if (part == "..")
            return fallback;
    return candidate;
}

inline std::filesystem::path UniqueAssetCreatePath(const std::filesystem::path& directory,
                                                   const std::string& stem,
                                                   const std::string& extension = {}) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (unsigned int n = 1; n < 100000; ++n) {
        const std::string name = stem + (n == 1 ? "" : " " + std::to_string(n));
        const fs::path candidate = directory / (name + extension);
        ec.clear();
        if (!fs::exists(candidate, ec) && !ec && !fs::exists(candidate.string() + ".meta", ec) &&
            !ec)
            return candidate;
    }
    return {};
}
} // namespace editor
