// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <source_import/asset_export.h>
#include <source_import/source_scene.h>
#include <string>

static void Require(bool condition) {
    if (!condition) {
        std::cerr << "SourceScene validation test failed\n";
        std::exit(1);
    }
}

int main() {
    using namespace smvk::source;
    smvk::source::Scene s;
    s.nodes.emplace_back();
    s.roots.push_back(0);
    s.meshes.emplace_back();
    s.nodes[0].mesh = 0;
    auto& p = s.meshes[0].primitives.emplace_back();
    p.vertices.resize(3);
    p.indices = {0, 1, 2};
    std::string error;
    Require(Validate(s, error) && error.empty());
    p.indices[2] = 3;
    Require(!Validate(s, error) && !error.empty());
    p.indices[2] = 2;
    p.vertices[0].position.x = std::numeric_limits<float>::infinity();
    Require(!Validate(s, error));
    p.vertices[0].position.x = 0;
    s.nodes[0].children.push_back(0);
    s.nodes[0].parent = 0;
    Require(!Validate(s, error));
    s.nodes[0].children.clear();
    s.nodes[0].parent = InvalidIndex;
    Require(Validate(s, error));
    s.nodes.emplace_back();
    Require(!Validate(s, error)); // dangling/unreachable node

    // The derivation API must accept a fully native source scene. No
    // tinygltf::Model, synthetic accessors, OBJ or USD adapter involved.
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "tourmaline_native_source_scene";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    Require(!ec);
    const fs::path input = dir / "native.src";
    {
        std::ofstream file(input, std::ios::binary);
        file << "native test scene";
        Require(bool(file));
    }
    smvk::source::Scene native;
    native.roots = {0};
    native.activeRoots = {0};
    native.nodes.emplace_back();
    native.nodes[0].name = "Triangle";
    native.nodes[0].mesh = 0;
    native.meshes.emplace_back();
    native.materials.emplace_back();
    auto& tri = native.meshes[0].primitives.emplace_back();
    tri.vertices.resize(3);
    tri.vertices[0].position = {0.0f, 0.0f, 0.0f};
    tri.vertices[1].position = {1.0f, 0.0f, 0.0f};
    tri.vertices[2].position = {0.0f, 1.0f, 0.0f};
    tri.indices = {0, 1, 2};
    tri.aabbMin = {0.0f, 0.0f, 0.0f};
    tri.aabbMax = {1.0f, 1.0f, 0.0f};
    tri.material = 0;
    Require(Validate(native, error));
    smvk::assets::AssetManifest manifest;
    smvk::assets::DeriveConfig config{};
    smvk::assets::DeriveStats stats{};
    std::string derivationError;
    Require(smvk::assets::DeriveNativeAssets(
        native, input, "", nullptr, config, manifest, &stats, &derivationError));
    smvk::assets::MeshAsset derived;
    std::string loadError;
    Require(smvk::assets::LoadMeshAsset(dir / "native.geometry", derived, loadError));
    Require(derived.geometry.size() == 1);
    Require(derived.geometry[0].indices.size() == 3);

    // STRICT OVERFLOW: the bounded static-mesh pool (Pool::kMaxSize = 8192)
    // must partition past its bound — never silently truncate, never overflow.
    // 8193 single-triangle meshes (one upload each) derive into 3 parts.
    {
        const fs::path odir = fs::temp_directory_path(ec) / "tourmaline_partition_derive";
        fs::remove_all(odir, ec);
        fs::create_directories(odir, ec);
        const fs::path oinput = odir / "overflow.src";
        {
            std::ofstream file(oinput, std::ios::binary);
            file << "partition derive test";
        }
        smvk::source::Scene big;
        big.roots.reserve(8193);
        big.activeRoots.reserve(8193);
        big.nodes.reserve(8193);
        big.meshes.resize(8193);
        for (int i = 0; i < 8193; ++i) {
            smvk::source::Node n;
            n.name = "m" + std::to_string(i);
            n.mesh = i;
            n.parent = InvalidIndex;
            big.nodes.push_back(n);
            big.roots.push_back(uint32_t(i));
            big.activeRoots.push_back(uint32_t(i));
            auto& prim = big.meshes[size_t(i)].primitives.emplace_back();
            prim.vertices.resize(3);
            prim.vertices[0].position = {float(i), 0.0f, 0.0f};
            prim.vertices[1].position = {float(i) + 1.0f, 0.0f, 0.0f};
            prim.vertices[2].position = {float(i), 1.0f, 0.0f};
            prim.indices = {0, 1, 2};
            prim.aabbMin = {float(i), 0.0f, 0.0f};
            prim.aabbMax = {float(i) + 1.0f, 1.0f, 0.0f};
        }
        Require(Validate(big, error));
        smvk::assets::AssetManifest om;
        smvk::assets::DeriveStats os{};
        std::string oerr;
        Require(smvk::assets::DeriveNativeAssets(big, oinput, "", nullptr, config, om, &os, &oerr));
        Require(os.geometryBlobs == 8193);
        Require(std::filesystem::exists(odir / "overflow_part0.geometry"));
        Require(std::filesystem::exists(odir / "overflow_part1.geometry"));
        Require(std::filesystem::exists(odir / "overflow_part2.geometry"));
        fs::remove_all(odir, ec);
    }

    // PARTITION: 8193 meshes at a 4096-record planning limit -> 3 parts, no
    // mesh lost, no mesh duplicated, every instance preserved (P1 + P3).
    {
        smvk::source::Scene src;
        src.roots.reserve(8193);
        src.activeRoots.reserve(8193);
        src.nodes.reserve(8193);
        src.meshes.resize(8193);
        for (int i = 0; i < 8193; ++i) {
            smvk::source::Node n;
            n.name = "m" + std::to_string(i);
            n.mesh = i;
            n.parent = InvalidIndex;
            src.nodes.push_back(n);
            src.roots.push_back(uint32_t(i));
            src.activeRoots.push_back(uint32_t(i));
            auto& prim = src.meshes[size_t(i)].primitives.emplace_back();
            prim.vertices.resize(3);
            prim.vertices[0].position = {float(i), 0.0f, 0.0f};
            prim.vertices[1].position = {float(i) + 1.0f, 0.0f, 0.0f};
            prim.vertices[2].position = {float(i), 1.0f, 0.0f};
            prim.indices = {0, 1, 2};
            prim.aabbMin = {float(i), 0.0f, 0.0f};
            prim.aabbMax = {float(i) + 1.0f, 1.0f, 0.0f};
        }
        Require(Validate(src, error));
        const smvk::source::ScenePartition part = smvk::source::PartitionScene(src, 4096, 256.0f);
        Require(part.parts.size() == 3);
        size_t totalMeshes = 0, totalInstances = 0;
        for (const auto& p : part.parts) {
            Require(p.meshes.size() <= 4096);
            Require(Validate(p, error));
            totalMeshes += p.meshes.size();
            totalInstances += p.nodes.size() - 1; // minus the synthetic root
        }
        Require(totalMeshes == 8193);
        Require(totalInstances == 8193);
    }

    // PARTITIONED-FAMILY MANIFEST helpers (P1): a manifest with geometry.<part>
    // rows is a partitioned family; GeometryPartFiles returns them ordered.
    {
        smvk::assets::AssetManifest m;
        m.guid = "0123456789abcdef0123456789abcdef";
        // Single geometry= row -> NOT partitioned, no part files.
        smvk::assets::AssetEntry single{};
        single.kind = smvk::assets::AssetKind::Geometry;
        single.sourceIndex = -1;
        single.file = "a.geometry";
        m.entries.push_back(single);
        Require(!smvk::assets::IsPartitionedFamily(m));
        Require(smvk::assets::GeometryPartFiles(m).empty());
        // Two geometry.<part> rows -> partitioned, ordered files.
        smvk::assets::AssetEntry p0{};
        p0.kind = smvk::assets::AssetKind::Geometry;
        p0.sourceIndex = 0;
        p0.file = "a_part0.geometry";
        m.entries.push_back(p0);
        smvk::assets::AssetEntry p1{};
        p1.kind = smvk::assets::AssetKind::Geometry;
        p1.sourceIndex = 1;
        p1.file = "a_part1.geometry";
        m.entries.push_back(p1);
        Require(smvk::assets::IsPartitionedFamily(m));
        const std::vector<std::string> parts = smvk::assets::GeometryPartFiles(m);
        Require(parts.size() == 2 && parts[0] == "a_part0.geometry" &&
                parts[1] == "a_part1.geometry");
    }

    fs::remove_all(dir, ec);
    return 0;
}