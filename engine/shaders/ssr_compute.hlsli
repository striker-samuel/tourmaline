// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// SSR compute-chain shared types + the FidelityFX SSSR math ports (see
// FidelityFX-SDK-v1.1.4
// sdk/include/FidelityFX/gpu/sssr/ffx_sssr_intersect.h):
//   - SsrRayList / SsrCounter / SsrIndirectArgs / SsrRadiance: the
//     classify->args->intersect handoff buffers (BDA storage; vk::
//     BufferPointer needs struct pointees, so the scalar payloads ride
//     in single/fixed-cap structs)
//   - Pack/UnpackRayCoords: the SDK's packed ray format (x 15b, y 14b,
//     three 2x2-quad copy flags in the top bits)
//   - SsrSampleGGXVNDF / SsrSampleReflectionVector: Heitz's VNDF
//     sampling (jcgt 0007/04/01), the listing the SDK carries - the
//     stochastic lobe the old fixed cone jitter approximated. PORT
//     HISTORY: the original "verbatim port" of the sampler was the
//     D_GGX evaluation (a scalar - see the sampler's port-history
//     note) and the caller passed the wrong view hemisphere; both were
//     the root cause of the reflections-read-NADA era (every
//     reflection direction garbage, ~0.05% visible coverage), found
//     via the deterministic-mirror bypass + the contribution debug
//     attachment (d8491e1d's WIP note -> the fix).
//   - SsrOctDecode: the G-buffer oct normal decode — MOVED to
//     ssr_receiver.hlsli (the FS filters decode the packed receiver
//     normals too; this file includes it), shared by the classify's
//     validity path and the intersect's receiver/hit reads
//
// License note: the VNDF sampler + ray format are AMD FidelityFX SSSR
// (MIT-licensed, see the SDK headers); this file keeps their shape so
// the port stays auditable against the SDK.

#include "vertex_pack.hlsli"
#include "ssr_receiver.hlsli"

// Ray list: one packed uint per ray (halfW*halfH capacity — the classify
// compacts only reflective pixels; the worst case (everything shoots)
// fits by construction). The C++ sizes the buffer from the half extent;
// SSR_RAY_LIST_CAP (ssr_receiver.hlsli) is the declared indexing domain.
struct SsrRayList { uint rays[SSR_RAY_LIST_CAP]; };

// Classify->args handoff: [0] = the live ray count (classify appends,
// args reads + resets), [1] = the frozen count for this frame's
// intersect (its early-out bound). The C++ sizes 2 uints.
struct SsrRayCounter { uint count[2]; };

// VkDispatchIndirectCommand (x/y/z group counts) written by the
// indirect-args pass, consumed by the intersect's DispatchIndirect.
struct SsrIndirectArgs { uint xyz[3]; };

// Per-half-res-pixel radiance (float4: rgb = reflected HDR x Fresnel x
// fade x intensity (firefly-clamped), a = hit confidence). Written
// (0,0,0,0) by the classify for non-ray pixels, overwritten by the
// intersect for rays; the copy pass blits it into the raw target.
struct SsrRadiance { float4 v[SSR_RAY_LIST_CAP]; };

void SsrPackRayCoords(uint2 coord, bool copyH, bool copyV, bool copyD, out uint packed)
{
    packed = (coord.x & 32767u) | ((coord.y & 16383u) << 15)
        | ((copyH ? 1u : 0u) << 29) | ((copyV ? 1u : 0u) << 30)
        | ((copyD ? 1u : 0u) << 31);
}

void SsrUnpackRayCoords(uint packed, out uint2 coord, out bool copyH, out bool copyV, out bool copyD)
{
    coord.x = (packed >> 0) & 32767u;
    coord.y = (packed >> 15) & 16383u;
    copyH = ((packed >> 29) & 1u) != 0u;
    copyV = ((packed >> 30) & 1u) != 0u;
    copyD = ((packed >> 31) & 1u) != 0u;
}

// Mirror reflections (roughness < 0.0001) always shoot a full ray and
// never reduce (same rule as FidelityFX SSSR IsMirrorReflection).
bool SsrIsMirrorReflection(float roughness)
{
    return roughness < 0.0001f;
}

static const float SSR_PI = 3.14159265358979f;
static const float SSR_FLOAT_MAX = 3.402823466e+38f;
static const float SSR_MAX_REFLECTED = 8192.0f; // firefly cap (same as the old trace)

// Heitz's VNDF sampler (jcgt "Sampling the GGX Distribution of Visible
// Normals", the listing the SDK's SampleGGXVNDF carries):
//   Ve: view direction, TBN space (+Z = the surface normal), from the
//       surface TOWARD the eye, unit length.
//   alpha_x/alpha_y: GGX roughness parameters (isotropic callers pass
//       roughness twice).
//   U1, U2: uniform randoms (our IGN epochs).
// Returns the sampled microfacet normal Ne (PDF D_Ve(Ne)), TBN space,
// upper hemisphere - the distribution WEIGHTED BY VISIBILITY, so
// grazing receivers sample the far side of the lobe densely (the
// classic uniform-normal sampler wastes those samples).
//
// PORT HISTORY (the reflections-read-NADA hunt, d8491e1d's WIP note):
// the "verbatim port" this replaced was not a sampler at all - its
// body was the D_GGX EVALUATION returning the scalar a2/denom (HLSL
// silently splats a scalar to the float3 signature), so the intersect
// reflected around the fake "normal" (d,d,d) with d = a2/denom ~
// 1/(pi*alpha^2) (4e6 at the chrome ball's r=0.13): every reflection
// direction was garbage, 96-99% left the frustum (the rejection
// census), and the few accidental knight-area landings were the whole
// visible output (0.05% of pixels). Bypassing the sampler with the
// deterministic mirror (Rview = reflect(normalize(viewPos), N)) jumped
// the measured contribution ~50x (ssr_contrib mean 0.26 -> 14, max
// 126 -> 1156) - proof the marcher and the hit validation were right
// all along and the sampler body was the defect.
float3 SsrSampleGGXVNDF(float3 Ve, float alpha_x, float alpha_y, float U1, float U2)
{
    // Section 3.2: transform the view direction to the hemisphere
    // configuration (stretch by alpha so the anisotropic distribution
    // becomes spherical in this space).
    float3 Vh = normalize(float3(alpha_x * Ve.x, alpha_y * Ve.y, Ve.z));
    // Section 4.1: orthonormal basis about Vh (degenerate guard near +Z).
    float3 T1 = (Vh.z < 0.99999f)
        ? normalize(cross(Vh, float3(0.0f, 0.0f, 1.0f)))
        : float3(1.0f, 0.0f, 0.0f);
    float3 T2 = cross(T1, Vh);
    // Section 2.3: warp the uniform (U1, U2) onto the spherical cap.
    float r = sqrt(U1);
    float phi = 2.0f * SSR_PI * U2;
    float3 P = float3(r * cos(phi), r * sin(phi), sqrt(max(0.0f, 1.0f - U1)));
    // Section 4.3: sphere point to hemisphere (reorient toward Ve's
    // hemisphere - the visible-normal domain).
    float3 Nne = P.x * T1 + P.y * T2 + P.z * Vh;
    // Section 3.3: transform the normal back to the ellipsoid
    // configuration (the inverse of the Section 3.2 stretch).
    return normalize(float3(alpha_x * Nne.x, alpha_y * Nne.y, max(0.0f, Nne.z)));
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    float denom = NdotV * (1.0f - k) + k;
    return NdotV / max(denom, 1e-7f);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0f);
    float NdotL = max(dot(N, L), 0.0f);
    float ggx1 = GeometrySchlickGGX(NdotV, roughness);
    float ggx2 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(max(1.0f - cosTheta, 0.0f), 5.0f);
}

// Importance-sampled reflection direction (the SDK's
// SampleReflectionVector, verbatim): builds the TBN around the G-buffer
// normal, maps the view direction into it, VNDF-samples the microfacet
// normal, reflects, maps back to view space.
//
// INPUT CONTRACT (the SDK convention - the internal negation):
// view_direction = the CAMERA-TO-SURFACE incident ray (the normalize of
// the view-space surface position). The function negates it internally
// (TBN dot -view_direction) into the surface-toward-eye direction
// Ve that the Heitz sampler expects, then reflects -Ve back around the
// sampled normal for the outgoing (surface-to-scene) direction. The
// marcher's z plane is armed for exactly that outgoing sense.
// SIGN HISTORY (the same NADA hunt as the sampler body above): the old
// caller passed V (surface-to-camera) - the opposite of this contract -
// so the sampler sampled the OPPOSITE hemisphere and the outgoing
// direction came out pointing TOWARD the camera (approaching: the dead
// z-plane side of the marcher) instead of into the scene. The caller
// now passes normalize(viewPos); V stays surface-to-camera for the
// Fresnel term only.
float3 SsrSampleReflectionVector(float3 view_direction, float3 normal, float roughness, float U1, float U2)
{
    float3 U;
    float3 N = normal;
    if (abs(N.z) > 0.0f)
    {
        float k = sqrt(N.y * N.y + N.z * N.z);
        U = float3(0.0f, -N.z / k, N.y / k);
    }
    else
    {
        float k = sqrt(N.x * N.x + N.y * N.y);
        U = float3(N.y / k, -N.x / k, 0.0f);
    }
    float3 TBN_row0 = U;
    float3 TBN_row1 = cross(N, U);
    float3 TBN_row2 = N;
    float3 view_direction_tbn = float3(dot(TBN_row0, -view_direction),
                                       dot(TBN_row1, -view_direction),
                                       dot(TBN_row2, -view_direction));
    float3 sampled_normal_tbn = SsrSampleGGXVNDF(view_direction_tbn, roughness, roughness, U1, U2);
    float3 reflected_direction_tbn = reflect(-view_direction_tbn, sampled_normal_tbn);
    float3 TBN_col0 = float3(TBN_row0[0], TBN_row1[0], TBN_row2[0]);
    float3 TBN_col1 = float3(TBN_row0[1], TBN_row1[1], TBN_row2[1]);
    float3 TBN_col2 = float3(TBN_row0[2], TBN_row1[2], TBN_row2[2]);
    return float3(dot(TBN_col0, reflected_direction_tbn),
                  dot(TBN_col1, reflected_direction_tbn),
                  dot(TBN_col2, reflected_direction_tbn));
}

// Interleaved gradient noise (the SDK's blue-noise replacement —
// spatial IGN with the per-frame epoch decorrelating records).
float SsrIGN(float2 px, uint frameIndex)
{
    uint f = frameIndex & 255u;
    float2 q = px + float2(float(f) * 17.23f, float(f) * 9.71f);
    return frac(52.9829189 * frac(dot(q, float2(0.06711056, 0.00583715))));
}

// origin (view space) -> screen-uv-space (uv in [0,1] y-down, z = reversed-Z depth).
float3 SsrProjectPosition(float3 origin, float4x4 proj)
{
    float4 p = mul(proj, float4(origin, 1.0f));
    p.xyz /= max(p.w, 1e-9f);
    p.xy = p.xy * 0.5f + 0.5f;
    return p.xyz;
}

// Screen-space ray direction from the view-space origin + direction
// (projects origin+direction and subtracts the origin's screen point).
float3 SsrProjectDirection(float3 origin, float3 direction, float3 screen_origin, float4x4 proj)
{
    float3 screen_pos = SsrProjectPosition(origin + direction, proj);
    return screen_pos - screen_origin;
}

// View-space position of a depth tap (exact, flip agnostic via invProj).
float3 SsrUnprojectView(float2 ndcXY, float ndcZ, float4x4 invProj)
{
    float4 v = mul(invProj, float4(ndcXY, ndcZ, 1.0f));
    return v.xyz / max(v.w, 1e-9f);
}

// Full-res pixel-center NDC (framebuffer y-down: top = -1 — NO y flip,
// unlike the SDK's D3D convention; matches ssao.frag.hlsl).
float2 SsrFullNdc(int2 fullPx, float2 invDepthSize)
{
    return float2((float(fullPx.x) + 0.5f) * invDepthSize.x * 2.0f - 1.0f,
                  (float(fullPx.y) + 0.5f) * invDepthSize.y * 2.0f - 1.0f);
}