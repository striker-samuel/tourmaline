// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// pak_append: copy every blob of one PakAssetKind from a source pak into a
// destination pak, using the append-grow path (AppendPakBlobs keeps the
// image valid at every step; duplicate URIs replace their TOC entry).
// The typical use is merging a separately-cooked terrain pak (or a GI /
// cubemap / content pak) into the main world pak without re-cooking it.
//
//   pak_append --src terrain/island_terrain.pak --dst island.pak --kind Terrain

#include <Engine/Asset/pak.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Opts {
    std::string src;
    std::string dst;
    smvk::PakAssetKind kind = smvk::PakAssetKind::Terrain;
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
        } else {
            std::fprintf(stderr,
                         "usage: pak_append --src <src.pak> --dst <dst.pak> "
                         "[--kind Terrain]\n");
            return 2;
        }
    }
    if (o.src.empty() || o.dst.empty()) {
        std::fprintf(stderr,
                     "usage: pak_append --src <src.pak> --dst <dst.pak> [--kind Terrain]\n");
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
    return 0;
}
