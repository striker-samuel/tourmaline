// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
#include "gpu_platform.hlsli"

// Static-only adaptive-octree distance field (ADF) fallback for the SSSR
// intersect pass. The CPU uploader packs a linear octree (nodes/children/
// leaves) + immutable inverse transforms from the mesh-local .geometry.sdf
// assets. Leaves carry a signed distance (int16, normalized by maxDistance)
// and an optional texture-baked RGB565 base color. All distances/epsilons
// outside the helper are WORLD metres.

#define STATIC_SDF_MAX_INSTANCES 128u
#define STATIC_SDF_MAX_NODE_WORDS 33554432u
#define STATIC_SDF_MAX_CHILD_WORDS 33554432u
#define STATIC_SDF_MAX_LEAF_WORDS 33554432u
#define STATIC_SDF_MAX_MATERIALS 4096u

struct StaticSdfGpuInstance
{
    float4 invRow0;    // inverse mesh-to-world ROW 0 (incl translation)
    float4 invRow1;    // ROW 1
    float4 invRow2;    // ROW 2
    float4 originSize; // xyz = root AABB min corner, w = root half-extent
    uint4 material;    // x = packed-material palette index, y = maxDepth
    uint4 meta;        // x = node offset, y = child offset, z = leaf offset,
                       // w = asuint(maxDistance)
}; // 96 bytes; C++ static_assert in static_sdf_gpu.h

struct StaticSdfInstanceBuffer
{
    StaticSdfGpuInstance items[STATIC_SDF_MAX_INSTANCES];
};
struct StaticSdfNodeWords
{
    uint words[STATIC_SDF_MAX_NODE_WORDS];
};
struct StaticSdfChildWords
{
    int words[STATIC_SDF_MAX_CHILD_WORDS];
};
struct StaticSdfLeafWords
{
    uint words[STATIC_SDF_MAX_LEAF_WORDS];
};
struct StaticSdfMaterialWords
{
    uint words[STATIC_SDF_MAX_MATERIALS];
};

// Scene descriptor uploaded by StaticSdfGpuScene. A single pointer in the
// SSR push fits Vulkan's guaranteed 128-byte minimum.
struct StaticSdfSceneHeader
{
    GPU_PTR_READ(StaticSdfInstanceBuffer) instances;
    GPU_PTR_READ(StaticSdfNodeWords) nodes;
    GPU_PTR_READ(StaticSdfChildWords) children;
    GPU_PTR_READ(StaticSdfLeafWords) leaves;
    GPU_PTR_READ(StaticSdfMaterialWords) materials;
    uint instanceCount;
    uint materialCount;
    uint maxSteps;
    float epsilon;
    float originBias;
};

// Decode a PackRgb565 word (see Engine/Asset/sdf.h). Returns linear color.
float3 UnpackStaticSdfRgb565(uint c)
{
    return float3(float(c & 31u), float((c >> 5) & 63u), float((c >> 11) & 31u)) /
           float3(31.0, 63.0, 31.0);
}

// Decode a PackSdfMaterial word (see Engine/Asset/sdf.h). Emissive color
// is the base color scaled by the emissive scalar (compact approximation).
void UnpackStaticSdfMaterial(uint packed,
                             out float3 baseColor,
                             out float roughness,
                             out float metallic,
                             out float emissive)
{
    baseColor = float3(float(packed & 31u),
                       float((packed >> 5) & 63u),
                       float((packed >> 11) & 31u)) /
                float3(31.0, 63.0, 31.0);
    roughness = float((packed >> 16) & 31u) / 31.0;
    metallic = float((packed >> 21) & 31u) / 31.0;
    emissive = float((packed >> 26) & 63u) / 63.0;
}

// Stable pseudo-random color for an integer id (material / mesh id debug
// modes). Deterministic across frames, distinct for nearby ids.
float3 StaticSdfHashColor(uint n)
{
    uint h = n;
    h = (h ^ 61u) ^ (h >> 16u);
    h = h * 0x27d4eb2du;
    h = h ^ (h >> 15u);
    return float3(float(h & 255u), float((h >> 8) & 255u), float((h >> 16) & 255u)) / 255.0f;
}

struct StaticSdfRayHit
{
    float t;
    float3 hitPoint;
    float3 normal;
    float3 baseColor;
    float roughness;
    float metallic;
    float emissive;
    float worldVoxel;
    uint materialIndex; // global packed-material palette index
    uint instanceIndex; // StaticSdfGpuInstance array index
    uint localLeafIndex; // mesh-local leaf; not a persistent instance key
    uint normalSide; // facing flip relative to SDF gradient
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

// Descend the octree to the leaf containing `local`, returning the leaf's
// signed distance (normalized * maxDistance) and texture-baked color. Empty
// cells (no surface) return maxDistance.
struct StaticSdfSampleCache
{
    bool live;
    float3 lo;
    float3 hi;
    float dist;
    float3 color;
    uint localLeafIndex; // 0xffffffff for absent child
};

void StaticSdfOctreeSampleCached(StaticSdfGpuInstance v, float3 local,
    GPU_PTR_READ(StaticSdfNodeWords) nodes,
    GPU_PTR_READ(StaticSdfChildWords) children,
    GPU_PTR_READ(StaticSdfLeafWords) leaves,
    inout StaticSdfSampleCache cache,
    out float dist, out float3 color)
{
    // Half-open bounds encode exactly the >= split decisions below.
    // Keep the outer bounds infinite: normal taps may lie outside the root.
    if (cache.live && all(local >= cache.lo) && all(local < cache.hi))
    {
        dist = cache.dist;
        color = cache.color;
        return;
    }
    cache.live = false;
    cache.localLeafIndex = 0xffffffffu;
    const float infinity = asfloat(0x7f800000u);
    cache.lo = -infinity;
    cache.hi = infinity;
    const float maxDist = asfloat(v.meta.w);
    float3 c = v.originSize.xyz + v.originSize.w; // root center
    float h = v.originSize.w;
    uint node = 0u;
    dist = maxDist;
    color = float3(0.0f, 0.0f, 0.0f);
    for (uint depth = 0u; depth <= v.material.y; ++depth)
    {
        const uint data = nodes.Get().words[v.meta.x + node];
        if ((data & 0x80000000u) != 0u)
        {
            cache.localLeafIndex = data & 0x7FFFFFFFu;
            const uint leaf = leaves.Get().words[v.meta.z + cache.localLeafIndex];
            int d = int(leaf >> 16);
            d = d >= 32768 ? d - 65536 : d; // sign-extend the int16 distance
            dist = float(d) / 32767.0f * maxDist;
            color = UnpackStaticSdfRgb565(uint(leaf & 0xFFFFu));
            cache.dist = dist;
            cache.color = color;
            cache.live = true;
            return;
        }
        const uint childBase = v.meta.y + (data & 0x7FFFFFFFu);
        const uint octant = (local.x >= c.x ? 1u : 0u) |
                            (local.y >= c.y ? 2u : 0u) |
                            (local.z >= c.z ? 4u : 0u);
        // Accumulate the actual split planes, including the last split
        // for an absent child. Equality always belongs to the high side.
        if (octant & 1u) cache.lo.x = max(cache.lo.x, c.x);
        else             cache.hi.x = min(cache.hi.x, c.x);
        if (octant & 2u) cache.lo.y = max(cache.lo.y, c.y);
        else             cache.hi.y = min(cache.hi.y, c.y);
        if (octant & 4u) cache.lo.z = max(cache.lo.z, c.z);
        else             cache.hi.z = min(cache.hi.z, c.z);
        const int child = children.Get().words[childBase + octant];
        if (child < 0)
        {
            cache.dist = dist;
            cache.color = color;
            cache.live = true;
            return; // empty cell: no surface along this branch
        }
        node = uint(child);
        c.x += (octant & 1u) ? h * 0.5f : -h * 0.5f;
        c.y += (octant & 2u) ? h * 0.5f : -h * 0.5f;
        c.z += (octant & 4u) ? h * 0.5f : -h * 0.5f;
        h *= 0.5f;
    }
}

// Standalone samples retain the original entry point.
void StaticSdfOctreeSample(StaticSdfGpuInstance v, float3 local,
    GPU_PTR_READ(StaticSdfNodeWords) nodes,
    GPU_PTR_READ(StaticSdfChildWords) children,
    GPU_PTR_READ(StaticSdfLeafWords) leaves,
    out float dist, out float3 color)
{
    StaticSdfSampleCache cache = (StaticSdfSampleCache)0;
    StaticSdfOctreeSampleCached(v, local, nodes, children, leaves, cache, dist, color);
}

StaticSdfRayHit StaticSdfTrace(float3 worldOrigin, float3 worldDir,
    float maxWorldDistance, float worldEpsilon, uint stepBudget,
    uint instanceCount, uint materialCount,
    GPU_PTR_READ(StaticSdfInstanceBuffer) instances,
    GPU_PTR_READ(StaticSdfNodeWords) nodes,
    GPU_PTR_READ(StaticSdfChildWords) children,
    GPU_PTR_READ(StaticSdfLeafWords) leaves,
    GPU_PTR_READ(StaticSdfMaterialWords) materials)
{
    StaticSdfRayHit best = (StaticSdfRayHit)0;
    best.t = maxWorldDistance;
    if (instanceCount == 0u || maxWorldDistance <= 0.0f) return best;

    [loop] for (uint index = 0; index < min(instanceCount, STATIC_SDF_MAX_INSTANCES); ++index)
    {
        StaticSdfGpuInstance v = instances.Get().items[index];
        float3 origin = StaticSdfPointLocal(v, worldOrigin);
        float3 dir = StaticSdfDirLocal(v, worldDir);
        float speed = length(dir);
        if (speed < 1e-10f || !isfinite(speed)) continue;
        const float extent = v.originSize.w * 2.0f;
        const float3 boxMin = v.originSize.xyz;
        const float3 boxMax = boxMin + extent;
        float beginT, endT;
        if (!StaticSdfRayAabb(origin, dir, boxMin, boxMax, beginT, endT))
            continue;
        endT = min(endT, best.t);
        // Finest leaf cell drives the convergence floor (the quantized
        // leaf distance converges to ~0.75 of the finest cell).
        const float finest = v.originSize.w / float(uint(1) << v.material.y);
        const float epsLocal = max(worldEpsilon * speed, 0.75f * finest);
        float t = beginT;
        // Cache is scoped to one instance; never reuse it across transforms.
        StaticSdfSampleCache sampleCache = (StaticSdfSampleCache)0;
        [loop] for (uint step = 0u; step < stepBudget && t <= endT; ++step)
        {
            float3 p = origin + t * dir;
            float sd;
            float3 col;
            StaticSdfOctreeSampleCached(v, p, nodes, children, leaves, sampleCache, sd, col);
            const float absSd = abs(sd);
            if (absSd <= epsLocal)
            {
                if (t <= 0.5f * finest / speed)
                    break; // receiver's own shell
                // Normal taps change the cache; capture the actual hit leaf first.
                const uint hitLeaf = sampleCache.localLeafIndex;
                // Central-difference normal from the octree field.
                const float h = finest * 0.5f;
                float dx, cx;
                StaticSdfOctreeSampleCached(v, p + float3(h, 0, 0), nodes, children, leaves, sampleCache, dx, cx);
                float dy, cy;
                StaticSdfOctreeSampleCached(v, p + float3(0, h, 0), nodes, children, leaves, sampleCache, dy, cy);
                float dz, cz;
                StaticSdfOctreeSampleCached(v, p + float3(0, 0, h), nodes, children, leaves, sampleCache, dz, cz);
                float sx, sxx;
                StaticSdfOctreeSampleCached(v, p - float3(h, 0, 0), nodes, children, leaves, sampleCache, sx, sxx);
                float sy, syy;
                StaticSdfOctreeSampleCached(v, p - float3(0, h, 0), nodes, children, leaves, sampleCache, sy, syy);
                float sz, szz;
                StaticSdfOctreeSampleCached(v, p - float3(0, 0, h), nodes, children, leaves, sampleCache, sz, szz);
                float3 grad = float3(dx - sx, dy - sy, dz - sz);
                float3 n = grad.x * v.invRow0.xyz +
                           grad.y * v.invRow1.xyz +
                           grad.z * v.invRow2.xyz;
                float n2 = dot(n, n);
                n = n2 > 1e-15f ? n * rsqrt(n2) : -worldDir;
                const bool flipped = dot(n, worldDir) > 0.0f;
                if (flipped) n = -n;
                best.valid = true;
                best.t = t;
                best.hitPoint = worldOrigin + t * worldDir;
                best.normal = n;
                const uint matIdx =
                    materialCount > 0u ? min(v.material.x, materialCount - 1u) : 0u;
                float3 bc;
                float ro, me, em;
                UnpackStaticSdfMaterial(materials.Get().words[matIdx], bc, ro, me, em);
                // Prefer the leaf's texture-baked color; the leaf carries 0
                // when the sidecar was baked without color.
                if (dot(col, col) > 1e-6f)
                    bc = col;
                best.baseColor = max(bc, 0.0f);
                best.roughness = ro;
                best.metallic = me;
                best.emissive = em;
                best.materialIndex = matIdx;
                best.instanceIndex = index;
                best.localLeafIndex = hitLeaf;
                best.normalSide = flipped ? 1u : 0u;
                best.worldVoxel = finest / speed;
                break;
            }
            float safeDistance = max((absSd - 0.25f * finest) * 0.75f,
                epsLocal * 0.25f);
            t += safeDistance / speed;
        }
    }
    return best;
}
