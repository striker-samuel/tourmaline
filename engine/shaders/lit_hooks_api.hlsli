// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// THE LIT-HOOK API (design: docs/shader_library_design.md).
//
// A compile-time HLSL library seam around the shared lit fragment body
// (default_lit_body.hlsli): a game supplies ORDINARY HLSL FUNCTIONS with
// these exact signatures, included by its entry wrappers BEFORE the body,
// and the body calls them at two deliberate points. No reflection, no
// string replacement, no runtime linking — source-level composition only.
//
//   TM_ModifySurface — after texture/layer material evaluation and
//     Forward+ decals, BEFORE the BRDF derives F0 / specular AA / any
//     lighting. Edits the evaluated surface (the wetness hook's home:
//     physically meaningful roughness/baseColor changes flow through the
//     normal PBR path).
//   TM_ModifyLitColor — after the lit color is assembled and fogged,
//     BEFORE the final HDR sanitization/output. Artistic tinting of the
//     final pixel (the damage-flash hook's home). NOT guaranteed to tint
//     the AO/SSR companion exports or downstream composites — effects
//     needing full post-composite consistency belong in a post pass.
//
// The contract in one line: ONE uint32 of GAME-OWNED runtime userdata,
// delivered flat per draw, edited by ordinary functions. The API also
// supplies already-existing readonly material/lightmap context without
// adding draw fields, descriptor bindings or vertex attributes. The
// bit layout of userData belongs to gameplay code.
//
// v1 deliberately EXCLUDES: the unlit early-return branch (no promise
// unlit/debug paths honor hooks), coverage/alpha edits (no dissolve
// without a cross-pass depth/shadow/motion contract — see design §10),
// and vertex displacement. The hook structs are the whole ABI: no raw
// MaterialValues, no GPU pointers, no descriptor tables.

#ifndef TM_LIT_HOOKS_API_HLSLI
#define TM_LIT_HOOKS_API_HLSLI

struct TMLitInput
{
    // Resolved per-draw userdata: the material instance's override or
    // the base material's default (bit layout is gameplay-owned).
    uint userData;
    // The pixel's first UV channel (TEXCOORD0).
    float2 uv;
    // World-space position of the shaded pixel.
    float3 worldPos;
    // The (post-normal-map, pre-JHFAA-correction) shading normal.
    float3 normalWS;
    // The frame clock (FramePSHot::time) in seconds.
    float time;

    // Resolved material ID. This is a draw-ring varying on the mesh path
    // and a fragment push on direct / GPU-indexed paths; projects should
    // never need to know which GPU backend supplied it.
    uint materialId;
    // Atlas-space UV, already reconstructed from the optional chart
    // stream by the producing VS/MS. Not the chart-local UV and not a
    // guarantee that the fragment has sampled a lightmap.
    float2 lightmapAtlasUv;
    // Cooked SG lobe count (5, 6, 9, 12) or TM_LM_NONE when absent.
    // Presence != runtime lightmap use: GI_NONE, lmEnabled, atlas
    // availability and material blending can affect sampling.
    uint lightmapLobes;
};

struct TMSurface
{
    // Linear, evaluated base color (textures + layers + decals applied).
    float3 baseColor;
    // glTF metallic factor.
    float metallic;
    // PERCEPTUAL roughness, pre specular-AA.
    float roughness;
    // HDR scene-linear emissive (factor * strength).
    float3 emissive;
};

// Prototypes ONLY — the implementation comes from the entry wrapper's
// include: lit_hooks_default.hlsli (the engine no-op family) or the
// project's hook library. An entry that includes the body without ANY
// implementation fails the DXC compile loudly (undefined function) —
// a project effect can never silently fall back to stock shading.
void TM_ModifySurface(inout TMSurface surface, TMLitInput input);
void TM_ModifyLitColor(inout float3 hdrColor, TMLitInput input);

#endif // TM_LIT_HOOKS_API_HLSLI
