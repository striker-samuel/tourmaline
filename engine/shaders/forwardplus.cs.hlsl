// Forward+ tile light culling (point/spot; directional stays on CSM path)
// + decal culling (projected splats share the light tiling).
//
// One thread group per tile ([1,1,1], dispatched tileCountX x tileCountY).
// Each tile:
//   1. loads its 16x16 depth texels, tracks NDC min/max (reversed-Z:
//      1 = near, 0 = far/sky),
//   2. unprojects tile corners via invProj -> view-space side planes,
//   3. sphere-tests every light against the 6 tile planes,
//   4. writes the first maxLightsPerTile passing light indices + the true
//      (unclamped) pass count; the shading pass iterates at most
//      maxLightsPerTile entries, so overflow lights pop (accepted: spots
//      are uploaded to the light buffer before the point grid so the few
//      cone lights never lose their slots),
//   5. sphere-tests every decal (ForwardPlusDecal bounding sphere) the
//      same way and writes parallel per-tile decal lists.
//
// Depth buffer conventions (must match app):
//   - infinite reversed-Z projection, clear 0 (far), GreaterEqual.
//   - view distance from NDC depth d: dist = near / max(d, eps).
//   - NDC mapping with the engine viewport (y down in framebuffer):
//       ndcX = (px / w) * 2 - 1, ndcY = (py / h) * 2 - 1.

#define FORWARDPLUS_PASS
#include "common.hlsli"

struct ForwardPlusCullPush
{
    vk::BufferPointer<FrameUniforms> frameUniforms;
    vk::BufferPointer<ForwardPlusLightBuffer> lights;
    vk::BufferPointer<ForwardPlusTileCountBuffer> tileCounts;
    vk::BufferPointer<ForwardPlusTileIndexBuffer> tileIndices;
    uint depthTextureIndex;
    uint tileCountX;
    uint tileCountY;
    uint lightCount;
    uint tileSize;
    uint maxLightsPerTile;
    uint pad0;
    uint pad1;
};
// NOTE: no static_assert here – this DXC (HLSL 2018 mode) rejects it at
// global scope. Size is checked on the C++ side (64 bytes).

[[vk::push_constant]]
ConstantBuffer<ForwardPlusCullPush> pcCull;

// Unproject NDC -> view space (exact, flip/reversed-Z agnostic).
float3 UnprojectView(float2 ndcXY, float ndcZ, float4x4 invProj)
{
    float4 v = mul(invProj, float4(ndcXY, ndcZ, 1.0f));
    return v.xyz / max(v.w, 1e-9f);
}

[numthreads(1, 1, 1)]
void mainCS(uint3 groupId : SV_GroupID)
{
    uint tileX = groupId.x;
    uint tileY = groupId.y;
    if (tileX >= pcCull.tileCountX || tileY >= pcCull.tileCountY)
        return;
    uint tileIdx = tileY * pcCull.tileCountX + tileX;
    if (tileIdx >= (uint)MAX_FP_TILES)
        return;

    FrameUniforms frame = pcCull.frameUniforms.Get();
    float viewportW = max(frame.viewportSize.x, 1.0f);
    float viewportH = max(frame.viewportSize.y, 1.0f);
    uint tileSize = max(pcCull.tileSize, 1u);

    uint px0 = tileX * tileSize;
    uint py0 = tileY * tileSize;
    uint px1 = min(px0 + tileSize, (uint)viewportW);
    uint py1 = min(py0 + tileSize, (uint)viewportH);

    // ---- Depth min/max over the tile (NDC z, reversed: 1 near) ----
    uint depthIdx = NonUniformResourceIndex(pcCull.depthTextureIndex);
    float minD = 1.0f;
    float maxD = 0.0f;
    for (uint py = py0; py < py1; ++py)
    {
        for (uint px = px0; px < px1; ++px)
        {
            float d = textures[depthIdx].Load(int3((int)px, (int)py, 0)).r;
            minD = min(minD, d);
            maxD = max(maxD, d);
        }
    }

    // ---- Tile corners in NDC (framebuffer y down -> ndcY down) ----
    float x0 = (float(px0) / viewportW) * 2.0f - 1.0f;
    float x1 = (float(px1) / viewportW) * 2.0f - 1.0f;
    float y0 = (float(py0) / viewportH) * 2.0f - 1.0f;
    float y1 = (float(py1) / viewportH) * 2.0f - 1.0f;

    float4x4 invProj = frame.invProj;
    // Side planes go through the camera origin; use near corners (ndc=1).
    float3 c00 = UnprojectView(float2(x0, y0), 1.0f, invProj);
    float3 c10 = UnprojectView(float2(x1, y0), 1.0f, invProj);
    float3 c11 = UnprojectView(float2(x1, y1), 1.0f, invProj);
    float3 c01 = UnprojectView(float2(x0, y1), 1.0f, invProj);

    // Inward-facing side planes through the camera origin (plane dist = 0).
    // NDC y is framebuffer-y-down while view space is y-up, so the ndc row
    // y0 (top of the tile on screen) unprojects to the TOP edge in view
    // space: c00=top-left, c10=top-right, c11=bottom-right, c01=bottom-left.
    // Each cross() below is ordered so the normal points INTO the frustum
    // (verified by hand: top->down, bottom->up, left->right, right->left);
    // a sphere is outside iff dot(n, center) < -radius.
    float3 planes[4];
    planes[0] = normalize(cross(c00, c10)); // top
    planes[1] = normalize(cross(c11, c01)); // bottom
    planes[2] = normalize(cross(c01, c00)); // left
    planes[3] = normalize(cross(c10, c11)); // right

    // Depth range -> view-space distances (reversed-Z: dist = near / d).
    float nearP = max(frame.nearPlane, 1e-4f);
    float nearestDist = nearP / max(maxD, 1e-6f);   // closest surface
    float farthestDist = nearP / max(minD, 1e-6f);  // farthest (sky -> huge)

    // ---- Cull lights ----
    uint lightCount = min(pcCull.lightCount, (uint)MAX_FP_LIGHTS);
    uint maxPerTile = min(pcCull.maxLightsPerTile, (uint)MAX_FP_PER_TILE);
    uint count = 0;

    float4x4 view = frame.view;
    for (uint li = 0; li < lightCount; ++li)
    {
        ForwardPlusLight L = pcCull.lights.Get().lights[li];
        float range = L.range <= 0.0f ? 1e6f : L.range;

        // View-space center. Skip lights behind the camera:
        // view z positive = behind (camera looks down -z).
        float3 cView = mul(view, float4(L.position, 1.0f)).xyz;
        if (cView.z > range)
            continue;

        // Depth-range test (view looks down -z): tile covers
        // [-farthestDist, -nearestDist].
        if (-cView.z + range < nearestDist)
            continue;
        if (-cView.z - range > farthestDist)
            continue;

        // Side planes (origin planes, outward normals).
        bool outside = false;
        [unroll]
        for (int p = 0; p < 4; ++p)
        {
            if (dot(planes[p], cView) < -range)
            {
                outside = true;
                break;
            }
        }
        if (outside)
            continue;

        // Stride is the runtime maxLightsPerTile (matches the C++ allocation
        // tiles * maxLightsPerTile), NOT the MAX_FP_PER_TILE type bound.
        if (count < maxPerTile)
            pcCull.tileIndices.Get().indices[tileIdx * maxPerTile + count] = li;
        count++;
    }

    // Store the TRUE (unclamped) count as a debug/stat value: tiles
    // spanning big depth discontinuities can pass more lights than the
    // list holds. The list keeps only the first maxPerTile; the shading
    // pass iterates min(count, maxPerTile) entries, so overflow lights
    // pop off — an accepted trade-off (no walk-every-light fallback).
    pcCull.tileCounts.Get().counts[tileIdx] = count;

    // ---- Cull decals (same conservative bounding-sphere test as the
    // lights: tile side planes + depth range). Decals share the light
    // tiling; the counts store is UNCONDITIONAL (fresh 0 with no decals)
    // so the shading pass never iterates stale entries. The app pushes
    // lightCount = 0 when the deferred-shadow pipeline owns the light
    // lists — the decal leg below still runs, so decals stay alive in
    // both shading paths.
    uint decalCount = min(frame.decalCount, (uint)MAX_FP_DECALS);
    uint decalPerTile = min(frame.decalMaxPerTile, (uint)MAX_FP_DECALS_PER_TILE);
    uint dcount = 0;
    for (uint di = 0; di < decalCount; ++di)
    {
        ForwardPlusDecal D = frame.decals.Get().decals[di];
        float radius = max(D.bounds.w, 0.0f);
        if (radius <= 0.0f)
            continue;

        float3 cView = mul(view, float4(D.bounds.xyz, 1.0f)).xyz;
        if (cView.z > radius)
            continue;
        if (-cView.z + radius < nearestDist)
            continue;
        if (-cView.z - radius > farthestDist)
            continue;

        bool dOutside = false;
        [unroll]
        for (int p = 0; p < 4; ++p)
        {
            if (dot(planes[p], cView) < -radius)
            {
                dOutside = true;
                break;
            }
        }
        if (dOutside)
            continue;

        // Stride is frame.decalMaxPerTile (matches the builder's
        // tiles * maxDecalsPerTile allocation).
        if (dcount < decalPerTile)
            frame.decalTileIndices.Get().indices[tileIdx * decalPerTile + dcount] = di;
        dcount++;
    }
    frame.decalTileCounts.Get().counts[tileIdx] = dcount;
}
