// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Forward+ tile light culling (point/spot/probe; directional stays on CSM
// path) + decal culling (projected splats share the light tiling) + convex
// GI-volume culling (per-tile bitmask; sg_gi_volume.hlsli consumes it).
//
// Reflection probes ride the light buffer as type-2 entries
// (kForwardPlusProbe: position = center, range = influence + fade): the
// generic sphere leg below culls them with zero special cases, and the
// lit shader's type filter picks them back out of the same tile lists.
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
//      same way and writes parallel per-tile decal lists,
//   6. sphere-tests every convex GI volume's fade-expanded AABB against
//      the tile side planes and packs the survivors into a per-tile
//      bitmask (one bit per volume — the volumes blend by ORDERED
//      priority, so a capped index list could not represent them; the
//      <= 8 volume budget makes a mask the natural encoding).
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
    // Per-tile convex-volume bitmask output (the tail slot keeps every
    // field offset identical to the old pad0/pad1 layout).
    vk::BufferPointer<GiVolumeTileMaskBuffer> volumeMasks;
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

    // ---- Cull lights + probes (split per-tile slots) ----
    // One pass over the mixed entry range: type != probe packs the LIGHT
    // slots [0..perTile), type == probe packs the PROBE slots
    // [perTile..perTile+MAX_FP_PROBE_TILE_SLOTS) — stride
    // perTile + MAX_FP_PROBE_TILE_SLOTS matches the C++ allocation.
    // Split slots are load-bearing: sharing one [0..perTile) budget let
    // light-saturated tiles pop their probes, flipping specular IBL to
    // the global fallback cubemap (chrome ball "breaks" when a light grid
    // comes on). Overflow now only ever pops within its own class:
    // lights beyond perTile (first-come = spots-first buffer order, the
    // accepted Forward+ trade-off), probes beyond the tail slots
    // (first-come = upload order, pinned probes first).
    // counts packs both TRUE (unclamped) values: low 16 = lights, high
    // 16 = probes.
    uint lightCount = min(pcCull.lightCount, (uint)MAX_FP_LIGHTS);
    uint maxPerTile = min(pcCull.maxLightsPerTile, (uint)MAX_FP_PER_TILE);
    const uint stride = maxPerTile + (uint)MAX_FP_PROBE_TILE_SLOTS;
    uint lcount = 0;
    uint pcount = 0;

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

        if (L.type == kForwardPlusProbe) {
            if (pcount < (uint)MAX_FP_PROBE_TILE_SLOTS)
                pcCull.tileIndices.Get().indices[tileIdx * stride + maxPerTile + pcount] = li;
            pcount++;
        } else {
            if (lcount < maxPerTile)
                pcCull.tileIndices.Get().indices[tileIdx * stride + lcount] = li;
            lcount++;
        }
    }

    // Packed TRUE (unclamped) counts: lights low 16 bits, probes high 16.
    // The light loops mask the low word, the specular walk shifts.
    pcCull.tileCounts.Get().counts[tileIdx] = (lcount & 0xFFFFu) | (pcount << 16);

    // ---- Cull decals (same conservative bounding-sphere test as the
    // lights: tile side planes + depth range). Decals share the light
    // tiling; the counts store is UNCONDITIONAL (fresh 0 with no decals)
    // so the shading pass never iterates stale entries. Under deferred
    // shadows the app pushes lights+probes (the DS lists own the lights,
    // the light leg re-culls them as dead work) — the decal leg below
    // still runs, so decals stay alive in both shading paths. Reflection
    // probes need no leg of their own: they ride the light leg above as
    // type-2 entries (position = center, range = influence + fade).
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

    // ---- Cull GI volumes into the per-tile bitmask ----
    // A volume reaches a tile when the bounding sphere of its
    // fade-expanded AABB intersects the tile's side planes (the
    // behind-camera reject included; the sphere is conservative over
    // the AABB — the per-pixel AABB + plane tests in SgGiVolumeWeight make
    // the final call). NO depth-range cut, unlike the lights: fog-march
    // samples live IN FRONT of the nearest opaque surface (the march
    // starts at the camera), so the near-depth cut would wrongly drop
    // volumes that light the empty space before the wall; surface
    // shading only pays a slightly wider mask instead. The store is
    // UNCONDITIONAL (fresh 0 with no volumes / GI volumes off — same
    // discipline as the decal counts), so the shading passes never see
    // stale bits. Bit v = volume v (upload order = compositing priority
    // order, sg_gi_volume.hlsli).
    uint volBits = 0u;
    uint giVolumeCount = min(frame.giVolumeCount, (uint)SG_GI_VOLUME_MAX);
    for (uint vi = 0; vi < giVolumeCount; ++vi)
    {
        const GiVolumeGpu V = frame.giVolumes.Get().volumes[vi];
        const float3 c = 0.5 * (V.aabbMin.xyz + V.aabbMax.xyz);
        const float r = 0.5 * length(V.aabbMax.xyz - V.aabbMin.xyz);
        float3 cView = mul(view, float4(c, 1.0f)).xyz;
        if (cView.z > r)
            continue; // entirely behind the camera
        bool vOutside = false;
        [unroll]
        for (int p = 0; p < 4; ++p)
        {
            if (dot(planes[p], cView) < -r)
            {
                vOutside = true;
                break;
            }
        }
        if (!vOutside)
            volBits |= 1u << vi;
    }
    pcCull.volumeMasks.Get().masks[tileIdx] = volBits;
}
