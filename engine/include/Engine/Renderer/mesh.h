// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <Engine/Asset/meshlet.h>
#include <Engine/Renderer/vertex_pack.h>
#include <Engine/smvk.h>
#include <glm/glm.hpp>
// The static vertex/index monoblock heap (the shared, backend-neutral
// allocation layer AllocateStaticVertexFmt creates + the draw paths bind).
#include <Engine/Renderer/static_vertex_heap.h>

// (namespace/RHI boundary audit, todo 8.4) the material DATA MODEL trio
// below (MaterialValues + ShadingModel + AlphaMode) is ENGINE-OWNED —
// material/shading POLICY, not an RHI primitive — and lives in tour;
// the smvk using-declarations after the block keep the unqualified
// spellings inside the rest of mesh.h and every smvk-scope consumer
// resolving (retire as call sites migrate).

namespace tour {

// GPU material buffer layout – must match HLSL common.hlsli::MaterialValues.
// Keep 16-byte alignment for StructuredBuffer / buffer_device_address.
struct alignas(16) MaterialValues {

    // Legacy sky-ambient RGB factor; ambient.w stores thin-surface
    // backlighting strength (0 = disabled). This unused lane preserves
    // the 128-byte GPU material/pak layout. Paks round-trip verbatim,
    // so paks cooked with an old default keep their baked value.
    glm::vec4 ambient{0.f};   // 0 – see cluster.cpp
    glm::vec4 diffuse{1.0f};  // 16 – baseColorFactor (alias: diffuse)
    glm::ivec4 textures{-1};  // 32 – x=baseColor, y=metallicRoughness, z=normal, w=occlusion
    glm::ivec4 textures2{-1}; // 48 – x=emissive, y=cavity, z=cavity strength (0..1000, -1=1),
                              // w=packed channel selectors
    glm::vec4 params{0.5f,
                     1.0f,
                     1.0f,
                     0.0f}; // 64 – x=alphaCutoff, y=metallic, z=roughness, w=shadingModel
    glm::vec4 params2{1.0f,
                      1.0f,
                      1.0f,
                      0.0f}; // 80 – x=occlusionStrength, y=normalScale, z=emissiveStrength,
                             // w=alphaMode (0=Opaque,1=Mask,2=Blend)
    glm::vec4 emissiveFactor{0.0f,
                             0.0f,
                             0.0f,
                             0.0f}; // 96 – xyz=emissiveFactor, w=material flags (kMaterialFlag*)
    // Layered materials (vertex-color blending): x/y/z hold the
    // cluster-local material pool indices of layers 1..3 (the material
    // itself is layer 0; -1 = no layer). w selects the mode: 0 = legacy
    // (vertex color is a plain albedo tint), 1..4 = layered with that
    // many active layers (weights from the vertex color RGBA channels,
    // normalized in the shader). Cluster-local ids are remapped to
    // world material ids by World::ResolveAndFixup_clusters, which also
    // appends slots for layer materials no node references directly.
    glm::ivec4 layerMaterials{-1, -1, -1, 0}; // 112
};
static_assert(sizeof(MaterialValues) % 16 == 0);
static_assert(sizeof(MaterialValues) == 128);

// -------------------------------------------------------------------------
// Shading / raster abstraction
// -------------------------------------------------------------------------

enum class ShadingModel : uint8_t {
    DefaultLit = 0, // PBR metallic-roughness, single forward pass
    Unlit,          // baseColor only, no lighting
    Masked,         // alpha-tested (cutout), uses alphaCutoff
};

enum class AlphaMode : uint8_t {
    Opaque = 0,
    Mask,
    Blend,
};

} // namespace tour

// (namespace/RHI boundary audit, todo 8.4) the whole renderer DATA
// MODEL below (materials, packed/import/terrain/skin vertices, the
// static-mesh + upload structures) is ENGINE-OWNED and lives in
// tour:: — the RHI types it references (smvk::TextureHandle, the pipeline
// state settings, CookedMeshlets, smvk::vertexpack::) stay smvk:: and are
// spelled QUALIFIED. The smvk re-exports at the bottom of the file
// keep every smvk::/unqualified consumer spelling resolving; retire
// them as call sites migrate. The RHI owns the vertex layout schema; engine-owned layout values
// live in Engine/Renderer/vertex_layouts.h.
namespace tour {

// -------------------------------------------------------------------------
// Per-material flag bits (MaterialValues::emissiveFactor.w - the old
// pad word; the HLSL mirror is common.hlsli's MATERIAL_* defines).
// The bits ride the float word as their raw uint pattern
// (MaterialFlagsToWord / MaterialFlagsFromWord) - nothing lerps or
// sanitizes .w (the layered material blends consume .rgb only) and
// the pak serializer copies the struct verbatim.
// -------------------------------------------------------------------------
inline constexpr uint32_t kMaterialFlagReceiveContactShadows = 1u << 0;
inline constexpr uint32_t kMaterialFlagClothSheen = 1u << 1; // Order: 1886 velvet BRDF
// Receive the sun contact march's visibility (Engine/Rendergraph/
// contact_shadow.h): off for materials that must not pick it up
// (mirror-like surfaces, emissive props); the cascade shadow is
// untouched and the march itself still runs. OPT-IN bit: paks cooked
// before the flag carry w=0 and receive nothing (recook per the
// no-conversion policy).

inline float MaterialFlagsToWord(uint32_t flags) {
    return std::bit_cast<float>(flags);
}
inline uint32_t MaterialFlagsFromWord(float w) {
    return std::bit_cast<uint32_t>(w);
}

// -------------------------------------------------------------------------
// Shading / raster abstraction (MaterialValues/ShadingModel/AlphaMode
// live in tour above; the smvk using-declarations re-export them)
// -------------------------------------------------------------------------

// Runtime transparency technique for AlphaMode::Blend materials.
// Dithered (default): screen-door discard in shader, opaque pipeline
// state (depth write on, blending off) – no sorting, correct depth/MSAA.
// Blended: classic back-to-front alpha blending (no depth write).
enum class TransparentMode : uint8_t {
    Dithered = 0,
    Blended,
};

struct MaterialPipelineState {
    smvk::RasterSettings raster{};
    smvk::DepthSettings depth{};
    smvk::BlendSettings blend{};
};

// ---- Shader family (per base material) --------------------------
// CODE IDENTITY for the lit FS (docs/shader_library_design.md §5):
// which compiled lit implementation a material's draws bind.
// StandardLit = the engine's no-op hook family (default);
// ProjectLit = the project-authored hook family (the project's
// lit_hooks_project.hlsli compiled into the project_lit.*.spv
// namespace). This is CPU pipeline-selection state — NEVER a
// fragment branch in MaterialValues, never per-instance (instances
// inherit the base material's family; only the 32-bit userdata is
// per-instance). Stored beside the world's material table (see
// World::SetMaterialShaderFamily), keyed by base material at the
// same granularity as draw batching — a family switch is a
// pipeline change for every draw of the material, but NEVER a
// batch split (the material stays the sort key).
enum class MaterialShaderFamily : uint8_t {
    StandardLit = 0,
    ProjectLit = 1,
};

// ---- Shader-LOD policy (per material) ---------------------------
// Replaces the old hardcoded LodShader enum: each material declares
// how its farthest cooked LOD level shades. Two mechanisms, both
// data-driven per material:
//   1. Whole-program substitution: the farthest level may use the
//      cheap vertex-lit program (per-vertex sun + ambient, no
//      textures/shadows), so far instances trade shading quality
//      for fragment cost; materials the cheap program cannot shade
//      stay fully lit at every level.
//   2. Quality specialization: when the farthest level stays on the
//      lit program, it binds with the specialization constants below
//      (simplified BRDF, no normal-map fetch, ...), so the far
//      program pays for only what the material needs at distance.
struct ShaderLodQuality {
    // GGX specular lobe (false = Lambert-only "simplified BRDF": no
    // specular math, no specular env fetch, no per-light specular).
    bool specular = true;
    // Tangent-space normal-map fetch (false = geometric normal: no
    // normal-texture sample, no TBN work).
    bool normalMap = true;
    bool operator==(const ShaderLodQuality&) const = default;
};
struct ShaderLodPolicy {
    // Farthest cooked level may use the cheap vertex-lit program.
    // DEFAULT OFF: the glTF importer only derives this when the
    // derive config opts in (DeriveConfig::shaderLods — see
    // asset_export.h); tour/material authoring still sets it
    // explicitly per material. Disable for materials that need the
    // full program at distance (alpha-tested cutouts with alpha
    // textures, emissives you want to keep glowing, view-dependent
    // effects...).
    bool allowFarVertexLit = false;
    // Quality spec applied to the lit program when the farthest
    // level stays lit (allowFarVertexLit == false). Default =
    // full quality (indistinguishable from the base program).
    ShaderLodQuality farQuality{};
};

// Specialization constant IDs for the far-level quality knobs above.
// Must match common.hlsli (SPEC_LIT_*).
inline constexpr uint32_t kSpecLitSpecular = 20;
inline constexpr uint32_t kSpecLitNormalMap = 21;
// GI-volume weighted-path telemetry (TOURMALINE_GI_STATS=1). Must match
// common.hlsli SPEC_GI_STATS.
inline constexpr uint32_t kSpecGiStats = 22;
// Constant-coefficient diagnostic (TOURMALINE_GI_COEFF_CONST=1). Must
// match common.hlsli SPEC_GI_COEFF_CONST.
inline constexpr uint32_t kSpecGiCoeffConst = 23;
// Octahedral diffuse-irradiance path (TOURMALINE_GI_OCTAHEDRAL=1). Must
// match common.hlsli SPEC_GI_OCTAHEDRAL.
inline constexpr uint32_t kSpecGiOctahedral = 24;
// Linear-HDR diffuse-irradiance capture (TOURMALINE_GI_EXPORT_IRRADIANCE).
// Must match common.hlsli SPEC_GI_EXPORT_IRRADIANCE.
inline constexpr uint32_t kSpecGiExportIrradiance = 25;

struct MaterialCreateInfo {
    // Primary PBR textures – baseColorTexture == diffuse for backward compat.
    smvk::TextureHandle baseColorTexture{};
    smvk::TextureHandle metallicRoughnessTexture{};
    smvk::TextureHandle normalTexture{};
    smvk::TextureHandle occlusionTexture{};
    smvk::TextureHandle cavityTexture{};
    smvk::TextureHandle emissiveTexture{};
    // Legacy alias: alias for baseColorTexture
    smvk::TextureHandle diffuse{};

    glm::vec4 baseColorFactor{1.0f};
    glm::vec3 emissiveFactor{0.0f};
    float emissiveStrength = 1.0f;

    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    float cavityStrength = 1.0f;
    int channelSelectors = -1; // packed output layout from .material metadata
    // Thin opaque backlighting tinted by baseColor; 0 disables it.
    // Remains in the opaque, GPU-driven draw path (no blend/sort/PSO).
    float translucency = 0.0f;

    ShadingModel shadingModel = ShadingModel::DefaultLit;
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    // Shadow participation (SMVK_materials_shadows.castShadow). Decal
    // overlay materials opt out at cook/import time; the World flag
    // build gates IteratorFlagBit_CastShadow on it.
    bool castsShadows = true;
    // Sun contact-shadow participation (SMVK_materials_contactShadows.
    // receive): whether this material RECEIVES the pre-Main contact
    // march's visibility. Rides the emissiveFactor.w flags word as
    // kMaterialFlagReceiveContactShadows (see the MaterialValues note).
    bool receiveContactShadows = true;
    // Opt-in fabric microfiber response, serialized in the existing flags word.
    bool clothSheen = false;
    // Far-LOD shading program (see ShaderLodPolicy): the farthest
    // cooked level may use the cheap vertex-lit program. DEFAULT
    // OFF — the glTF importer only derives this when the derive
    // config opts in (DeriveConfig::shaderLods); masked/blend stay
    // lit regardless (the cheap program has no cutout/blend).
    bool allowFarVertexLit = false;
    // Far-LOD quality spec for materials that stay on the lit
    // program (see ShaderLodQuality): specular/normal-map switches
    // for the farthest cooked level. Tour attributes farSpecular /
    // farNormalMap (1 = full quality, 0 = simplified).
    ShaderLodQuality farQuality{};
};

MaterialPipelineState MakePipelineStateForMaterial(
    AlphaMode alphaMode,
    ShadingModel shadingModel = ShadingModel::DefaultLit,
    bool doubleSided = false);

struct Material {
    MaterialValues values{};
    ShadingModel shadingModel = ShadingModel::DefaultLit;
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    // Two-sided rendering (the TODO's "un-ignore" landed): the flag drives
    // CullMode::None in MakePipelineStateForMaterial, and the lit FS family
    // flips the shading normal on !SV_IsFrontFace so backfaces light
    // correctly. The GPU filter's per-triangle backface test spares these
    // draws too (DrawFlagBit_DoubleSided on the draw ring), else the
    // filtered list would drop the faces the rasterizer now draws.
    bool doubleSided = false;
    bool castsShadows = true;
    ShaderLodPolicy shaderLod{};

    // Derived / cached pipeline state. Authoritative for PSO selection
    // and for World iterator flags. Mutated only via helpers so values
    // and pipeline stay coherent.
    MaterialPipelineState pipeline{};

    bool isTransparent() const noexcept {
        return alphaMode == AlphaMode::Blend;
    }
    bool isCutout() const noexcept {
        return alphaMode == AlphaMode::Mask;
    }

    void rebuildPipeline() {
        pipeline = MakePipelineStateForMaterial(alphaMode, shadingModel, doubleSided);
    }
};

// -----------------------------------------------------------------------------
// GPU vertex streams
// -----------------------------------------------------------------------------

struct PackedVertexPosition {
    glm::vec3 pos;
};

// GPU shading stream (16 bytes): color is packed via
// smvk::vertexpack::PackColorUnorm8 and fetched as Format::RGBA8_UNorm, uv
// via smvk::vertexpack::PackUvHalf fetched as Format::RG16_Float,
// normal/tangent via PackNormalOct/PackTangentOct fetched as
// Format::R32_UInt and unpacked by hand in the vertex shader (see
// vertex_pack.hlsli). The bitangent is never stored (B =
// cross(N, T) * w in the fragment).
//
// THE LIGHTMAP WORDS MOVED OUT (they were never a contiguous tail —
// in the old 24-byte record chartUv sat at bytes 8..11 BETWEEN uv
// and normal, chart at 20..23 AFTER tangent): chartUv/chart now ride
// the SEPARATE PackedVertexLightmap stream below, carved ONLY for
// meshes that actually carry chart data. Every mesh that is not
// lightmapped (all of them while lightmaps stay deprecated) saves
// the full 8 B/vertex: 36 -> 28 B/vertex (12 position + 16 shading),
// ~8 MB per million vertices of position+shading storage. The
// trade-offs taken: an optional third heap stream, a serializer
// layout bump (mesh blob marker 2 — old paks fail closed, recook
// from zero per the no-conversion policy), and the mesh-shader leg
// fetches the chart words through the per-draw lightmap delta
// (GfxDraw.misc2.w) instead of inline shading words.
struct PackedVertexShading {
    uint32_t color;   // RGBA8_UNORM
    uint32_t uv;      // RG16_FLOAT (packHalf2x16)
    uint32_t normal;  // custom oct encoding (16+16)
    uint32_t tangent; // custom oct encoding (16+15) + handedness bit
};
static_assert(sizeof(PackedVertexShading) == 16);
static_assert(offsetof(PackedVertexShading, color) == 0 && offsetof(PackedVertexShading, uv) == 4 &&
                  offsetof(PackedVertexShading, normal) == 8 &&
                  offsetof(PackedVertexShading, tangent) == 12,
              "PackedVertexShading repacked — update StaticLayout/SkinnedLayout"
              " + the blob layout marker");

// OPTIONAL lightmap stream (8 bytes, carved only for meshes whose
// vertices carry chart data — any chartId != 0xFFFFFFFF): chartUv is
// the CHART-LOCAL lightmap UV in the chart's inner rect, [0,1],
// packed via smvk::vertexpack::PackChartUv fetched as Format::RG16_UNorm —
// UNORM because chart-space UVs need 65536-step precision (half-float
// is 1-texel-at-2048, 4-texel-at-8192); chartId is the owning chart
// index (0xFFFFFFFF = uncharted). Consumed (when lightmaps return) by
// the chart-unpack shader as
//   atlasUv = chartUv * xform[misc2.x + chartId].xy + ...zw
// with the per-draw chart offset (GfxDraw.misc2.x). Addressing: the
// lightmap heap packs INDEPENDENTLY of the pos/shading windows (that
// is where the memory comes from), so it is NOT bindable through
// baseVertex at offset 0 — the mesh-shader leg reads it via the
// per-draw delta (GfxDraw.misc2.w = lightmapBase - meshVertexBase,
// 0xFFFFFFFF = none), and a future raster chart-unpack VS does the
// same through its per-draw push (or a per-draw BindVertexBuffer
// offset).
struct PackedVertexLightmap {
    uint32_t chartUv = 0;           // RG16_UNORM (packUnorm2x16)
    uint32_t chartId = 0xFFFFFFFFu; // R32_UINT — 0xFFFFFFFF = uncharted
};
static_assert(sizeof(PackedVertexLightmap) == 8);
// NOTE: no separate skinned shading struct. The old
// VertexSkinnedShading (16 bytes: color/uv/normal/tangent only, no
// lightmap tail) was REMOVED — SkinnedLayout binds the SAME
// 24-byte stream as the static format (one heap, and its 16-byte
// stride used to misalign every vertex past the first against the
// shared stream). If a genuinely compact skinned layout is ever
// wanted, design it explicitly against the heap it reads — never
// revive this shape as a silent alternative.

// GPU skin stream (8 bytes, binding 2): joint ids RGBA8_UInt + blend
// weights RGBA8_UNorm, 4 influences per vertex. Joint ids index the
// mesh's skin palette (the Skeleton joint table, glTF JOINTS_0 order),
// which caps skeletons at 256 joints; weights are 255-based and
// renormalized in the vertex shader (w /= w0+w1+w2+w3, see
// touranim.h for the palette contract). Skinned pipelines fetch
// bindings 0 (position) + 2 (skin) for depth/shadow passes and
// 0+1+2 for shading, so every static pipeline stays untouched;
// JointIndices/JointWeights append as VSIn locations 5/6 (binding 1
// carries no TexCoord1/ChartId). VS math: skinMat = sum(w_i * pal[j_i])
// / sum(w_i); pos = skinMat * pos; normal/tangent = mat3(skinMat) * n,
// renormalized.
struct PackedSkinInfluences {
    uint32_t joints;  // RGBA8_UINT — palette indices x4
    uint32_t weights; // RGBA8_UNORM — blend weights x4 (renormalize in VS)
};
static_assert(sizeof(PackedSkinInfluences) == 8);

// GPU TERRAIN vertex (32 bytes, ONE interleaved binding): the tiled
// heightfield's own stream — never the glTF static streams (generated
// geometry, no UVs/charts/skin; the tangent carries its bitangent-flip
// handedness in w so no side channel is needed). Layout (offsets are
// load-bearing, see TerrainFullLayout):
//   POSITION      R32G32B32_FLOAT  @ 0   (pos.pos)
//   NORMAL        R16G16B16A16_FLOAT @ 12  (normal[4], xyz + pad)
//   TANGENT_BFLIP R16G16B16A16_FLOAT @ 20  (tangent[4], xyz + flip w)
//   COLOR         R8G8B8A8_UNORM    @ 28  (color[4], albedo tint)
// The LOWER-DETAIL format (TerrainReducedLayout) binds POSITION +
// COLOR only, at the SAME offsets in the SAME 32-byte stride — one
// vertex buffer serves both pipelines; the far/low pipeline just
// fetches two attributes instead of four (the terrain LOD's cheap
// shading path skips normal/tangent entirely).
struct TerrainVertex {
    glm::vec3 pos;       // @ 0  (12 B)
    uint16_t normal[4];  // @ 12 (8 B)  half xyz (w pad)
    uint16_t tangent[4]; // @ 20 (8 B)  half xyz + bitangent flip w
    uint8_t color[4];    // @ 28 (4 B)  S-mask RGBA attributes (A=solid/hole)
};
static_assert(sizeof(TerrainVertex) == 32);
static_assert(offsetof(TerrainVertex, pos) == 0);
static_assert(offsetof(TerrainVertex, normal) == 12);
static_assert(offsetof(TerrainVertex, tangent) == 20);
static_assert(offsetof(TerrainVertex, color) == 28);

// GPU TERRAIN FAR vertex (12 bytes): half4 grid + RGBA8 mask attributes.
// p = half4(ix, iz, unused, skirtFlag); the RGB attribute channels
// are available to terrain materials, alpha=0 marks a clipping hole.
// World XZ reconstructs in the VS; Y samples the R16 height atlas.
struct TerrainFarVertex {
    uint16_t p[4];
    uint8_t color[4];
};
static_assert(sizeof(TerrainFarVertex) == 12);
static_assert(offsetof(TerrainFarVertex, p) == 0);
static_assert(offsetof(TerrainFarVertex, color) == 8);

inline void PackTerrainFarVertex(float ix,
                                 float iz,
                                 const glm::vec4& attributes,
                                 bool skirt,
                                 TerrainFarVertex& out) noexcept {
    out.p[0] = glm::packHalf1x16(ix);
    out.p[1] = glm::packHalf1x16(iz);
    out.p[2] = 0;
    out.p[3] = glm::packHalf1x16(skirt ? 1.0f : 0.0f);
    for (int c = 0; c < 4; ++c)
        out.color[c] = uint8_t(glm::clamp(int(attributes[c] * 255.0f + 0.5f), 0, 255));
}

// CPU-side terrain vertex authoring form (floats in, packed at build
// time): terrain block builders fill these and PackTerrainVertex
// writes the GPU struct. colorAlpha rides the color.w channel (the
// layered-blend weight future path; 1 by default).
struct TerrainVertexSrc {
    glm::vec3 pos;
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec3 tangent{1.0f, 0.0f, 0.0f};
    float tangentFlip = 1.0f;
    glm::vec4 color{1.0f};
};

inline void PackTerrainVertex(const TerrainVertexSrc& src, TerrainVertex& out) noexcept {
    out.pos = src.pos;
    smvk::vertexpack::PackTerrainNormalHalf(src.normal, out.normal);
    smvk::vertexpack::PackTerrainTangentHalf(src.tangent, src.tangentFlip, out.tangent);
    out.color[0] = uint8_t(glm::clamp(int(src.color.r * 255.0f + 0.5f), 0, 255));
    out.color[1] = uint8_t(glm::clamp(int(src.color.g * 255.0f + 0.5f), 0, 255));
    out.color[2] = uint8_t(glm::clamp(int(src.color.b * 255.0f + 0.5f), 0, 255));
    out.color[3] = uint8_t(glm::clamp(int(src.color.a * 255.0f + 0.5f), 0, 255));
}

// CPU-side import vertex. texCoord2 is the LIGHTMAP chart-UV channel
// (glTF TEXCOORD_1): filled by the importer when the attribute exists
// and overwritten with the CHART-LOCAL uv by the lightmap cook at bake
// time (the atlas-space transform lives per chart in
// StaticMesh::lmChartXforms and is applied in the vertex shader).
// Zero when a scene carries no lightmaps (shader gates on the
// per-draw flag, never on the value). chartId is the owning lightmap
// chart index (0xFFFFFFFF = uncharted), stamped by the cook alongside
// texCoord2 (seam corners are duplicated per chart).
struct MeshVertex {
    glm::vec3 pos;
    glm::vec4 color;
    glm::vec2 texCoord;
    glm::vec2 texCoord2;
    glm::vec3 normal;
    glm::vec4 tangent;
    uint32_t chartId = 0xFFFFFFFFu;

    bool operator==(const MeshVertex& other) const {
        return pos == other.pos && color == other.color && texCoord == other.texCoord &&
               texCoord2 == other.texCoord2 && normal == other.normal && tangent == other.tangent &&
               chartId == other.chartId;
    }
};

// One AoS import vertex -> one PackedVertexPosition + one
// PackedVertexShading + one PackedVertexLightmap (the exact GPU
// heap-stream layout). Shared by Cluster::UploadMesh(es)
// (runtime upload) and the pak cooker's packed-SoA mesh blobs (v5;
// cooked geometry round-trips the heap layout verbatim) — the two
// must never diverge. The lightmap word pair packs unconditionally;
// the STREAM decision (whether the mesh carves a lightmap range at
// all) happens at allocation — any chartId != 0xFFFFFFFF.
inline void PackMeshVertex(const MeshVertex& src,
                           PackedVertexPosition& pos,
                           PackedVertexShading& shade,
                           PackedVertexLightmap& lm) noexcept {
    pos.pos = src.pos;
    shade.color = smvk::vertexpack::PackColorUnorm8(src.color);
    shade.uv = smvk::vertexpack::PackUvHalf(src.texCoord);
    shade.normal = smvk::vertexpack::PackNormalOct(src.normal);
    shade.tangent = smvk::vertexpack::PackTangentOct(glm::vec3(src.tangent), src.tangent.w);
    lm.chartUv = smvk::vertexpack::PackChartUv(src.texCoord2);
    lm.chartId = src.chartId;
}

// Inverse pack (the .mesh-asset cook path rebuilds AoS uploads from
// the packed SoA streams; lossy exactly like the GPU fetch, so the
// round trip is byte-stable).
inline void UnpackMeshVertex(const PackedVertexPosition& pos,
                             const PackedVertexShading& shade,
                             const PackedVertexLightmap& lm,
                             MeshVertex& out) noexcept {
    out.pos = pos.pos;
    const glm::vec4 c = glm::unpackUnorm4x8(shade.color);
    out.color = c;
    out.texCoord = glm::unpackHalf2x16(shade.uv);
    out.texCoord2 = glm::unpackUnorm2x16(lm.chartUv);
    out.chartId = lm.chartId;
    out.normal = smvk::vertexpack::UnpackNormalOct(shade.normal);
    out.tangent = smvk::vertexpack::UnpackTangentOct(shade.tangent);
}

// CPU-side import vertex (skinned): position/color/uv/normal/tangent
// plus the glTF JOINTS_0/WEIGHTS_0 pair. The importer keeps the top 4
// influences, renormalizes the weights, and packs at upload (joints ->
// RGBA8_UInt, weights -> RGBA8_UNorm) into the binding-2 skin stream.
// No texCoord2/charts, no chartId: skinned meshes are dynamic and
// never lightmapped.
struct SkinnedVertex {
    glm::vec3 pos;
    glm::vec4 color;
    glm::vec2 texCoord;
    glm::vec3 normal;
    glm::vec4 tangent;
    glm::u8vec4 joints; // palette indices x4 (Skeleton joint table)
    glm::vec4 weights;  // normalized, sums to 1

    bool operator==(const SkinnedVertex& other) const {
        return pos == other.pos && color == other.color && texCoord == other.texCoord &&
               normal == other.normal && tangent == other.tangent && joints == other.joints &&
               weights == other.weights;
    }
};

// Debug line stream (pos-only, binding 0): powers the meshlet/cone
// debug views without touching the static heap (see BindVertexBuffer).
struct DebugLineVertex {
    glm::vec3 pos;
};
static_assert(sizeof(DebugLineVertex) == 12);

} // namespace tour

// COMPAT (the namespace/RHI boundary audit, todo item 8.4): the
// renderer data model lives in tour; the smvk re-exports keep the
// unqualified spellings inside smvk-scope code (the Vulkan backend,
// the rendergraph passes, the layout contract below) and every
// smvk::-qualified consumer (WorldRenderer, the editor, the tools,
// the tests) resolving. Retire as call sites migrate; the R7
// architecture gate baselines this block.
namespace smvk {
using ::tour::AlphaMode;
using ::tour::DebugLineVertex;
using ::tour::kMaterialFlagReceiveContactShadows;
using ::tour::kSpecLitNormalMap;
using ::tour::kSpecLitSpecular;
using ::tour::MakePipelineStateForMaterial;
using ::tour::Material;
using ::tour::MaterialCreateInfo;
using ::tour::MaterialFlagsFromWord;
using ::tour::MaterialFlagsToWord;
using ::tour::MaterialPipelineState;
using ::tour::MaterialShaderFamily;
using ::tour::MaterialValues;
using ::tour::MeshVertex;
using ::tour::PackedSkinInfluences;
using ::tour::PackedVertexLightmap;
using ::tour::PackedVertexPosition;
using ::tour::PackedVertexShading;
using ::tour::PackMeshVertex;
using ::tour::PackTerrainFarVertex;
using ::tour::PackTerrainVertex;
using ::tour::ShaderLodPolicy;
using ::tour::ShaderLodQuality;
using ::tour::ShadingModel;
using ::tour::SkinnedVertex;
using ::tour::TerrainFarVertex;
using ::tour::TerrainVertex;
using ::tour::TerrainVertexSrc;
using ::tour::TransparentMode;
using ::tour::UnpackMeshVertex;
} // namespace smvk

// Engine-owned vertex layout definitions now live in Renderer/vertex_layouts.h.
namespace smvk {

// ---------------------------------------------------------------------------
// Vertex-layout validation (backend-neutral: compiles into every
// backend TU with the descriptions above). Structural invariants the
// EXPLICIT locations made checkable: unique location per format,
// usable attribute formats, offsets aligned to the format and
// contained in the owning binding's stride, bounded binding indices.
// A pipeline built from an invalid layout is a programmer error —
// the Vulkan build asserts this (see BuildGraphicsPipeline) and the
// CPU test pins it; err (optional) receives the first problem found.
// ---------------------------------------------------------------------------
inline bool ValidateVertexLayout(const VertexLayoutDesc& desc, std::string* err = nullptr) {
    const auto fail = [err](const char* what) {
        if (err)
            *err = what;
        return false;
    };
    // maxVertexInputBindings/Attributes minimums are 16 across
    // Vulkan versions — the engine's layouts stay well under.
    std::array<bool, 16> bindingUsed{};
    std::array<bool, 16> locationUsed{};
    for (const VertexBinding& b : desc.bindings) {
        if (b.binding >= bindingUsed.size())
            return fail("binding index >= 16 (maxVertexInputBindings)");
        if (bindingUsed[b.binding])
            return fail("duplicate binding index");
        bindingUsed[b.binding] = true;
        for (const VertexAttribute& a : b.attributes) {
            if (a.location >= locationUsed.size())
                return fail("attribute location >= 16"
                            " (maxVertexInputAttributes)");
            if (locationUsed[a.location])
                return fail("duplicate attribute location");
            locationUsed[a.location] = true;
            const FormatBlockInfo fb = GetFormatBlockInfo(a.format);
            if (fb.bytesPerBlock == 0 || fb.compressed)
                return fail("attribute format is not a usable"
                            " vertex-attribute format");
            const uint32_t compBytes =
                fb.bytesPerBlock / std::max(GetFormatComponentCount(a.format), 1u);
            if (a.offset % compBytes != 0)
                return fail("attribute offset not aligned to the format");
            if (a.offset + fb.bytesPerBlock > b.stride)
                return fail("attribute overruns its binding stride");
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
// Draw information
// -----------------------------------------------------------------------------

} // namespace smvk

// (the data model tail — back into tour:: with the rest of the model;
// no dependency on the layout contract above)
namespace tour {

struct Surface {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int32_t baseVertex = 0;
};

struct Geometry {
    smvk::PrimitiveTopology topology = smvk::PrimitiveTopology::TriangleList;

    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;

    std::vector<Surface> surfaces;

    glm::vec3 aabbMin{};
    glm::vec3 aabbMax{};
    float sphereRadius = 0.0f;
};

// -----------------------------------------------------------------------------
// Static mesh
// -----------------------------------------------------------------------------

struct StaticMesh {
    Geometry geometry;
    // Skinned marker (the import post pass / the deserializer sets
    // it from a non-empty PackedSkinInfluences stream): tooling + the draw
    // path's format pick gate on this + the retained stream, never
    // on a heuristic.
    bool skinned = false;
    // Cook-time meshlet data for the future indirect/mesh-shader path.
    // Empty when the mesh was cooked without meshlets. Never consulted
    // by the current direct-draw renderer.
    std::optional<smvk::CookedMeshlets> meshlets;
    // Lightmap: atlas chunk index (cooked with --lightmap; the lobe
    // count rides the gfx draw ring (GfxDraw.misc.w) to the lit
    // fragment). 0xFFFFFFFF = none.
    uint32_t lmChunk = 0xFFFFFFFFu;
    // OPTIONAL lightmap stream: this mesh's window start in the
    // lightmap heap, in VERTEX units — carved only when the mesh's
    // vertices carry chart data (any chartId != 0xFFFFFFFF).
    // 0xFFFFFFFF = no lightmap window. The mesh-shader leg's draw
    // ring carries the addressing delta (lightmapBase - the mesh's
    // global vertexBase) in GfxDraw.misc2.w.
    uint32_t lightmapBase = 0xFFFFFFFFu;
    // Lightmap chart atlas transforms, parallel to the cook's chart
    // indices (one float4 per chart: xy = scale, zw = offset, so
    // atlasUv = chartUv * xy + zw). Empty when the mesh carries no
    // lightmap. The runtime concatenates every mesh's transforms into
    // one GPU buffer (sorted by mesh, then chart) and writes each
    // draw's base offset into the draw ring (GfxDraw chart offset);
    // the vertex shader fetches xform[chartOffset + chartId].
    std::vector<glm::vec4> lmChartXforms;
};

// -----------------------------------------------------------------------------
// Upload structure
// -----------------------------------------------------------------------------

struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Surface> surfaces;
    glm::vec3 aabbMin{};
    glm::vec3 aabbMax{};
    // glTF skin stream (binding 2), parallel to `vertices` when
    // non-empty: the packed PackedSkinInfluences per vertex (top-4 influences,
    // renormalized, RGBA8). Joint ids index the CLUSTER's Skeleton
    // joint table (touranim.h; the glTF JOINTS_0 order). Empty = a
    // static mesh (the formats/paths gate on the empty stream, never
    // on a separate flag).
    std::vector<PackedSkinInfluences> skin;
};

void AllocateStaticVertexFmt(smvk::ResourceManager& ctx);

} // namespace tour

// COMPAT tail (the second half of the data-model re-export: these
// five are declared in the tour block above, so their using rows
// belong here — after the definitions; the R7 gate counts the whole
// file's total).
namespace smvk {
using ::tour::AllocateStaticVertexFmt;
using ::tour::Geometry;
using ::tour::MeshData;
using ::tour::StaticMesh;
using ::tour::Surface;
} // namespace smvk
