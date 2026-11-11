// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

#include <Engine/Renderer/mesh.h>

// Engine-owned vertex input layouts. The RHI accepts any VertexLayoutDesc;
// these are merely the layouts used by Tourmaline's packed geometry streams.
namespace tour::vertex_layouts {
using smvk::VertexLayoutDesc;
using smvk::VertexInputRate;
using smvk::VertexSemantic;
using smvk::Format;

inline const VertexLayoutDesc Empty{};

inline const VertexLayoutDesc DebugLine{
    .bindings = {{.binding = 0,
                  .stride = sizeof(DebugLineVertex),
                  .inputRate = VertexInputRate::Vertex,
                  .attributes = {{.semantic = VertexSemantic::Position,
                                  .location = 0,
                                  .format = Format::RGB32_Float,
                                  .offset = offsetof(DebugLineVertex, pos)}}}}};

inline const VertexLayoutDesc Static{
    .bindings = {{.binding = 0,
                  .stride = sizeof(PackedVertexPosition),
                  .inputRate = VertexInputRate::Vertex,
                  .attributes = {{.semantic = VertexSemantic::Position,
                                  .location = 0,
                                  .format = Format::RGB32_Float,
                                  .offset = offsetof(PackedVertexPosition, pos)}}},
                 {.binding = 1,
                  .stride = sizeof(PackedVertexShading),
                  .inputRate = VertexInputRate::Vertex,
                  .attributes = {// Shader VSIn locations are EXPLICIT (0..4 = Position,
                                 // Color, TexCoord0, Normal, Tangent) — the pairing with
                                 // the compiled static vertex shaders is pinned by
                                 // location, no longer by this list's order.
                                 // Lightmaps are DEPRECATED: the chart words ride the
                                 // SEPARATE optional PackedVertexLightmap stream (carved
                                 // only for meshes with chart data — see the struct above)
                                 // and this format does not bind them. The static vertex
                                 // shaders declare no TexCoord1/ChartId inputs;
                                 // StaticLightmapped below keeps the option alive
                                 // (gated off until lightmaps return).
                                 {.semantic = VertexSemantic::Color,
                                  .location = 1,
                                  .format = Format::RGBA8_UNorm,
                                  .offset = offsetof(PackedVertexShading, color)},
                                 {.semantic = VertexSemantic::TexCoord0,
                                  .location = 2,
                                  .format = Format::RG16_Float,
                                  .offset = offsetof(PackedVertexShading, uv)},
                                 {.semantic = VertexSemantic::Normal,
                                  .location = 3,
                                  .format = Format::R32_UInt,
                                  .offset = offsetof(PackedVertexShading, normal)},
                                 {.semantic = VertexSemantic::Tangent,
                                  .location = 4,
                                  .format = Format::R32_UInt,
                                  .offset = offsetof(PackedVertexShading, tangent)}}}}};

// LIGHTMAPPED static format (the gated-off lightmap option): the
// SAME bindings 0/1 as Static (position + the 16-byte shading
// stream) plus BINDING 2 = the optional PackedVertexLightmap stream
// (8-byte stride): TexCoord1 (chart-local lightmap UV, RG16_UNorm)
// as VSIn location 5 and ChartId (R32_UInt) as location 6. Nothing
// binds this format while lightmaps stay deprecated; when they
// return, the lightmap-aware vertex shaders (chart-unpack tail:
// atlasUv = chartUv * xform.xy + xform.zw with the per-draw chart
// offset) pair with it, and the classic static VSs above keep their
// 0..4 locations. ADDRESSING NOTE: the lightmap heap packs
// independently of the pos/shading windows (that is where the memory
// saving comes from), so binding 2 is NOT addressable through
// baseVertex at offset 0 — the consuming shader reads it through a
// per-draw BindVertexBuffer offset or the per-draw BDA delta
// (GfxDraw.misc2.w, the mesh-shader leg's pattern). The binding
// declaration pins the formats/locations ABI either way.
inline const VertexLayoutDesc StaticLightmapped{
    .bindings = {{.binding = 0,
                  .stride = sizeof(PackedVertexPosition),
                  .inputRate = VertexInputRate::Vertex,
                  .attributes = {{.semantic = VertexSemantic::Position,
                                  .format = Format::RGB32_Float,
                                  .offset = offsetof(PackedVertexPosition, pos)}}},
                 {.binding = 1,
                  .stride = sizeof(PackedVertexShading),
                  .inputRate = VertexInputRate::Vertex,
                  .attributes = {{.semantic = VertexSemantic::Color,
                                  .location = 1,
                                  .format = Format::RGBA8_UNorm,
                                  .offset = offsetof(PackedVertexShading, color)},
                                 {.semantic = VertexSemantic::TexCoord0,
                                  .location = 2,
                                  .format = Format::RG16_Float,
                                  .offset = offsetof(PackedVertexShading, uv)},
                                 {.semantic = VertexSemantic::Normal,
                                  .location = 3,
                                  .format = Format::R32_UInt,
                                  .offset = offsetof(PackedVertexShading, normal)},
                                 {.semantic = VertexSemantic::Tangent,
                                  .location = 4,
                                  .format = Format::R32_UInt,
                                  .offset = offsetof(PackedVertexShading, tangent)}}},
                 {.binding = 2,
                  .stride = sizeof(PackedVertexLightmap),
                  .inputRate = VertexInputRate::Vertex,
                  .attributes = {{.semantic = VertexSemantic::TexCoord1,
                                  .location = 5,
                                  .format = Format::RG16_UNorm,
                                  .offset = offsetof(PackedVertexLightmap, chartUv)},
                                 {.semantic = VertexSemantic::ChartId,
                                  .location = 6,
                                  .format = Format::R32_UInt,
                                  .offset = offsetof(PackedVertexLightmap, chartId)}}}}};

// Skinned vertex format: bindings 0 (position) + 1 (the SAME 16-byte
// PackedVertexShading heap stream as Static — ONE heap serves
// both formats, the terrain full/low precedent; skinned meshes never
// carry lightmap charts, so they never carve the optional lightmap
// stream) plus the binding-2 skin stream. JointIndices/JointWeights
// are locations 5/6. Depth/shadow passes bind only bindings 0+2
// (position + skin) — skinning must move the position. NOTE: there
// is no separate compact skinned shading struct — the old
// VertexSkinnedShading (a plausible-looking alternative whose 16-byte
// stride misaligned against the then-24-byte heap stream) was
// REMOVED; the 16-byte goal is now reached by the MAIN stream itself
// being 16 bytes (28 B/vertex static, 36 B skinned incl. the skin
// influences).
inline const VertexLayoutDesc Skinned{
    .bindings = {{.binding = 0,
                  .stride = sizeof(PackedVertexPosition),
                  .inputRate = VertexInputRate::Vertex,
                  .attributes = {{.semantic = VertexSemantic::Position,
                                  .location = 0,
                                  .format = Format::RGB32_Float,
                                  .offset = offsetof(PackedVertexPosition, pos)}}},
                 {.binding = 1,
                  // The SAME 16-byte heap shading stream as Static
                  // (PackedVertexShading) - ONE heap serves both formats
                  // (the terrain full/low precedent). Skinned meshes never
                  // carry lightmap charts, so they never carve the
                  // optional PackedVertexLightmap stream.
                  .stride = sizeof(PackedVertexShading),
                  .inputRate = VertexInputRate::Vertex,
                  .attributes = {{.semantic = VertexSemantic::Color,
                                  .location = 1,
                                  .format = Format::RGBA8_UNorm,
                                  .offset = offsetof(PackedVertexShading, color)},
                                 {.semantic = VertexSemantic::TexCoord0,
                                  .location = 2,
                                  .format = Format::RG16_Float,
                                  .offset = offsetof(PackedVertexShading, uv)},
                                 {.semantic = VertexSemantic::Normal,
                                  .location = 3,
                                  .format = Format::R32_UInt,
                                  .offset = offsetof(PackedVertexShading, normal)},
                                 {.semantic = VertexSemantic::Tangent,
                                  .location = 4,
                                  .format = Format::R32_UInt,
                                  .offset = offsetof(PackedVertexShading, tangent)}}},
                 {.binding = 2,
                  .stride = sizeof(PackedSkinInfluences),
                  .inputRate = VertexInputRate::Vertex,
                  .attributes = {{.semantic = VertexSemantic::JointIndices,
                                  .location = 5,
                                  .format = Format::RGBA8_UInt,
                                  .offset = offsetof(PackedSkinInfluences, joints)},
                                 {.semantic = VertexSemantic::JointWeights,
                                  .location = 6,
                                  .format = Format::RGBA8_UNorm,
                                  .offset = offsetof(PackedSkinInfluences, weights)}}}}};

// Terrain FULL-detail format: ONE interleaved binding, 32-byte
// stride, four attributes (near tiles; the shading path reads the
// normal + tangent + bitangent flip). Shader VSIn locations are
// EXPLICIT: 0=Position, 1=Normal, 2=TangentBFlip, 3=Color.
inline const VertexLayoutDesc TerrainFull{
    .bindings = {{.binding = 0,
                  .stride = sizeof(TerrainVertex),
                  .inputRate = VertexInputRate::Vertex,
                  .attributes = {{.semantic = VertexSemantic::Position,
                                  .location = 0,
                                  .format = Format::RGB32_Float,
                                  .offset = offsetof(TerrainVertex, pos)},
                                 {.semantic = VertexSemantic::Normal,
                                  .location = 1,
                                  .format = Format::RGBA16_Float,
                                  .offset = offsetof(TerrainVertex, normal)},
                                 {.semantic = VertexSemantic::TangentBFlip,
                                  .location = 2,
                                  .format = Format::RGBA16_Float,
                                  .offset = offsetof(TerrainVertex, tangent)},
                                 {.semantic = VertexSemantic::Color,
                                  .location = 3,
                                  .format = Format::RGBA8_UNorm,
                                  .offset = offsetof(TerrainVertex, color)}}}}};

// Terrain LOWER-detail format: the SAME binding + stride + offsets,
// two attributes only (far tiles; the cheap shading path skips the
// normal/tangent fetch entirely — POSITION @0 and COLOR @28). One
// vertex buffer serves BOTH formats: the far pipeline simply binds
// fewer attributes out of the same 32-byte records (the SAME
// physical vertex size as the full format — "low" refers to the
// fetched-attribute count, not the record size). VSIn locations are
// 0=Position, 1=Color (the low terrain shader declares only these
// two, in that order).
inline const VertexLayoutDesc TerrainReduced{
    .bindings = {{.binding = 0,
                  .stride = sizeof(TerrainVertex),
                  .inputRate = VertexInputRate::Vertex,
                  .attributes = {{.semantic = VertexSemantic::Position,
                                  .location = 0,
                                  .format = Format::RGB32_Float,
                                  .offset = offsetof(TerrainVertex, pos)},
                                 {.semantic = VertexSemantic::Color,
                                  .location = 1,
                                  .format = Format::RGBA8_UNorm,
                                  .offset = offsetof(TerrainVertex, color)}}}}};

// Terrain FAR format: ONE interleaved binding, 8-byte stride, one
// RGBA16_FLOAT attribute (grid ix/iz/shade/skirt packed half4).
// VSIn location 0 = PackedGrid (the far terrain VS declares only this).
inline const VertexLayoutDesc TerrainFarGrid{
    .bindings = {{.binding = 0,
                  .stride = sizeof(TerrainFarVertex),
                  .inputRate = VertexInputRate::Vertex,
                  .attributes = {{.semantic = VertexSemantic::Position,
                                  .location = 0,
                                  .format = Format::RGBA16_Float,
                                  .offset = offsetof(TerrainFarVertex, p)},
                                 {.semantic = VertexSemantic::Color,
                                  .location = 1,
                                  .format = Format::RGBA8_UNorm,
                                  .offset = offsetof(TerrainFarVertex, color)}}}}};

} // namespace tour::vertex_layouts
