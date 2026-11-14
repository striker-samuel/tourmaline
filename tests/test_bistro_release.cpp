// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// bistro_release_tests: the Bistro demo-content release regression gate
// (CPU-only, skip-if-absent).
//
// The bug this pins: a material texture cross-wiring where a subset of
// bistro materials resolved their baseColor (and other) texture slots to
// the WRONG texture — most visibly the bakery shop sign sampling the
// wicker basket's diffuse (its masked alpha test cut out the wrong
// shape). The cooked pak serializes the correct URIs, but the regression
// gate proves the released artifact stays self-consistent: every material
// texture reference resolves to an existing, unique texture blob, and the
// bakery sign's baseColor is the bakery diffuse (never the wicker basket).
//
// Dependency: a cooked bistro pak (bistro_static-<hash>.pak). Bistro is
// a recreate-from-source demo (see releases/bistro/README.md), so the pak
// is produced by `tools/recreate_bistro.ps1` / `.sh` (output into
// releases/bistro/build) or staged into content/paks. The test SKIPS
// (passes) when the pak is absent so offline / hosted CI stays green.
//
// GPU-free on purpose: it reads the pak TOC + material/texture blobs with
// the file-backed PakReader (positional page reads, no whole-image load)
// and never deserializes into a Cluster (that needs a Vulkan device).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <Engine/Asset/pak.h>
#include <Engine/Renderer/mesh.h>

static int g_failures = 0;
static int g_checks = 0;

static void Check(bool cond, const char* name) {
    ++g_checks;
    if (cond) {
        std::cout << "PASS " << name << "\n";
    } else {
        ++g_failures;
        std::cout << "FAIL " << name << "\n";
    }
}

// Minimal little-endian blob cursor (the material/texture envelope field
// reader — mirrors cluster_serializer.cpp's Cursor for the two fields we
// need: u32 length-prefixed strings and the fixed-size MaterialValues gap).
struct Cur {
    const uint8_t* p;
    size_t n;
    size_t off = 0;
    bool ok = true;
    uint32_t u32() {
        uint32_t v = 0;
        if (off + 4 > n) {
            ok = false;
            return 0;
        }
        std::memcpy(&v, p + off, 4);
        off += 4;
        return v;
    }
    void skip(size_t bytes) {
        if (off + bytes > n)
            ok = false;
        else
            off += bytes;
    }
    std::string str() {
        const uint32_t len = u32();
        if (!ok || off + len > n) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(p + off), len);
        off += len;
        return s;
    }
};

// Locates the bistro release pak by prefix (bistro_static.tapp's
// content-addressed pak is bistro_static-<hash>.pak; a stable-named
// bistro_static.pak from a manual deploy also matches). Returns "" when
// absent (the skip path).
static std::string FindBistroPak() {
    const char* dirs[] = {"content/paks", "releases/paks", "releases/bistro/build"};
    for (const char* dir : dirs) {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec))
            continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec || !entry.is_regular_file())
                continue;
            const std::string name = entry.path().filename().string();
            if (name.rfind("bistro_static", 0) == 0 && name.size() >= 4 &&
                name.compare(name.size() - 4, 4, ".pak") == 0) {
                return entry.path().string();
            }
        }
    }
    return "";
}

static void TestBistroPak(const std::string& pakPath) {
    std::string err;
    auto reader = smvk::PakReader::OpenFile(pakPath, err);
    Check(reader.has_value() && err.empty(), "bistro.pak-opens");
    if (!reader) {
        std::cerr << "  open err: " << err << "\n";
        return;
    }

    const auto materials = reader->BlobsOfKind(smvk::PakAssetKind::Material);
    const auto textures = reader->BlobsOfKind(smvk::PakAssetKind::Texture);
    Check(!materials.empty() && !textures.empty(), "bistro.pak-carries-materials-and-textures");
    if (materials.empty() || textures.empty())
        return;

    // Texture blobs: the first field is the texture's VFS URI. Collect the
    // set and reject duplicates (a duplicate URI is a cross-wiring vector).
    std::unordered_set<std::string> textureUris;
    textureUris.reserve(textures.size());
    bool unique = true;
    for (const smvk::PakTocEntry* e : textures) {
        const auto payload = reader->Payload(*e);
        Cur c{payload.data(), payload.size()};
        const std::string uri = c.str();
        if (!c.ok || uri.empty() || !textureUris.insert(uri).second)
            unique = false;
    }
    Check(unique && textureUris.size() == textures.size(), "bistro.textures-unique");

    // Material blobs: uri | MaterialValues (fixed) | shading/alpha/cutoff/ds
    // | texUris[0..4] | ... The baseColor is texUris[0]. Every non-empty
    // reference must resolve to a real texture (no dangling cross-pak ref).
    bool refsResolve = true;
    std::string bakeryMaterial;
    std::string bakeryBase;
    std::string wickerMaterial;
    std::string wickerBase;
    for (const smvk::PakTocEntry* e : materials) {
        const auto payload = reader->Payload(*e);
        Cur c{payload.data(), payload.size()};
        const std::string matUri = c.str();
        c.skip(sizeof(smvk::MaterialValues));
        c.skip(4u * 4u); // shading, alpha, cutoff, ds
        std::string texUris[5];
        for (int k = 0; k < 5; ++k)
            texUris[k] = c.str();
        if (!c.ok) {
            refsResolve = false;
            continue;
        }
        for (int k = 0; k < 5; ++k) {
            if (!texUris[k].empty() && textureUris.find(texUris[k]) == textureUris.end())
                refsResolve = false;
        }
        // The two canary materials (the exact cross-wiring symptom): the
        // bakery sign and the wicker basket. Track their baseColor URIs.
        if (texUris[0].find("bakery") != std::string::npos ||
            texUris[0].find("shopsign") != std::string::npos) {
            if (bakeryMaterial.empty()) {
                bakeryMaterial = matUri;
                bakeryBase = texUris[0];
            }
        }
        if (texUris[0].find("wickerbasket") != std::string::npos) {
            if (wickerMaterial.empty()) {
                wickerMaterial = matUri;
                wickerBase = texUris[0];
            }
        }
    }
    Check(refsResolve, "bistro.material-refs-resolve");

    // The regression itself: the bakery sign's baseColor is the bakery
    // diffuse, NOT the wicker basket's; the two are distinct materials.
    const bool bakeryOk =
        bakeryBase == "objects/props/street/paris_shopsign_01/paris_shopsign_bakery_diff.ktx2";
    Check(!bakeryMaterial.empty() && bakeryOk, "bistro.bakery-basecolor-correct");
    const bool wickerOk =
        wickerBase == "objects/props/bistro/paris_wickerbasket_01/paris_wickerbasket_01_diff.ktx2";
    Check(!wickerMaterial.empty() && wickerOk, "bistro.wickerbasket-basecolor-correct");
    Check(!bakeryMaterial.empty() && !wickerMaterial.empty() && bakeryMaterial != wickerMaterial,
          "bistro.bakery-not-crosswired-to-wickerbasket");

    std::cout << "  pak: " << pakPath << " (materials=" << materials.size()
              << " textures=" << textures.size() << ")\n";
}

int main() {
    std::cout << std::unitbuf;
    const std::string pak = FindBistroPak();
    if (pak.empty()) {
        std::cout << "SKIP bistro_release_tests: no bistro_static*.pak found under "
                     "content/paks, releases/paks or releases/bistro/build (run "
                     "tools/recreate_bistro.ps1 to recreate it).\n";
        return 0;
    }
    TestBistroPak(pak);
    std::cout << "bistro_release_tests: " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
