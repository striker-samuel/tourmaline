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
