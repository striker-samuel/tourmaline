// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
#include "gpu_platform.hlsli"

// Static-only TSDF geometry fallback for the SSSR intersect pass. The
// CPU uploader packs samples (two signed 16-bit SNORMs per uint) and
// immutable inverse transforms from the mesh-local .geometry.sdf assets.
// All distances/epsilons outside the helper are WORLD metres.
//
// In contrast to screen-space depth traversal, ray/AABB and narrow-band
// sphere tracing can find geometry outside the camera frustum. The
// returned hit carries an approximate normal plus the mesh's constant
// material base-color factor; shading is handled by the SSR caller.
#define STATIC_SDF_MAX_INSTANCES 128u
#define STATIC_SDF_MAX_SAMPLE_WORDS 2097152u

struct StaticSdfGpuInstance
{
    float4 invRow0;       // inverse mesh-to-world ROW 0 (incl translation)
    float4 invRow1;       // ROW 1
    float4 invRow2;       // ROW 2
    float4 originVoxel;   // xyz first voxel center, w voxel width
    float4 surface;       // xyz material base color factor, w roughness
    uint4 meta;           // x resolution, y packed sample WORD offset,
                          // z signedDistance, w asuint(maxDistance)
}; // 96 bytes; C++ static_assert in static_sdf_gpu.h

struct StaticSdfInstanceBuffer
{
    StaticSdfGpuInstance items[STATIC_SDF_MAX_INSTANCES];
};
struct StaticSdfSampleWords
{
    uint words[STATIC_SDF_MAX_SAMPLE_WORDS];
};

// 32-byte scene descriptor uploaded by StaticSdfGpuScene. A single
// pointer in the SSR push fits Vulkan's guaranteed 128-byte limit.
struct StaticSdfSceneHeader
{
    GPU_PTR_READ(StaticSdfInstanceBuffer) instances;
    GPU_PTR_READ(StaticSdfSampleWords) samples;
    uint instanceCount;
    uint maxSteps;
    float epsilon;
    float originBias;
};

struct StaticSdfRayHit
{
    float t;
    float3 hitPoint;
    float3 normal;
    float3 baseColor;
    float roughness;
    float worldVoxel;
    bool valid;
};

float3 StaticSdfPointLocal(StaticSdfGpuInstance instance, float3 worldPos)
{
    float4 p = float4(worldPos, 1.0f);
    return float3(dot(instance.invRow0, p),
                  dot(instance.invRow1, p),
                  dot(instance.invRow2, p));
}

float3 StaticSdfDirLocal(StaticSdfGpuInstance instance, float3 worldDir)
{
    return float3(dot(instance.invRow0.xyz, worldDir),
                  dot(instance.invRow1.xyz, worldDir),
                  dot(instance.invRow2.xyz, worldDir));
}

bool StaticSdfRayAabb(float3 origin, float3 dir,
    float3 lo, float3 hi, out float nearT, out float farT)
{
    nearT = 0.0f;
    farT = 3.402823466e+38f;
    [unroll] for (int axis = 0; axis < 3; ++axis)
    {
        const float d = dir[axis];
        if (abs(d) < 1e-10f)
        {
            if (origin[axis] < lo[axis] || origin[axis] > hi[axis])
                return false;
        }
        else
        {
            float a = (lo[axis] - origin[axis]) / d;
            float b = (hi[axis] - origin[axis]) / d;
            if (a > b) { float tmp = a; a = b; b = tmp; }
            nearT = max(nearT, a);
            farT = min(farT, b);
            if (farT < nearT) return false;
        }
    }
    return true;
}

float StaticSdfTap(uint linearIndex, uint sampleWordOffset,
    GPU_PTR_READ(StaticSdfSampleWords) samples)
{
    uint packed = samples.Get().words[sampleWordOffset + (linearIndex >> 1u)];
    uint shift = (linearIndex & 1u) * 16u;
    // Sign-extend the chosen two's-complement R16 word.
    int signedSample = asint(packed << (16u - shift)) >> 16;
    return float(signedSample) / 32767.0f;
}

float StaticSdfDistance(StaticSdfGpuInstance instance, float3 p,
    GPU_PTR_READ(StaticSdfSampleWords) samples)
{
    const uint r = instance.meta.x;
    const float3 g = clamp((p - instance.originVoxel.xyz) /
        instance.originVoxel.w, 0.0f, float(r - 1u));
    uint3 i = uint3(g);
    uint3 i1 = min(i + 1u, uint3(r - 1u, r - 1u, r - 1u));
    float3 f = g - float3(i);
    const uint off = instance.meta.y;
    uint idx000 = i.x + r * (i.y + r * i.z);
    uint idx100 = i1.x + r * (i.y + r * i.z);
    uint idx010 = i.x + r * (i1.y + r * i.z);
    uint idx110 = i1.x + r * (i1.y + r * i.z);
    uint idx001 = i.x + r * (i.y + r * i1.z);
    uint idx101 = i1.x + r * (i.y + r * i1.z);
    uint idx011 = i.x + r * (i1.y + r * i1.z);
    uint idx111 = i1.x + r * (i1.y + r * i1.z);
    float a = lerp(StaticSdfTap(idx000, off, samples), StaticSdfTap(idx100, off, samples), f.x);
    float b = lerp(StaticSdfTap(idx010, off, samples), StaticSdfTap(idx110, off, samples), f.x);
    float c = lerp(StaticSdfTap(idx001, off, samples), StaticSdfTap(idx101, off, samples), f.x);
    float d = lerp(StaticSdfTap(idx011, off, samples), StaticSdfTap(idx111, off, samples), f.x);
    return lerp(lerp(a, b, f.y), lerp(c, d, f.y), f.z) *
        asfloat(instance.meta.w);
}

float3 StaticSdfGradient(StaticSdfGpuInstance instance, float3 p,
    GPU_PTR_READ(StaticSdfSampleWords) samples)
{
    float h = instance.originVoxel.w * 0.5f;
    float3 dx = float3(h, 0.0f, 0.0f);
    float3 dy = float3(0.0f, h, 0.0f);
    float3 dz = float3(0.0f, 0.0f, h);
    return float3(
        StaticSdfDistance(instance, p + dx, samples) -
        StaticSdfDistance(instance, p - dx, samples),
        StaticSdfDistance(instance, p + dy, samples) -
        StaticSdfDistance(instance, p - dy, samples),
        StaticSdfDistance(instance, p + dz, samples) -
        StaticSdfDistance(instance, p - dz, samples));
}

StaticSdfRayHit StaticSdfTrace(float3 worldOrigin, float3 worldDir,
    float maxWorldDistance, float worldEpsilon, uint stepBudget,
    uint instanceCount,
    GPU_PTR_READ(StaticSdfInstanceBuffer) instances,
    GPU_PTR_READ(StaticSdfSampleWords) samples)
{
    StaticSdfRayHit best = (StaticSdfRayHit)0;
    best.t = maxWorldDistance;
    if (instanceCount == 0u || maxWorldDistance <= 0.0f) return best;

    // Broad-phase AABBs are cheap; narrow-band steps run only on
    // intersecting boxes. Shortest world-space hit wins.
    [loop] for (uint index = 0; index < min(instanceCount, STATIC_SDF_MAX_INSTANCES); ++index)
    {
        StaticSdfGpuInstance v = instances.Get().items[index];
        float3 origin = StaticSdfPointLocal(v, worldOrigin);
        float3 dir = StaticSdfDirLocal(v, worldDir);
        float speed = length(dir);
        if (speed < 1e-10f || !isfinite(speed)) continue;
        const float extent = float(v.meta.x - 1u) * v.originVoxel.w;
        float3 boxMin = v.originVoxel.xyz;
        float3 boxMax = boxMin + extent;
        float beginT, endT;
        if (!StaticSdfRayAabb(origin, dir, boxMin, boxMax, beginT, endT))
            continue;
        endT = min(endT, best.t);
        // Trilinear, int16-quantized fields need a 0.75-voxel
        // convergence floor (matching the CPU reference); otherwise
        // surface samples between voxel planes never hit.
        const float epsLocal = max(worldEpsilon * speed,
            0.75f * v.originVoxel.w);
        float t = beginT;
        // The SDF is trilinear/quantized and distances saturate outside
        // its narrow band. Use a conservative factor and a voxel floor;
        // the AABB clips every step to the actually represented domain.
        [loop] for (uint step = 0u; step < stepBudget && t <= endT; ++step)
        {
            float3 p = origin + t * dir;
            float sd = StaticSdfDistance(v, p, samples);
            float absSd = abs(sd); // unsigned sidecars are valid, too
            if (absSd <= epsLocal)
            {
                // Avoid immediately reflecting the receiver's own SDF
                // shell (baked fields are coarse relative to the SSR
                // origin bias). Continue to other static instances.
                if (t <= 0.5f * v.originVoxel.w / speed)
                    break;
                float3 grad = StaticSdfGradient(v, p, samples);
                float3 n = grad.x * v.invRow0.xyz +
                           grad.y * v.invRow1.xyz +
                           grad.z * v.invRow2.xyz;
                float n2 = dot(n, n);
                n = n2 > 1e-15f ? n * rsqrt(n2) : -worldDir;
                if (dot(n, worldDir) > 0.0f) n = -n;
                best.valid = true;
                best.t = t;
                best.hitPoint = worldOrigin + t * worldDir;
                best.normal = n;
                best.baseColor = max(v.surface.xyz, 0.0f);
                best.roughness = v.surface.w;
                best.worldVoxel = v.originVoxel.w / speed;
                break;
            }
            float safeDistance = max((absSd - 0.25f * v.originVoxel.w) * 0.75f,
                epsLocal * 0.25f);
            t += safeDistance / speed;
        }
    }
    return best;
}
