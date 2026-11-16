// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <gltf_import/source_scene.h>
#include <gltf_import/asset_export.h>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

static void Require(bool condition) {
    if (!condition) {
        std::cerr << "SourceScene validation test failed\n";
        std::exit(1);
    }
}

int main() {
    using namespace smvk::source;
    Scene s;
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
    Scene native;
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
    fs::remove_all(dir, ec);
    return 0;
}
