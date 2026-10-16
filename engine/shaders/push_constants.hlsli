// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Scene push-constant mirror. SINGLE SOURCE OF TRUTH for field order:
//   tools/push_constants_idl/push_constants.yaml
// C++ twin: engine/include/Engine/Renderer/Renderer.h
//   (engine::IRenderer::PushConstants, static_assert per-field offsets).
// Verify: python tools/push_constants_idl/codegen.py --check
// Included by common.hlsli (legacy umbrella) — new shaders may include
// this file directly for fine-grained DXC deps.
//
// LAYOUT INVARIANT: real shared fields stay within bytes 0..192 — the
// HLSL mirror structs (DefaultVsPush, UvFlatVsPush, the seed/shadow/
// placement/filtered tails) hard-code the sub-192 offsets and bytes
// 192..256 are the per-draw overlay scratch (filtered VS tail 192..224,
// direct chart tail 192..208, per-draw prevModel push 192..240). The
// FS-only hot slab is reached via FrameUniforms::frameHot, NOT a pc
// member (a pc member here shifted every later field and broke every
// mirror — async device loss; see Engine/Renderer/Renderer.h).
struct PushConstants
{
    float4x4 model;
    vk::BufferPointer<FrameUniforms>
    frameUniforms;
    vk::BufferPointer<LightBuffer>
    bLights;
    vk::BufferPointer<MaterialsBuffer>
    bMats;
    vk::BufferPointer<DirectionalCascadeBuffer>
    cascadeBuffer;
    uint materialId;
    uint comparisonSamplerId;
    uint numCascades;
    uint cascadeIndex;
    // Forward+ (opaque pass; ignored when fpLightCount == 0)
    vk::BufferPointer<ForwardPlusLightBuffer>
    fpLights;
    vk::BufferPointer<ForwardPlusTileCountBuffer>
    fpTileCounts;
    vk::BufferPointer<ForwardPlusTileIndexBuffer>
    fpTileIndices;
    uint fpTileCountX;
    uint fpLightCount;
    uint fpTileSize;
    uint fpMaxPerTile;
    // Spot shadows (opaque pass sampling + spot depth passes).
    // spotShadowBuffer: tile descriptors (GpuCascadeDescription tiles).
    // spotShadowMapIndex: bindless spot atlas, UINT32_MAX when none.
    // spotShadowCount: active tiles this frame (<= MAX_SPOT_SHADOWS).
    // spotIndex: tile rendered by the current shadow pass,
    //   0xFFFFFFFF when the pass is NOT a spot shadow pass.
    vk::BufferPointer<SpotShadowBuffer>
    spotShadowBuffer;
    uint spotShadowMapIndex;
    uint spotShadowCount;
    uint spotIndex;
    // Transparency technique for AlphaMode::Blend materials (see DitherKeep
    // above): 0 = dithered screen-door (default, opaque state), 1 = classic
    // alpha blend. Must match Engine/Renderer/Renderer.h pad_pc.
    uint pad_pc;
    // RenderView system (see renderview.hlsli): per-frame virtual cameras
    // (main game camera + one view per shadow cascade / spot tile / custom
    // pass). mainRenderView indexes the game camera view; renderViewIndex
    // is the view the current pass renders from, 0xFFFFFFFF when the pass
    // has no view of its own (compute / post passes).
    vk::BufferPointer<RenderViewBuffer>
    renderViews;
    uint mainRenderView;
    uint renderViewIndex;
    // Motion vector pass: last frame's model matrix per draw as float3x4 (192..240).
	// Last row (0,0,0,1) is implied. 12 floats = 48 bytes. The CPU pack
	// (world_renderer.cpp) stores matrix COLUMNS (glm columns xyz +
	// translation), matching the SPV RowMajor/stride-12 v4 x M lowering.
    float3x4 prevModel;
    // .x: the DIRECT legs' per-draw dither-on-closeup flag (Scene::
    // ditherCloseup — the direct prepass/Main/shadow legs push it per
    // draw, 0/1; default.vert / shadow.vert forward it flat at VS
    // location 8 so the FS family reads one channel on both paths —
    // the filtered legs deliver the Draw ring's flag bit there). y..w
    // stay scratch.
    uint4 padEnd;
};
