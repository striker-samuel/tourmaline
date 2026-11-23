// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// pak_append: copy every blob of one PakAssetKind from a source pak into a
// destination pak, using the append-grow path (AppendPakBlobs keeps the
// image valid at every step; duplicate URIs replace their TOC entry).
// The typical use is merging a separately-cooked terrain pak (or a GI /
// cubemap / content pak) into the main world pak without re-cooking it.
//
// After the append the destination's content-addressed name is stale (the
// filename embeds the fnv1a64 digest of its bytes). The tool re-hashes the
// file and prints the new name, or renames in place with --rename:
//
//   pak_append --src terrain/island_terrain.pak --dst island-xxxx.pak --kind Terrain --rename

#include <Engine/Asset/pak.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Opts {
    std::string src;
    std::string dst;
    smvk::PakAssetKind kind = smvk::PakAssetKind::Terrain;
    bool rename = false;
};

bool ParseKind(const char* name, smvk::PakAssetKind& out) {
    struct Pair {
        const char* name;
        smvk::PakAssetKind kind;
    };
    static constexpr Pair kKinds[] = {
        {"Terrain", smvk::PakAssetKind::Terrain},
        {"Mesh", smvk::PakAssetKind::Mesh},
        {"Material", smvk::PakAssetKind::Material},
        {"Texture", smvk::PakAssetKind::Texture},
        {"Light", smvk::PakAssetKind::Light},
        {"Gi", smvk::PakAssetKind::Gi},
        {"Camera", smvk::PakAssetKind::Camera},
        {"Decal", smvk::PakAssetKind::Decal},
        {"Scene", smvk::PakAssetKind::Scene},
    };
    for (const Pair& p : kKinds)
        if (std::strcmp(name, p.name) == 0) {
            out = p.kind;
            return true;
        }
    return false;
}

// Streaming FNV-1a 64 with the length finish, byte-for-byte the same as
// smvk::cache::HashBytes (artifact_cache) but over a file (never loads the
// whole multi-GB pak into memory).
std::string HashFile(const std::string& path, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "cannot open " + path + " for rehash";
        return {};
    }
    uint64_t h = 1469598103934665603ull;
    uint64_t total = 0;
    // Heap buffer, not a stack array: an 8 MiB automatic array overflows the
    // 1 MiB default thread stack (0xC00000FD on the first rehash call).
    std::vector<char> buf(8u * 1024 * 1024);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            h ^= static_cast<uint8_t>(buf[static_cast<size_t>(i)]);
            h *= 1099511628211ull;
        }
        total += static_cast<uint64_t>(n);
    }
    if (!in.eof()) {
        err = "read error while rehashing " + path;
        return {};
    }
    h ^= total * 0x9E3779B97F4A7C15ull;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 32;
    char hex[32];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
    return hex;
}

// Rebuild a content-addressed name: <stem>-<digest>.pak. A trailing
// -<16hex> digest in the current name is replaced, otherwise one is appended.
std::string AddressedName(const std::string& path, const std::string& digest) {
    namespace fs = std::filesystem;
    const fs::path p(path);
    std::string stem = p.stem().generic_string(); // drops ".pak"
    if (stem.size() > 17 && stem[stem.size() - 17] == '-') {
        const std::string tail = stem.substr(stem.size() - 16);
        bool hex = true;
        for (char c : tail)
            hex = hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        if (hex)
            stem = stem.substr(0, stem.size() - 17);
    }
    const fs::path dir = p.parent_path();
    const std::string name = stem + "-" + digest + ".pak";
    return dir.empty() ? name : (dir / name).generic_string();
}

} // namespace

int main(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--src") == 0 && i + 1 < argc) {
            o.src = argv[++i];
        } else if (std::strcmp(a, "--dst") == 0 && i + 1 < argc) {
            o.dst = argv[++i];
        } else if (std::strcmp(a, "--kind") == 0 && i + 1 < argc) {
            if (!ParseKind(argv[++i], o.kind)) {
                std::fprintf(stderr, "pak_append: unknown --kind %s\n", argv[i]);
                return 2;
            }
        } else if (std::strcmp(a, "--rename") == 0) {
            o.rename = true;
        } else {
            std::fprintf(stderr,
                         "usage: pak_append --src <src.pak> --dst <dst.pak> "
                         "[--kind Terrain] [--rename]\n");
            return 2;
        }
    }
    if (o.src.empty() || o.dst.empty()) {
        std::fprintf(stderr,
                     "usage: pak_append --src <src.pak> --dst <dst.pak> "
                     "[--kind Terrain] [--rename]\n");
        return 2;
    }

    std::string openErr;
    std::optional<smvk::PakReader> reader = smvk::PakReader::OpenFile(o.src, openErr);
    if (!reader) {
        std::fprintf(stderr, "pak_append: open %s failed: %s\n", o.src.c_str(), openErr.c_str());
        return 1;
    }

    const std::vector<const smvk::PakTocEntry*> entries = reader->BlobsOfKind(o.kind);
    std::vector<std::pair<std::string, std::span<const uint8_t>>> blobs;
    blobs.reserve(entries.size());
    for (const smvk::PakTocEntry* e : entries)
        blobs.emplace_back(e->path, reader->Payload(*e));

    std::vector<smvk::PakAppendInfo> info;
    std::string err;
    if (!smvk::AppendPakBlobs(o.dst, o.kind, blobs, info, err)) {
        std::fprintf(stderr, "pak_append: append failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("pak_append: appended %zu blob(s) -> %s\n", blobs.size(), o.dst.c_str());

    const std::string digest = HashFile(o.dst, err);
    if (digest.empty()) {
        std::fprintf(stderr, "pak_append: rehash failed: %s\n", err.c_str());
        return 1;
    }
    const std::string newName = AddressedName(o.dst, digest);
    if (o.rename) {
        std::error_code ec;
        std::filesystem::rename(o.dst, newName, ec);
        if (ec) {
            std::fprintf(stderr, "pak_append: rename failed: %s\n", ec.message().c_str());
            return 1;
        }
        std::printf("pak_append: renamed -> %s\n", newName.c_str());
    } else {
        std::printf("pak_append: new digest %s (rename to %s, or re-run with --rename)\n",
                    digest.c_str(),
                    newName.c_str());
    }
    return 0;
}
