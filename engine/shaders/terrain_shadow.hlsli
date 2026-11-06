// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Tiled-terrain heightfield shadows (see Rendergraph/terrain.h).
//
// The R16 height atlas (one 4096 slot, h01 in .r, linear clamp sampler 2)
// backs two queries:
//   - TerrainAtlasHeight01: single texel height for the far VS + CPU parity;
//   - TerrainRaymarchShadow: HZD-style distant self-shadow. Marches the sun
//     ray in world XZ over the atlas; a texel surface above the ray
//     occludes, softened by the height margin over distance. Starts past
//     the receiver neighborhood (kMarchT0) — near-field shadowing belongs
//     to the shadowmap receive; this covers inter-ridge landscape shadows
//     out to kMarchDist, including beyond the last cascade.
//
// Filtering note: the atlas stores TRUE 16-bit heights (not packed
// channels), so hardware linear filtering interpolates heights correctly.

static const float kTerrainMarchDist = 1500.0f;
static const float kTerrainMarchT0 = 30.0f;
static const float kTerrainMarchSoft = 3.0f;
static const float kTerrainMarchBias = 1.5f;

float TerrainAtlasHeight01(uint slot, float2 uv)
{
    return textures[NonUniformResourceIndex(slot)]
        .SampleLevel(samplers[2], uv, 0).r;
}

float TerrainRaymarchShadow(uint slot, float2 originXZ, float invWorldSize,
    float baseY, float range, float3 worldPos, float3 sunDir, int steps)
{
    if (sunDir.y <= 0.02f || steps <= 0)
        return 1.0f;
    if (worldPos.x < originXZ.x || worldPos.z < originXZ.y
        || worldPos.x > originXZ.x + 1.0f / max(invWorldSize, 1e-9f)
        || worldPos.z > originXZ.y + 1.0f / max(invWorldSize, 1e-9f))
        return 1.0f; // outside the height world: no occluders
    float occ = 0.0f;
    [loop]
    for (int i = 1; i <= 64; ++i)
    {
        if (i > steps)
            break;
        float t = kTerrainMarchT0
            + (kTerrainMarchDist - kTerrainMarchT0) * ((float)i / (float)steps);
        float3 rp = worldPos + sunDir * t;
        float2 uv = (rp.xz - originXZ) * invWorldSize;
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
            break;
        float terrainY = baseY + TerrainAtlasHeight01(slot, uv) * range;
        float d = terrainY + kTerrainMarchBias - rp.y;
        occ = max(occ, d / max(t, 1.0f));
    }
    return saturate(1.0f - occ * kTerrainMarchSoft);
}


// --------------------------------------------------------------------------
// Cooked terrain+placed-object conservative max-height traversal.
//
// Packed R16 2048x1024 atlas. Level 2 = 4m footprints (1024^2 at x=0),
// level 3 = 8m (512^2 at x=1024), level 4 = 16m (256^2 at x=1536),
// ... level 6 = 64m. Each texel MAX-reduces every finer height below it.
// This is NOT ordinary texture mipmapping: average-filtered height would
// miss thin occluders. No hardware bilinear filtering of bounds.
//
// Each interval tests the minimum sun-ray Y within that horizontal cell;
// since sunDir.y > 0, that is the ray height at the ENTRY t. If even the
// maximum possible world height is below it, the entire interval is empty.
// Otherwise descend from 64m -> 16m -> 4m. The finest 4m column is
// conservative (may over-shadow thin silhouettes); a 1m/geometry test is
// needed for exact silhouettes. Near-range CSM still owns contact quality.
// --------------------------------------------------------------------------
uint TerrainOccLevelOffset(uint level)
{
    if (level <= 2u)
        return 0u;
    uint x = 1024u;
    [unroll]
    for (uint l = 3u; l < 12u; ++l)
        if (l < level)
            x += 4096u >> l;
    return x;
}

float TerrainOccHeight(uint slot, uint level, int2 cell, float baseY, float range)
{
    int dim = int(4096u >> level);
    if (any(cell < 0) || any(cell >= dim))
        return -1e30f;
    uint offX = TerrainOccLevelOffset(level);
    float h = textures[NonUniformResourceIndex(slot)]
        .Load(int3(cell.x + int(offX), cell.y, 0)).r;
    return baseY + h * range;
}

// Compute the next XZ cell boundary from a point epsilon past t.
// Positive t always advances, even when a ray passes exactly through
// a grid corner; callers clip to the parent interval.
float TerrainOccCellExit(float2 originXZ, float invWorldSize,
    float3 worldPos, float3 sunDir, uint level, float t,
    out int2 cell)
{
    float cellMeters = (1.0f / max(invWorldSize, 1e-9f))
        * (float(1u << level) / 4096.0f);
    float2 p = worldPos.xz + sunDir.xz * (t + 0.001f);
    cell = int2(floor((p - originXZ) / cellMeters));
    float2 boundary = originXZ + (float2(cell)
        + float2(sunDir.x > 0.0f ? 1.0f : 0.0f,
                 sunDir.z > 0.0f ? 1.0f : 0.0f)) * cellMeters;
    float dtX = abs(sunDir.x) > 1e-6f
        ? (boundary.x - (worldPos.x + sunDir.x * t)) / sunDir.x
        : 1e30f;
    float dtZ = abs(sunDir.z) > 1e-6f
        ? (boundary.y - (worldPos.z + sunDir.z * t)) / sunDir.z
        : 1e30f;
    return t + max(0.005f, min(dtX, dtZ));
}

bool TerrainOccFine(uint slot, float2 originXZ, float invWorldSize,
    float baseY, float range, float3 worldPos, float3 sunDir,
    float begin, float end)
{
    float t = begin;
    [loop]
    for (int i = 0; i < 64 && t < end; ++i)
    {
        int2 cell;
        float exitT = TerrainOccCellExit(originXZ, invWorldSize,
            worldPos, sunDir, 2u, t, cell);
        // At the smallest cooked footprint use the exact 4m cell max.
        // Only a candidate above the ray can block directional light.
        float maxY = TerrainOccHeight(slot, 2u, cell, baseY, range);
        if (maxY + kTerrainMarchBias >= worldPos.y + sunDir.y * t)
            return true;
        t = min(exitT, end);
    }
    return false;
}

bool TerrainOccMiddle(uint slot, float2 originXZ, float invWorldSize,
    float baseY, float range, float3 worldPos, float3 sunDir,
    float begin, float end)
{
    float t = begin;
    [loop]
    for (int i = 0; i < 64 && t < end; ++i)
    {
        int2 cell;
        float exitT = TerrainOccCellExit(originXZ, invWorldSize,
            worldPos, sunDir, 4u, t, cell);
        float maxY = TerrainOccHeight(slot, 4u, cell, baseY, range);
        if (maxY + kTerrainMarchBias >= worldPos.y + sunDir.y * t
            && TerrainOccFine(slot, originXZ, invWorldSize,
                baseY, range, worldPos, sunDir, t, min(exitT, end)))
            return true;
        t = min(exitT, end);
    }
    return false;
}

float TerrainHierarchicalShadow(uint slot, float2 originXZ, float invWorldSize,
    float baseY, float range, float3 worldPos, float3 sunDir)
{
    if (sunDir.y <= 0.02f || invWorldSize <= 0.0f)
        return 1.0f;
    const float worldSize = 1.0f / invWorldSize;
    if (any(worldPos.xz < originXZ)
        || any(worldPos.xz >= originXZ + worldSize))
        return 1.0f;
    float t = kTerrainMarchT0;
    [loop]
    for (int i = 0; i < 96 && t < kTerrainMarchDist; ++i)
    {
        int2 cell;
        float exitT = TerrainOccCellExit(originXZ, invWorldSize,
            worldPos, sunDir, 6u, t, cell);
        const int dim = 4096 >> 6;
        if (any(cell < 0) || any(cell >= dim))
            break; // sunlight left the cooked world
        float maxY = TerrainOccHeight(slot, 6u, cell, baseY, range);
        if (maxY + kTerrainMarchBias >= worldPos.y + sunDir.y * t
            && TerrainOccMiddle(slot, originXZ, invWorldSize,
                baseY, range, worldPos, sunDir, t,
                min(exitT, kTerrainMarchDist)))
            return 0.0f;
        t = min(exitT, kTerrainMarchDist);
    }
    return 1.0f;
}
