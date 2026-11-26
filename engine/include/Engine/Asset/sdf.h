// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// A mesh-local, versioned signed/unsigned distance volume. This is a
// derived companion to a .geometry artifact, not a replacement for its
// meshlets/LODs. Grid origin is the CENTER of voxel (0,0,0); x is fastest.
// Samples are R16_SNORM: world distance = sample / 32767 * maxDistance.
// If signedDistance is false, the source was not watertight and samples
// are UNSIGNED; consumers must not interpret them as interior occupancy.
#include <cstddef>
#include <cstdint>
#include <glm/common.hpp> // glm::clamp
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <string>
#include <vector>

namespace smvk {
class ThreadPool;
}

namespace tour::asset {

inline constexpr uint32_t kSdfVersion = 4;

// Bit-packed static material (32 bits) carried by the SDF sidecar so
// reflections can sample a small PBR set without the runtime material
// table. Layout (LSB first):
//   [0..4]   base color R  (5 bits -> 0..1)
//   [5..10]  base color G  (6 bits -> 0..1)
//   [11..15] base color B  (5 bits -> 0..1)
//   [16..20] roughness     (5 bits -> 0..1)
//   [21..25] metallic      (5 bits -> 0..1)
//   [26..31] emissive      (6 bits -> 0..1; emissive color = base color)
// The HLSL mirror is static_sdf_trace.hlsli (UnpackStaticSdfMaterial).
inline uint32_t PackSdfMaterial(glm::vec3 baseColor,
                                float roughness,
                                float metallic,
                                float emissive) {
    auto q = [](float v, uint32_t levels) {
        return uint32_t(glm::clamp(v, 0.0f, 1.0f) * float(levels - 1u) + 0.5f);
    };
    const uint32_t r = q(baseColor.r, 32u);
    const uint32_t g = q(baseColor.g, 64u);
    const uint32_t b = q(baseColor.b, 32u);
    const uint32_t ro = q(roughness, 32u);
    const uint32_t me = q(metallic, 32u);
    const uint32_t em = q(emissive, 64u);
    return r | (g << 5) | (b << 11) | (ro << 16) | (me << 21) | (em << 26);
}
inline void UnpackSdfMaterial(uint32_t packed,
                              glm::vec3& baseColor,
                              float& roughness,
                              float& metallic,
                              float& emissive) {
    baseColor = glm::vec3(float(packed & 31u) / 31.0f,
                          float((packed >> 5) & 63u) / 63.0f,
                          float((packed >> 11) & 31u) / 31.0f);
    roughness = float((packed >> 16) & 31u) / 31.0f;
    metallic = float((packed >> 21) & 31u) / 31.0f;
    emissive = float((packed >> 26) & 63u) / 63.0f;
}

// RGB565 for the per-voxel texture-baked base color (linear, see
// BakeMeshSdfColors). HLSL mirror: UnpackStaticSdfRgb565.
inline uint16_t PackRgb565(glm::vec3 linear) {
    auto q = [](float v, uint32_t levels) {
        return uint32_t(glm::clamp(v, 0.0f, 1.0f) * float(levels - 1u) + 0.5f);
    };
    return uint16_t(q(linear.r, 32u) | (q(linear.g, 64u) << 5) | (q(linear.b, 32u) << 11));
}
inline glm::vec3 UnpackRgb565(uint16_t c) {
    return glm::vec3(
        float(c & 31u) / 31.0f, float((c >> 5) & 63u) / 63.0f, float((c >> 11) & 31u) / 31.0f);
}

struct SdfVolume {
    uint32_t resolution = 0; // cubic grid, x fastest
    glm::vec3 origin{0.0f};  // center of first voxel, mesh-local units
    float voxelSize = 0.0f;
    float maxDistance = 0.0f; // saturating narrow band, mesh-local units
    bool signedDistance = false;
    std::vector<int16_t> samples;
    // Optional per-voxel texture-baked base color (RGB565, parallel to
    // samples; empty when the sidecar was baked without color).
    std::vector<uint16_t> colors;
};

// Input for the optional per-voxel texture bake (BakeMeshSdfColors).
struct SdfColorInput {
    const glm::vec2* uvs = nullptr; // per-vertex (parallel to positions)
    const uint8_t* texel = nullptr; // RGBA8, texWidth * texHeight, linear
    uint32_t texWidth = 0;
    uint32_t texHeight = 0;
};

// Adaptive octree distance field: a sparse, variable-depth octree refined
// only near the surface. The finest cell (the "brick") is size * 2^(1-maxDepth);
// empty space and flat slabs stay coarse, so the same finest resolution as a
// uniform grid costs a fraction of the memory. Leaves carry a signed
// distance (int16, normalized by maxDistance) + an optional texture-baked
// color (RGB565). Linear flat arrays for the GPU trace.
struct SdfOctree {
    glm::vec3 origin{0.0f}; // root AABB min corner (mesh-local)
    float size = 0.0f;      // root half-extent (world units)
    float maxDistance = 0.0f;
    uint32_t maxDepth = 6;         // finest brick = size * 2^(1-maxDepth)
    std::vector<uint32_t> nodes;   // [31]=leaf; [30:0]=childBase (internal) or leafIndex (leaf)
    std::vector<int32_t> children; // 8 per internal node, -1 = empty
    std::vector<uint32_t> leaves;  // [31:16]=dist int16, [15:0]=color RGB565
};

struct SdfMesh {
    std::string fragment; // .geometry URI fragment, e.g. "mesh.0/prim.0"
    SdfVolume volume;
    // Adaptive octree field (preferred when present). Empty (nodes empty)
    // when the sidecar was baked with the legacy uniform grid only.
    SdfOctree octree;
    // Index into SdfAsset::materials (the shared bit-packed palette).
    // Constant per primitive — the color is the material's flat base-color
    // factor, not a texture bake (see SdfMaterial).
    uint32_t materialIndex = 0;
};

struct SdfAsset {
    std::vector<SdfMesh> meshes;
    // Shared bit-packed material palette (PackSdfMaterial); meshes index
    // into it. Entry 0 is a neutral grey fallback for unbound materials.
    std::vector<uint32_t> materials;
};

struct SdfBakeOptions {
    uint32_t resolution = 24; // [8,128], including padding on all sides
    uint32_t padding = 2;     // voxels on each edge, >=1
    uint32_t bandVoxels = 8;  // max distance encoded, >=1
    // Octree depth (the adaptive field's finest "brick" = extent / 2^maxDepth).
    // Lower = coarser bricks (less memory, blockier); higher = finer. The
    // uniform-grid path treats resolution = 2^maxDepth.
    uint32_t maxDepth = 6;
    // Experimental meshoptimizer 1.3 remesher: reconstructs topology first.
    // OFF by default: it can close gaps/erase thin wires and change signs.
    bool voxelRemesh = false;
    int remeshResolution = 64; // meshopt_remesh accepts [4,256]
};

// Indexed triangles, mesh-local positions, CPU-only. On open or
// nonmanifold meshes the bake succeeds but returns unsigned distances.
// Rejects empty/degenerate input and impossible settings. Raw pointer +
// count at the boundary (the caller owns the memory; {nullptr, 0} is a
// valid empty input that fails validation loudly).
bool BakeMeshSdf(const glm::vec3* positions,
                 size_t positionCount,
                 const uint32_t* indices,
                 size_t indexCount,
                 const SdfBakeOptions& options,
                 SdfVolume& out,
                 std::string& err,
                 smvk::ThreadPool* pool = nullptr);

// Texture bake: fill `out.colors` (RGB565, parallel to out.samples) with
// the base-color texture sampled at each voxel's closest surface point.
// Consumes the ALREADY-BAKED `volume` (the caller runs BakeMeshSdf first),
// re-searches the triangles for the closest point + barycentric, and
// bilinearly samples `color.texel` at the interpolated UV. Voxels outside
// the narrow band keep 0. Returns false (with err) on invalid input.
bool BakeMeshSdfColors(const glm::vec3* positions,
                       size_t positionCount,
                       const glm::vec2* uvs,
                       const uint32_t* indices,
                       size_t indexCount,
                       const SdfColorInput& color,
                       const SdfVolume& volume,
                       std::vector<uint16_t>& outColors,
                       std::string& err,
                       smvk::ThreadPool* pool = nullptr);

// Adaptive octree bake: builds a sparse, surface-refined octree (see
// SdfOctree) instead of a uniform grid. When `color` is non-null, leaves
// also carry the texture-baked base color (sampled at the leaf's closest
// surface point). `options.maxDepth` is the finest brick depth.
bool BakeMeshSdfOctree(const glm::vec3* positions,
                       size_t positionCount,
                       const uint32_t* indices,
                       size_t indexCount,
                       const SdfColorInput* color,
                       const SdfBakeOptions& options,
                       SdfOctree& out,
                       std::string& err,
                       smvk::ThreadPool* pool = nullptr);

// TSDF v1, little-endian, bounds-checked, no struct padding or host
// endianness dependencies. A single sidecar groups a .geometry family's
// primitive volumes by stable URI fragment. Parse is transactional.
std::vector<uint8_t> WriteSdfAsset(const SdfAsset& asset);
bool ParseSdfAsset(const uint8_t* bytes, size_t byteCount, SdfAsset& out, std::string& err);

} // namespace tour::asset
