// SPDX-License-Identifier: MIT
#define CONTACT_SHADOW_PASS
#include "static_sdf_trace.hlsli"
#include "ddgi_common.hlsli"

// SDF-assisted probe relocation (Rendergraph/ddgi.h + docs #410): probes
// placed inside concave geometry (a U-shaped building, a doorway alcove)
// are "buried" - every ray hits nearby walls, so the blend marks them
// invalid and the surface they serve goes black. Use the signed distance
// field to push buried probes into free space, independently of the
// ray-tracing backend.

struct DdgiRelocatePush {
    GPU_PTR_READ(StaticSdfSceneHeader) scene;
    GPU_PTR_WRITE(DdgiRelocationBuffer) relocation;
    float originX, originY, originZ, spacing;
    uint dimX, dimY, dimZ, probes;
    float searchStep; // world-space axis search step (m)
    uint iterations;  // push-out iterations
};
GPU_PUSH(DdgiRelocatePush, sp);

[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID) {
    const uint probe = tid.x;
    if (probe >= sp.probes)
        return;

    const StaticSdfSceneHeader scene = sp.scene.Get();
    const uint3 dims = uint3(sp.dimX, sp.dimY, sp.dimZ);
    const float3 origin = float3(sp.originX, sp.originY, sp.originZ);
    float3 worldPos = origin + float3(DdgiGridCoord(probe, dims)) * sp.spacing;

    float dist = StaticSdfSceneDistance(scene, worldPos);
    // Relocate probes that are INSIDE geometry (SDF distance negative). The
    // bistro's grid places probes outside the subcell AABBs, so only meshes
    // spanning a subcell boundary bury a probe; most probes keep their slot.
    if (dist >= 0.0f || scene.instanceCount == 0u) {
        // Not buried (or no SDF): keep the grid position.
        sp.relocation.Get().values[probe] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // Buried: search the 6 world-axis directions for free space, iterating
    // to push out of thick geometry. The step is one probe spacing so a
    // probe escapes a wall/doorframe in a single hop.
    const float3 axes[6] = { float3(1, 0, 0),  float3(-1, 0, 0), float3(0, 1, 0),
                             float3(0, -1, 0), float3(0, 0, 1),  float3(0, 0, -1) };
    for (uint it = 0; it < sp.iterations && dist < 0.0f; ++it) {
        float bestD = dist;
        float3 bestPos = worldPos;
        [unroll] for (uint a = 0; a < 6; ++a) {
            const float3 p = worldPos + axes[a] * sp.searchStep;
            const float d = StaticSdfSceneDistance(scene, p);
            if (d > bestD) {
                bestD = d;
                bestPos = p;
            }
        }
        if (bestD <= dist)
            break; // no improvement: stuck (leave the offset we have)
        worldPos = bestPos;
        dist = bestD;
    }

    sp.relocation.Get().values[probe] =
        float4(worldPos - (origin + float3(DdgiGridCoord(probe, dims)) * sp.spacing), 0.0f);
}
