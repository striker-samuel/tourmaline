// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#define CONTACT_SHADOW_PASS
#include "common.hlsli"
#include "dof.hlsli"
#include "gpu_platform.hlsli"

// Sun contact shadows (Engine/Rendergraph/contact_shadow.h): the pre-Main
// compute raymarch through the FINAL opaque depth toward the sun, writing
// a per-pixel visibility texture the lit body min()s with the CSM/EVSM
// factor (default_lit_body.hlsli) - the short-range sun occlusion the
// cascade texel size cannot resolve (contact hardening at centimeter
// scale: the leaf lattice over a wall, the ball in the corner, the
// awning's hairline penumbra).
//
// ONE thread per VISIBILITY texel (full or half res - the lit body
// bilinearly upsamples the half-res target, the AO-family pattern).
// Per texel:
//   1. reconstruct the receiver's world position from the depth texture
//      (point-sampled at the texel center mapped through the full-res
//      uv window);
//   2. march a SHORT ray (maxDistance, ~0.35 m) toward the sun in world
//      space, startBias past the receiver (the self-hit guard);
//   3. project each sample with the main viewProj into screen uv;
//   4. compare LINEAR VIEW-SPACE distances against the scene depth -
//      never raw NDC words against world meters (reversed-Z NDC is
//      nonlinear; the hit test must live in one consistent linear unit:
//      dist = near / max(ndcZ, eps), the ao.hlsli AoViewDist identity);
//   5. accept a hit when the sampled surface sits IN FRONT of the march
//      point (delta > startBias: rejects the receiver's own plane) but
//      not far in front (delta < thickness: the thin-occluder band) -
//      write 0 (occluded) or 1 (visible, the default).
//
// Conventions (the engine standards - see ssgi_diffuse_trace.cs.hlsl's
// coordinate audit): framebuffer y-down NDC (top = -1), uv = ndc*0.5+0.5;
// reversed-Z (1 = near, 0 = far/sky, clear 0); background rejection via
// DofIsBackgroundOrGarbage (NaN fails both range tests, garbage texels
// classify as sky and never occlude).
//
// What v1 deliberately does NOT do (documented gaps, not surprises):
//   - no screen-edge FADING: a march that leaves the frustum keeps
//     visibility 1 (untestable = lit, the conservative choice);
//   - no temporal filtering (the initial tuning table pins it off -
//     the IGN-dithered start offset is the noise mitigation for now);
//   - no normal-based grazing guard: a receiver facing away from the
//     sun marches anyway - its NdotL is 0, the wasted march is the
//     cost, the result is inert.
//
// The march direction is frame.env.sunDirection (TO the sun, the same
// source the fog-shadow ortho uses), NOT lights[0].direction: the pass
// runs before the light buffer is read, and the env sun is the single
// CPU-owned sun state the frame uniforms carry.
//
// Bindless storage write (binding 2 - the shared storage array, same
// slot space as textures[], the evsm_resolve pattern): the element type
// MUST match the target's format exactly or the write is undefined for
// the whole image - the visibility target is R32_Float (see
// contact_shadow.h's format note), so the element is float. The SAME
// bindless index is then SAMPLED by Main's lit body through textures[]
// (the dual visibility evsm_blur proves: one descriptor, both arrays).
[[vk::binding(2, 0)]]
RWTexture2D<float> contactStorage[];

// Must match Engine/Rendergraph/contact_shadow.h ContactShadowPush
// (56 bytes). uint64 BDA leads, all-scalar after (the spirv-val
// straddling-vector hazard discipline).
struct ContactShadowPush
{
    GPU_PTR_READ(FrameUniforms) frameUniforms;
    uint depthTextureIndex;  // full-res opaque depth (the prepass target)
    uint outImageIndex;      // bindless storage index of the visibility
    uint outWidth;           // visibility extent (the dispatch domain)
    uint outHeight;
    float invDepthW;         // 1 / FULL-res depth extent
    float invDepthH;
    float maxDistance;       // march length (m, world units)
    float thickness;         // thin-occluder acceptance band (m)
    float startBias;         // ray start offset + self-hit delta floor (m)
    uint steps;              // march sample count (4..32)
    uint samplerIndex;       // point/clamp (hard depth taps, no filtering)
    uint pad0;
};

GPU_PUSH(ContactShadowPush, sp);

[numthreads(8, 8, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= sp.outWidth || tid.y >= sp.outHeight)
        return;

    const FrameUniforms frame = sp.frameUniforms.Get();
    const float2 invOut = float2(1.0f / float(sp.outWidth),
        1.0f / float(sp.outHeight));

    // The receiver: visibility-texel center -> full-res uv window ->
    // y-down NDC -> world. Point tap (a hard occluder test wants exact
    // texels; a filtered depth tap blends unrelated surfaces).
    const float2 uv = (float2(tid.xy) + 0.5f) * invOut;
    Texture2D depthTex = textures[NonUniformResourceIndex(sp.depthTextureIndex)];
    SamplerState hardSmp = samplers[NonUniformResourceIndex(sp.samplerIndex)];
    const float ndcD = depthTex.SampleLevel(hardSmp, uv, 0).r;

    // Sky/garbage receivers have nothing to shadow: fully lit (never 0 -
    // a background write of 0 would shadow the sky dome's sun glow).
    float visibility = 1.0f;
    if (DofIsBackgroundOrGarbage(ndcD))
    {
        contactStorage[sp.outImageIndex][tid.xy] = visibility;
        return;
    }

    const float2 ndc = uv * 2.0f - 1.0f;
    float4 world = mul(frame.invViewProj, float4(ndc, ndcD, 1.0f));
    const float3 worldPos = world.xyz / max(world.w, 1e-9f);

    const float3 toSun = normalize(frame.env.sunDirection.xyz + 1e-9f);
    const float nearPlane = max(frame.nearPlane, 1e-4f);
    const float stepSize = max(sp.maxDistance, 1e-4f) / float(max(sp.steps, 1u));

    // The march: fixed step, IGN-free v1 (the initial tuning table - no
    // temporal accumulation to converge, so a per-pixel dither would only
    // shimmer). [loop]: steps is a push uniform, not a compile constant.
    [loop]
    for (uint i = 1u; i <= sp.steps; ++i)
    {
        const float3 p = worldPos
            + toSun * (float(i) * stepSize + sp.startBias);

        const float4 clip = mul(frame.viewProj, float4(p, 1.0f));
        // Behind the camera (w <= 0): untestable, stay lit.
        if (clip.w <= 0.0f)
            break;

        const float2 tapUv = (clip.xy / clip.w) * 0.5f + 0.5f;
        // Offscreen: untestable, stay lit (v1 has no edge fade).
        if (any(tapUv < 0.0f) || any(tapUv > 1.0f))
            break;

        const float tapD = depthTex.SampleLevel(hardSmp, tapUv, 0).r;
        // Sky taps never occlude (NaN fails the range test too).
        if (DofIsBackgroundOrGarbage(tapD))
            continue;

        // LINEAR view-space distances on both sides of the comparison
        // (the reversed-Z identity dist = near / ndcZ): the march point
        // and the sampled surface measured in the same meters.
        const float rayDist = nearPlane / max(clip.z / clip.w, kDofBackgroundEps);
        const float sceneDist = nearPlane / max(tapD, kDofBackgroundEps);
        const float delta = rayDist - sceneDist;

        // In front of the march point (delta > 0), past the self-hit
        // floor, inside the thin-occluder band: the sun is blocked.
        if (delta > sp.startBias && delta < sp.thickness)
        {
            visibility = 0.0f;
            break;
        }
    }

    contactStorage[sp.outImageIndex][tid.xy] = visibility;
}
