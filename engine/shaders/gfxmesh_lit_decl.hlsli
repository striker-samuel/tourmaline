// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// THE MESH-LIT FS INPUT DECLARATION (docs/shader_library_design.md
// Phase B, plan item 6): the GFX_MESH_PSIN block gfxmesh_lit.frag.hlsl
// always carried, extracted so a project-authored mesh family wrapper
// (gfxmesh_project_lit.frag.hlsl) reproduces the EXACT linked
// interface without copying 40 lines of varyings — locations are a
// linkage contract, and a hand-copied twin drifting from the engine's
// would fail to link against gfxfilter.mesh.hlsl at pipeline build.
//
// The wrapper's shape (order is LOAD-BEARING):
//   #define GFXMESH_FS_PASS
//   #include "lit_dependencies.hlsli"
//   #include "gfxmesh_lit_decl.hlsli"   <- this file
//   #include "<the hook family>"
//   #include "default_lit_body.hlsli"
//
// This links ONLY against the mesh shader (gfxfilter.mesh.hlsl,
// locations 0..9): the classic VS never writes location 7
// (meshMaterial), so a mesh FS must never pair with it.

#ifndef TM_GFXMESH_LIT_DECL_HLSLI
#define TM_GFXMESH_LIT_DECL_HLSLI

#define GFX_MESH_PSIN
struct PSIn
{
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 worldPos : TEXCOORD1;
    [[vk::location(3)]] float3 normal : TEXCOORD2;
    [[vk::location(4)]] float4 tangent : TANGENT;
    [[vk::location(5)]] float2 lightmapUv : TEXCOORD3;
    [[vk::location(6)]] nointerpolation uint lmChunk : TEXCOORD4;
    // Mesh-only: per-draw material (misc2.y) for the multi-material dispatch.
    [[vk::location(7)]] nointerpolation uint meshMaterial : TEXCOORD5;
    // Dither-on-closeup flag (flat; Scene::ditherCloseup — the ring's
    // GFX_DRAW_DITHER_CLOSEUP forwarded by the mesh MS; see the body's
    // discard + common.hlsli CloseupDitherKeep). EXPLICIT location 8
    // (pinned like every ditherCloseup declaration — the mesh MS writes
    // location 8; sequential assignment happened to match here but
    // dead-stripping could shift it).
    [[vk::location(8)]] nointerpolation uint ditherCloseup : TEXCOORD7;
    // The resolved per-draw material userdata (flat; the ring's misc3.x
    // forwarded by the mesh MS — the hooks' runtime input; see
    // lit_hooks_api.hlsli). EXPLICIT location 9, pinned exactly like
    // ditherCloseup above.
    [[vk::location(9)]] nointerpolation uint userData : TEXCOORD8;
    [[vk::location(10)]] nointerpolation uint lightLayerMask : TEXCOORD9;
    bool isFrontFace : SV_IsFrontFace;
};
#define GFX_MATERIAL_ID (IN.meshMaterial)

#endif // TM_GFXMESH_LIT_DECL_HLSLI
