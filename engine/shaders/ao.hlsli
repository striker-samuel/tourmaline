// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Shared SSGI-chain AO helpers (raw pass, blur pass, compose pass — see
// Engine/Rendergraph/ssgi.h).
//
// Conventions (must match Engine/Rendergraph/ssgi.h + forwardplus.cs.hlsl):
//   - infinite reversed-Z projection: depth 1 = near, 0 = far/sky (clear 0)
//   - view distance (-z) = near / max(d, eps)
//   - background classification: d <= kAoBackgroundEps (never a second epsilon)
//
// The invariant this header enforces everywhere: a background pixel can
// never receive AO. Depth is the authority; color is irrelevant.
//
// The half-res AO texture (R16G16B16A16_UNorm) carries a SELF-CONSISTENT
// per-block representative triple — every stage reads and re-emits the
// same representation, so no stage ever re-derives a block's depth or
// normal from a corner full-res texel again:
//   r  = ao (1 = unoccluded)
//   g  = encoded representative view distance (AoEncodeRepDist: codes
//        0 .. 65534/65535 = valid normalized distance, 1.0 = the
//        RESERVED sky/sentinel code — legitimate far distances clamp
//        BELOW it so they can never alias "no data")
//   bc = octahedral representative normal, xy*0.5+0.5 (the raw pass's
//        own depth-derived block normal — the same 2x2 depth block the
//        g distance was averaged from, so the triple stays coherent)

static const float kAoBackgroundEps = 1e-6;
static const float kAoSkyDist = 1e6; // sentinel view distance for sky taps

bool AoIsBackground(float ndcDepth)
{
    return ndcDepth <= kAoBackgroundEps;
}

float AoViewDist(float ndcDepth, float nearPlane)
{
    // Infinite reversed-Z: view distance (-z) = near / d.
    return nearPlane / max(ndcDepth, kAoBackgroundEps);
}

// Relative-depth edge weight: 1 on the same surface, ~0 across a
// discontinuity. A geometry|sky step collapses to exp(-sharpness) at worst
// because sky taps carry the kAoSkyDist sentinel.
float AoEdgeWeight(float tapDist, float centerDist, float sharpness)
{
    float depthDiff = abs(tapDist - centerDist) / max(centerDist, 1e-3f);
    return exp(-depthDiff * max(sharpness, 0.0f));
}

// ---- representative-distance encoding (the .g channel contract) ----
// Normalized against the AO working range (maxDistance): the raw pass's
// fade already forces ao -> 1 beyond it, so clamping there is loss-free
// for every edge weight that follows. The TOP code 1.0 is RESERVED:
//   - encode: min(normalized, 65534/65535) — a legitimate (far) distance
//     can never produce the reserved code;
//   - guard/sky blocks write exactly 1.0;
//   - decode: 1.0 -> the kAoSkyDist sentinel, everything else -> meters.
// (A plain unorm16 channel without the reserved code would alias "far
// geometry" with "no data" — the ambiguity that would otherwise force a
// float channel.)
float AoEncodeRepDist(float viewDist, float maxDistance)
{
    float nd = viewDist / max(maxDistance, 1e-4f);
    return min(nd, 65534.0f / 65535.0f);
}

float AoDecodeRepDist(float g, float maxDistance)
{
    if (g >= 1.0f)
        return kAoSkyDist; // reserved sentinel code
    return g * max(maxDistance, 1e-4f);
}

// ---- octahedral helpers (same fold/sign as gbuffer.frag.hlsl's
// OctEncodeNormal and vertex_pack.hlsli's OctDecodeNormal — keep the
// three in sync). Never average ENCODED pairs across the fold: decode
// first, weight, normalize, re-encode (see the blur pass).
float2 AoOctEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 e = (n.z >= 0.0f) ? n.xy
        : float2((1.0f - abs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f),
                 (1.0f - abs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f));
    return e * 0.5f + 0.5f;
}

float3 AoOctDecode(float2 oct01)
{
    float x = oct01.x * 2.0f - 1.0f;
    float y = oct01.y * 2.0f - 1.0f;
    // z keeps its (negative) pre-fold value (see OctDecodeNormal).
    float z = 1.0f - abs(x) - abs(y);
    if (z < 0.0f)
    {
        float tx = (1.0f - abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        float ty = (1.0f - abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = tx;
        y = ty;
    }
    float l2 = x * x + y * y + z * z;
    if (!(l2 > 1e-24f))
        return float3(0.0f, 0.0f, 1.0f);
    return float3(x, y, z) / sqrt(l2);
}

// Joint-bilateral normal term (the compose): 1 on coplanar surfaces, ~0
// across normal discontinuities (a floor|wall corner where the depth reps
// agree within the edge-weight slack). pow4 keeps the tap's AO on the
// surface it was computed for even when the depth reps straddle.
float AoNormalWeight(float3 tapN, float3 centerN)
{
    return pow(saturate(dot(tapN, centerN)), 4.0f);
}

// Guarded/sky block output: unoccluded, reserved sentinel distance, and a
// neutral +Z octahedral pair ((0.5, 0.5) encodes exactly +Z). static const:
// compile-time folded — a plain const global would ride the auto-generated
// $Globals cbuffer (see common.hlsli's spec-constant note).
static const float4 kAoSkyBlock = float4(1.0f, 1.0f, 0.5f, 0.5f);
