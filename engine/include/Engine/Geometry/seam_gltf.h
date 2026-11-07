// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// glTF 2.0 source exporter for baked ribbons / skirts. Carries the fade
// coverage through COLOR_0.a (OBJ vertex-color extensions are not portable).
// The existing glTF importer cooks the result into standard engine geometry.
#include <Engine/Geometry/intersection_curves.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>
#include <vector>

namespace smvk::geometry {
inline bool SaveRibbonGltf(const std::string& path, const RibbonMesh& mesh, std::string& error) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        error = "No ribbon triangles to export";
        return false;
    }
    if (mesh.vertices.size() > uint64_t(std::numeric_limits<uint32_t>::max())) {
        error = "Ribbon exceeds 32-bit glTF vertex count";
        return false;
    }
    const std::filesystem::path jsonPath(path);
    if (jsonPath.extension() != ".gltf") {
        error = "Output path must end in .gltf";
        return false;
    }
    const std::filesystem::path binPath =
        jsonPath.parent_path() / (jsonPath.stem().string() + ".bin");
    if (binPath.filename().string().find('"') != std::string::npos ||
        binPath.filename().string().find('\\') != std::string::npos) {
        error = "Unsupported output filename for glTF URI";
        return false;
    }
    // Planar SoA streams; four-byte alignment is guaranteed by float/u32.
    struct View {
        uint64_t offset = 0, bytes = 0;
    };
    std::array<View, 5> views{};
    std::vector<uint8_t> binary;
    auto append = [&](const void* src, size_t size) {
        const auto* p = static_cast<const uint8_t*>(src);
        binary.insert(binary.end(), p, p + size);
    };
    auto writeFloat = [&](float value) { append(&value, sizeof(value)); };
    glm::vec3 mn(std::numeric_limits<float>::max());
    glm::vec3 mx(std::numeric_limits<float>::lowest());
    views[0].offset = binary.size();
    for (const auto& v : mesh.vertices) {
        mn = glm::min(mn, v.position);
        mx = glm::max(mx, v.position);
        writeFloat(v.position.x);
        writeFloat(v.position.y);
        writeFloat(v.position.z);
    }
    views[0].bytes = binary.size() - views[0].offset;
    views[1].offset = binary.size();
    for (const auto& v : mesh.vertices) {
        writeFloat(v.normal.x);
        writeFloat(v.normal.y);
        writeFloat(v.normal.z);
    }
    views[1].bytes = binary.size() - views[1].offset;
    views[2].offset = binary.size();
    for (const auto& v : mesh.vertices) {
        writeFloat(v.uv.x);
        writeFloat(v.uv.y);
    }
    views[2].bytes = binary.size() - views[2].offset;
    views[3].offset = binary.size();
    for (const auto& v : mesh.vertices) {
        // Vertex RGB remains white (no darkening); alpha is edge coverage.
        // Imported glTF material uses alphaMode BLEND for this mask.
        writeFloat(1.0f);
        writeFloat(1.0f);
        writeFloat(1.0f);
        writeFloat(std::clamp(v.coverage, 0.0f, 1.0f));
    }
    views[3].bytes = binary.size() - views[3].offset;
    views[4].offset = binary.size();
    for (uint32_t index : mesh.indices) {
        if (index >= mesh.vertices.size()) {
            error = "Out-of-range ribbon index";
            return false;
        }
        append(&index, sizeof(index));
    }
    views[4].bytes = binary.size() - views[4].offset;
    // Write binary first, so failed binary output never publishes a
    // glTF manifest that names a missing asset.
    std::ofstream bout(binPath, std::ios::binary | std::ios::trunc);
    if (!bout) {
        error = "Cannot write " + binPath.string();
        return false;
    }
    bout.write(reinterpret_cast<const char*>(binary.data()), std::streamsize(binary.size()));
    bout.close();
    if (!bout) {
        error = "Failed writing " + binPath.string();
        return false;
    }

    std::ofstream out(jsonPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Cannot write " + path;
        return false;
    }
    out << std::setprecision(9);
    out << "{\n\"asset\":{\"version\":\"2.0\","
           "\"generator\":\"Tourmaline seam baker\"},\n";
    out << "\"buffers\":[{\"uri\":\"" << binPath.filename().string()
        << "\",\"byteLength\":" << binary.size() << "}],\n";
    out << "\"bufferViews\":[";
    for (size_t i = 0; i < views.size(); ++i) {
        if (i)
            out << ',';
        out << "{\"buffer\":0,\"byteOffset\":" << views[i].offset
            << ",\"byteLength\":" << views[i].bytes << ",\"target\":" << (i == 4 ? 34963 : 34962)
            << "}";
    }
    out << "],\n\"accessors\":[";
    const char* types[] = {"VEC3", "VEC3", "VEC2", "VEC4", "SCALAR"};
    for (size_t i = 0; i < views.size(); ++i) {
        if (i)
            out << ',';
        out << "{\"bufferView\":" << i << ",\"componentType\":" << (i == 4 ? 5125 : 5126)
            << ",\"count\":" << (i == 4 ? mesh.indices.size() : mesh.vertices.size())
            << ",\"type\":\"" << types[i] << "\"";
        if (i == 0) {
            out << ",\"min\":[" << mn.x << ',' << mn.y << ',' << mn.z << "]"
                << ",\"max\":[" << mx.x << ',' << mx.y << ',' << mx.z << "]";
        }
        out << '}';
    }
    out << "],\n";
    out << "\"materials\":[{\"name\":\"Seam ribbon / skirt\","
           "\"doubleSided\":true,\"alphaMode\":\"BLEND\","
           "\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1],"
           "\"metallicFactor\":0,\"roughnessFactor\":1}}],\n";
    out << "\"meshes\":[{\"name\":\"SeamBake\",\"primitives\":[{"
           "\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
           "\"TEXCOORD_0\":2,\"COLOR_0\":3},"
           "\"indices\":4,\"material\":0}]}],\n";
    out << "\"nodes\":[{\"mesh\":0}],"
           "\"scenes\":[{\"nodes\":[0]}],\"scene\":0}\n";
    out.close();
    if (!out) {
        error = "Failed writing " + path;
        return false;
    }
    return true;
}
} // namespace smvk::geometry
