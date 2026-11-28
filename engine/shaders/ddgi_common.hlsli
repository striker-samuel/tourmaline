// SPDX-License-Identifier: MIT
#pragma once
#include "gpu_platform.hlsli"

static const uint DDGI_IRR_RES = 8;
static const uint DDGI_DIST_RES = 16;
// Each history record ends with the offset used to trace that probe.
static const uint DDGI_IRR_BINS = DDGI_IRR_RES * DDGI_IRR_RES;
static const uint DDGI_IRR_STRIDE = DDGI_IRR_BINS + 1;
static const float DDGI_PI = 3.14159265358979323846;
struct DdgiRay {
    float3 direction;
    float distance;
    float3 radiance;
    uint flags;
};
struct DdgiRayBuffer { DdgiRay values[1]; };
struct DdgiIrrBuffer { float4 values[1]; };
struct DdgiDistanceBuffer { float2 values[1]; };
// Per-probe candidate relocation offset (xyz = world offset, w = 0).
// Hardware RT writes it before tracing; SDF uses zero offsets.
struct DdgiRelocationBuffer { float4 values[1]; };
// Per-TLAS-instance compact material (published by BuildRayTracingWorld,
// indexed by the TLAS instance custom index).
struct RayMaterial {
    float4 albedoEmissive; // rgb = flat baseColor, w = emissive scalar
    float4 pbr;            // x = metallic, y = roughness
    uint firstIndex;       // absolute index-buffer offset (hit-normal fetch)
    uint baseVertex;       // absolute vertex offset (hit-normal fetch)
};
struct RayMaterialBuffer { RayMaterial items[1]; };
// Shared static vertex positions (R32G32B32, 12 bytes/vertex) and indices
// (uint32, 4 bytes/index) for the hardware-RT hit-normal fetch. These are the
// same monoblocks the BLAS geometry references, so firstIndex/baseVertex map
// a committed triangle straight to its three vertices.
struct GpuPositions { float3 values[1]; };
struct GpuIndices { uint values[1]; };
// One probe volume's lookup descriptor (mirror of smvk::ddgi::VolumeDesc,
// 64 bytes). Carries both history-slot BDAs; the lit shader picks the
// published slot via DdgiParams::slot.
struct DdgiVolumeDesc {
    GPU_PTR_READ(DdgiIrrBuffer) irradiance[2];
    GPU_PTR_READ(DdgiDistanceBuffer) distance[2];
    float originX, originY, originZ;
    float spacing;
    uint dimX, dimY, dimZ;
    uint pad;
};
struct DdgiVolumeDescBuffer { DdgiVolumeDesc values[1]; };
// The lit shader's DDGI sampling params (mirror of smvk::ddgi::DdgiParams,
// 32 bytes) — reached through FrameUniforms::ddgiParams.Get().
struct DdgiParams {
    GPU_PTR_READ(DdgiVolumeDescBuffer) volumes;
    uint volumeCount;
    uint slot;
    float strength;
    uint enabled;
    uint debug;
    float crossFade;
};


float2 DdgiSign(float2 v) {
    return float2(v.x >= 0 ? 1 : -1, v.y >= 0 ? 1 : -1);
}
float2 DdgiOctEncode(float3 d) {
    d /= abs(d.x) + abs(d.y) + abs(d.z);
    float2 p = d.xy;
    if (d.z < 0) p = (1 - abs(p.yx)) * DdgiSign(p);
    return p * 0.5 + 0.5;
}
float3 DdgiOctDecode(float2 uv) {
    float2 p = uv * 2 - 1;
    float3 d = float3(p, 1 - abs(p.x) - abs(p.y));
    if (d.z < 0) d.xy = (1 - abs(d.yx)) * DdgiSign(d.xy);
    return normalize(d);
}
float3 DdgiBinDirection(uint bin, uint resolution) {
    return DdgiOctDecode((float2(bin % resolution, bin / resolution) + 0.5) / resolution);
}
// Reflect across octahedral seams rather than clamping unrelated directions.
uint DdgiOctTap(int2 p, uint res) {
    const int n = int(res);
    if (p.x < 0 || p.x >= n) {
        p.x = p.x < 0 ? -1 - p.x : 2 * n - 1 - p.x;
        p.y = n - 1 - p.y;
    }
    if (p.y < 0 || p.y >= n) {
        p.y = p.y < 0 ? -1 - p.y : 2 * n - 1 - p.y;
        p.x = n - 1 - p.x;
    }
    return uint(p.x) + uint(p.y) * res;
}
void DdgiOctBilinearTaps(float3 dir, uint res, out uint bin[4], out float w[4])
{
    const float2 g = DdgiOctEncode(dir) * float(res) - 0.5f;
    const int2 b = int2(floor(g));
    const float2 f = g - float2(b);
    bin[0] = DdgiOctTap(b, res);
    bin[1] = DdgiOctTap(b + int2(1, 0), res);
    bin[2] = DdgiOctTap(b + int2(0, 1), res);
    bin[3] = DdgiOctTap(b + int2(1, 1), res);
    w[0] = (1.0f - f.x) * (1.0f - f.y);
    w[1] = f.x * (1.0f - f.y);
    w[2] = (1.0f - f.x) * f.y;
    w[3] = f.x * f.y;
}
float3 DdgiHistoryOffset(GPU_PTR_READ(DdgiIrrBuffer) history, uint probe) {
    return history.Get().values[probe * DDGI_IRR_STRIDE + DDGI_IRR_BINS].xyz;
}
bool DdgiSameOrigin(float3 a, float3 b) {
    // Never carry moments measured from a different ray origin.
    return all(a == b);
}
float DdgiFacing(float3 normal, float3 toProbe) {
    const float d2 = dot(toProbe, toProbe);
    if (d2 <= 1e-10f)
        return 1.0f;
    const float facing = saturate(0.5f + 0.5f * dot(normal, toProbe * rsqrt(d2)));
    return facing * facing;
}
uint3 DdgiGridCoord(uint p, uint3 dims) {
    return uint3(p % dims.x, (p / dims.x) % dims.y, p / (dims.x * dims.y));
}
float3 DdgiRayDirection(uint ray, uint count, uint updateIndex) {
    float z = 1 - 2 * (float(ray) + 0.5) / float(count);
    float radius = sqrt(max(0, 1 - z*z));
    float phi = 2 * DDGI_PI * frac(float(ray) * 0.61803398875 +
                                   float(updateIndex % 4096) * 0.754877666);
    // Alternate axes to avoid a fixed polar sampling pattern.
    float3 d = float3(radius * cos(phi), radius * sin(phi), z);
    if (updateIndex % 3 == 1) d = d.yzx;
    if (updateIndex % 3 == 2) d = d.zxy;
    return d;
}
float DdgiVisibility(float2 moments, float distance) {
    if (distance <= moments.x) return 1;
    float variance = max(moments.y - moments.x * moments.x, 1e-5);
    float delta = distance - moments.x;
    float v = variance / (variance + delta * delta);
    return v*v*v;
}

// Incremental tracing: each frame only the scrolled-in probes + a rotating
// 1/N subset of carried probes are re-traced; the blend copies the rest.
static const uint DDGI_TRACE_SUBSET = 4;

// Toroidal carry decision: probe at grid coord `coord` lived at `coord +
// delta` in the previous grid. Returns true (and the previous-frame linear
// probe index) when that world position was inside the old grid.
bool DdgiProbeCarried(int3 coord, int3 delta, uint3 dims, out uint oldProbe)
{
    const int3 oldCoord = coord + delta;
    const bool carry = all(oldCoord >= int3(0, 0, 0)) && all(oldCoord < int3(dims));
    oldProbe = carry ? uint(oldCoord.x) + uint(oldCoord.y) * dims.x
        + uint(oldCoord.z) * dims.x * dims.y : 0u;
    return carry;
}

// Rotating-subset trace decision: a carried probe is re-traced only when its
// residue matches the current phase (new/scrolled-in probes always trace).
bool DdgiProbeTraced(uint probe, uint updateIndex)
{
    return (probe % DDGI_TRACE_SUBSET) == (updateIndex % DDGI_TRACE_SUBSET);
}
