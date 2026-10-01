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
//   - SampleGGXVNDF: Heitz's VNDF sampler, verbatim (jcgt 0007/04/01) —
//     the stochastic lobe the old fixed cone jitter approximates
//   - SsrOctDecode: the G-buffer oct normal decode (shared by the
//     classify's validity path and the intersect's receiver/hit reads)
//
// License note: the VNDF sampler + ray format are AMD FidelityFX SSSR
// (MIT-licensed, see the SDK headers); this file keeps their shape so
// the port stays auditable against the SDK.

#include "vertex_pack.hlsli"

// Ray list: one packed uint per ray (halfW*halfH capacity — the classify
// compacts only reflective pixels; the worst case (everything shoots)
// fits by construction). The C++ sizes the buffer from the half extent.
#define SSR_RAY_LIST_CAP (2048u * 2048u / 4u)
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

// Octahedral decode of the G-buffer RG (the inverse of
// gbuffer.frag.hlsl OctEncodeNormal; same fold/sign as
// vertex_pack.hlsli OctDecodeNormal).
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

static const float SSR_PI = 3.14159265358979f;
static const float SSR_FLOAT_MAX = 3.402823466e+38f;
static const float SSR_MAX_REFLECTED = 8192.0f; // firefly cap (same as the old trace)

// Heitz VNDF sampler (verbatim port of the SDK's SampleGGXVNDF):
// Input Ve: view direction (TBN space, +Z = the surface normal)
// Input alpha_x, alpha_y: roughness parameters
// Input U1, U2: uniform random numbers (our IGN epochs)
// Output Ne: normal sampled with PDF D_Ve(Ne)
float3 SsrSampleGGXVNDF(float3 Ve, float alpha_x, float alpha_y, float U1, float U2)
{
    // Section 3.2: transforming the view direction to the hemisphere configuration
    float3 Vh = normalize(float3(alpha_x * Ve.x, alpha_y * Ve.y, Ve.z));
    float a = Vh.x * Vh.x;
    float a2 = a * a;
    float3 H = normalize(float3(Ve.x, Ve.y, 1.0f)); // Half-vector approximation for VNDF
    float NdotH = max(dot(Vh, H), 0.0f);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0f) + 1.0f);
    denom = SSR_PI * denom * denom;
    return a2 / max(denom, 1e-7f);
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