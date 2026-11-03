// SPDX-License-Identifier: MIT
#pragma once
// User-owned source/content/publish layout. No ROM, extracted content,
// recomp output or cooked pak is ever staged under this repository.
// Canonicalization resolves existing symlinks and component boundaries.
#include <filesystem>
#include <string>
namespace tour::recomp {
inline bool PathWithin(std::filesystem::path child,
                       std::filesystem::path root) {
    if(root.empty() || child.empty()) return false;
    auto a=child.begin(), b=root.begin();
    for(;b!=root.end();++b,++a)
        if(a==child.end() || *a!=*b)return false;
    return true;
}
inline bool CanonicalExternal(std::filesystem::path path,
                              std::filesystem::path repoRoot,
                              const char* label,std::string& error) {
    if(path.empty()){error=std::string(label)+" path is empty";return false;}
    std::error_code ec;
    const auto repo=std::filesystem::weakly_canonical(repoRoot,ec);
    if(ec || !repo.is_absolute()){
        error="Cannot canonicalize repository root";return false;
    }
    const auto candidate=std::filesystem::weakly_canonical(path,ec);
    if(ec || !candidate.is_absolute()){
        error=std::string("Cannot canonicalize ")+label+" path";return false;
    }
    if(PathWithin(candidate,repo)){
        error=std::string(label)+" must be OUTSIDE the Tourmaline repository";
        return false;
    }
    error.clear();return true;
}
struct ExternalAssets {
    std::filesystem::path repoRoot;
    // Leave unused paths empty. Validate all supplied paths.
    std::filesystem::path rom;
    std::filesystem::path project;
    std::filesystem::path manifest;
    std::filesystem::path artifacts;
    bool Validate(std::string& error) const {
        if(repoRoot.empty()){error="Repository root is required";return false;}
        if(!rom.empty() && !CanonicalExternal(rom,repoRoot,"ROM",error))
            return false;
        if(!project.empty() && !CanonicalExternal(project,repoRoot,"Asset project",error))
            return false;
        if(!manifest.empty() && !CanonicalExternal(manifest,repoRoot,"Asset manifest",error))
            return false;
        if(!artifacts.empty() && !CanonicalExternal(artifacts,repoRoot,"Generated artifacts",error))
            return false;
        error.clear();return true;
    }
};
} // namespace tour::recomp
