// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// SSR receiver metadata — the shared receiver-domain header for BOTH
// shader families (the compute chain + the FS filters):
//   - SsrPackReceiver/SsrUnpackReceiver: the half-res receiver buffer's
//     32-bit word (the thin G-buffer's RGBA8 channels packed — the
//     classifier-selected receiver of each full-res 2x2 block, written
//     by ssr_classify.cs.hlsl for EVERY in-range half-res pixel, rays
//     or not: blur/compose taps reach pixels that never shot).
//   - SsrOctDecode: the octahedral normal decode (MOVED here from
//     ssr_compute.hlsli so the blur/compose filters can decode the
//     packed normals without pulling the whole compute-chain header;
//     ssr_compute.hlsli includes this file — one definition, both
//     families, never two in one compilation unit).
//
// Why a buffer instead of re-reading the G-buffer: the blur's old
// receiver material read was the block's TOP-LEFT full-res texel
// (halfPx * 2) while the classifier selects the block's NEAREST valid
// texel (reverse-Z max) — a material-edge block borrowed a neighbor's
// bucket. The buffer carries the classifier's actual selection to
// every consumer (blur radius bucket + the filters' normals) at one
// 32-bit word per half-res pixel (~2 MB at 1080p), and the consumers
// stop re-doing the 4-tap depth scan entirely.

#include "vertex_pack.hlsli"

// Half-res pixel-count capacity for the classify->consumer handoff
// buffers (ray list + radiance in ssr_compute.hlsli, the receiver
// metadata below). CAP covers a 4096x4096 FULL-res extent (half
// 2048x2048): the old 2048*2048/4 silently under-covered 4K frames
// (half 1920x1080 = 2.07M > 1.05M declared — the radiance/raylist
// indexing was already past the struct extent). Declared extents cost
// no runtime memory (the C++ allocates from the actual extent); they
// are the BDA indexing domain, so they must cover the largest
// supported render.
#define SSR_RAY_LIST_CAP (2048u * 2048u)

// Per-half-res-pixel RECEIVER metadata (one packed uint — the pack
// helpers below): the classifier-selected receiver of each full-res
// 2x2 block (the thin G-buffer sample at the block's nearest-valid
// texel). Written by the classify for EVERY in-range pixel (including
// non-ray pixels — the blur/compose taps reach them);
// kSsrReceiverNoData is the no-G-buffer sentinel. Read by the blur
// (radius bucket + normals) and the composite (tap normals) — the
// consumers stop re-doing the block's 4-tap depth scan + the texel
// material read. The C++ sizes halfW*halfH uints (same as the ray
// list's worst case).
struct SsrReceiverData { uint v[SSR_RAY_LIST_CAP]; };

// Pack the thin G-buffer sample (RGBA8-unorm semantics: xy = oct
// normal, z = roughness, w = metal) into one 32-bit word. round() on
// the saturated channels matches the RGBA8_UNorm target's quantization
// (the pack must round-trip the same texel the G-buffer would have
// stored).
uint SsrPackReceiver(float4 gb)
{
    uint4 q = (uint4)round(saturate(gb) * 255.0f);
    return q.x | (q.y << 8) | (q.z << 16) | (q.w << 24);
}

float4 SsrUnpackReceiver(uint packed)
{
    return float4(
        float(packed & 255u),
        float((packed >> 8) & 255u),
        float((packed >> 16) & 255u),
        float((packed >> 24) & 255u)) / 255.0f;
}

// The no-data sentinel: the thin G-buffer's documented CLEAR value
// (0.5, 0.5, 1, 0) packed — decodes as normal +Z, roughness 1 (matte),
// metal 0, so the no-G-buffer fallback (MSAA frames, legacy hosts)
// reads as a safe non-reflector with a neutral normal (radius bucket 2
// + a normal weight of 1 against everything, i.e. depth-only filtering
// — exactly the old behavior). Precomputed as a literal because HLSL
// global const initializers want constant expressions: 0.5*255 rounds
// to 128, 1.0*255 to 255, 0.0 to 0.
static const uint kSsrReceiverNoData =
    128u | (128u << 8) | (255u << 16) | (0u << 24); // 0x00FF8080

// Normal-similarity weight for the bilateral filters (the blur's taps
// + the composite's upsample): ~1 for similarly oriented surfaces,
// falling to 0 as orientations diverge — two surfaces at nearly the
// SAME depth but different normals (a wall meeting a floor, a step's
// riser vs its tread) stay separated, where the depth weight alone
// cannot tell them apart. Both normals must be in the SAME space: the
// packed receivers are WORLD-space oct (the thin G-buffer's domain) —
// decode both, never rotate just one into view space. The 0.75..0.98
// smoothstep band is a tuning value, not a physical constant (raise
// the floor for tighter rejection if reflections bleed across creases;
// lower it if smooth curved surfaces tear apart).
float SsrNormalWeight(float3 centerN, float3 tapN)
{
    float nd = saturate(dot(centerN, tapN));
    return smoothstep(0.75f, 0.98f, nd);
}

// Octahedral decode of the G-buffer RG (the inverse of
// gbuffer.frag.hlsl OctEncodeNormal; same fold/sign as
// vertex_pack.hlsli OctDecodeNormal). Lived in ssr_compute.hlsli
// before the receiver buffer needed it in the FS filters — moved here
// (the include graph: ssr_compute.hlsli -> this file), shape unchanged.
float3 SsrOctDecode(float2 e01)
{
    float x = e01.x * 2.0f - 1.0f;
    float y = e01.y * 2.0f - 1.0f;
    float z = 1.0f - abs(x) - abs(y);
    if (z < 0.0f)
    {
        float tx = (1.0f - abs(y)) * OctSignH(x);
        float ty = (1.0f - abs(x)) * OctSignH(y);
        x = tx;
        y = ty;
    }
    float l2 = x * x + y * y + z * z;
    return (l2 > 1e-24f) ? float3(x, y, z) / sqrt(l2) : float3(0.0f, 0.0f, 1.0f);
}
