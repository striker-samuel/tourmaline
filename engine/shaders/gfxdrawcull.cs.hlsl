// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GfxDrawCull (GPU cull chain): Northlight-style mesh-bound fast discard
// for the filter legs — one thread per draw. Runs after GfxHiz, before the
// filter dispatch (the filter reads the verdicts to skip whole groups).
//
//   frustum: draw union bound vs the FROZEN cull view's planes (the gfx
//            view record matrix — held by the debug culling freeze; the
//            live-view counterpart runs in gfxbatchcull for the seed /
//            remainder verdicts)
//   occlusion: the same ProjectBound + BoundHzbOccluded math the texture
//            demand publisher uses on mesh bounds (containment: the draw
//            sphere contains every cluster sphere, so a draw that fails
//            here would fail the filter's per-cluster tests anyway — the
//            verdict is a strict fast-reject, never a new hole)
//   SKINNED draws test the DEFORMED union bound (GfxSkinBounds' drawBound
//            tail — the union over the deformed cluster spheres the filter's
//            per-cluster tests run on, so the containment argument holds on
//            the animated pose; SkinDump cross-checked the table) in the
//            FRUSTUM test; their HZB OCCLUSION test is skipped (conservative:
//            the occluder-depth pyramid is built by the skinned occluder
//            leg, and the fog evidence — depth silhouette complete while
//            Main misses meshlets — pinned the visible drops on the lossy
//            filter stages). Without the table the whole fast-reject stays
//            skipped for them (the conservative keep)
//   MASK draws can be excluded from the occlusion test (the MIN-reduced
//            pyramid poisons cutout texels; the filter's per-cluster test
//            takes the same skip via GFX_CULL_HZBSKIPMASKED)
//
// C++ mirror: Engine/Renderer/gfx_filter.h (DrawCullPush).

#define GFXDRAWCULL_PASS
#include "common.hlsli"
#include "gfxfilter.hlsli"
#include "gpu_platform.hlsli"

GPU_PUSH(GfxDrawCullPush, pcD);

[numthreads(64, 1, 1)]
void mainCS(uint3 tid : SV_DispatchThreadID)
{
    // Verdicts default to 1 ("visible"): every failure direction below is
    // the conservative keep — a false keep only costs work, a false drop
    // would delete geometry the rasterizer should have drawn.
    if (tid.x >= pcD.drawCount || tid.x >= GFX_MAX_DRAWS)
        return;
    GfxDraw d = pcD.draws.Get().draws[tid.x];

    uint verdict = 1u;

    // SKINNED draws test the DEFORMED union bound (GfxSkinBounds' drawBound
    // tail — the union over the deformed cluster spheres the filter's
    // per-cluster tests run on, so the containment argument holds on the
    // animated pose): the static bind-pose mesh union can swing a whole
    // animated mesh clean outside itself. Without the table (the pass
    // skipped / null BDA — skinBoundsValid = 0) the whole fast-reject
    // stays SKIPPED for skinned draws: skipping only loses the reject,
    // never drops geometry.
    const bool skinned = (d.misc.y & GFX_DRAW_SKINNED) != 0;
    const bool useSkinBounds = skinned && pcD.skinBoundsValid != 0u;

    // ---- Frustum (draw bound, FROZEN view, guard-dilated planes) ----
    if (!skinned || useSkinBounds)
    {
        float4 planes[6];
        GfxExtractViewPlanes(pcD.viewProj,
            GFX_CULL_GUARD_PX * 2.0f / max((float) pcD.width, 1.0f),
            GFX_CULL_GUARD_PX * 2.0f / max((float) pcD.height, 1.0f),
            planes);
        float4 bound = useSkinBounds
            ? pcD.skinBounds.Get().drawBound[tid.x]
            : pcD.meshBounds.Get().centerRadius[d.misc.x];
        float3 worldC = mul(d.model, float4(bound.xyz, 1.0f)).xyz;
        float worldR = bound.w * GfxMaxBasisScale(d.model);
        if (!GfxSphereInPlanes(planes, worldC, worldR))
            verdict = 0u;

        // ---- HZB occlusion (draw bound; the filter's own per-cluster test
        //      stays in gfxfilter.cs.hlsl for exact mesh/indexed parity) ----
        // CONSERVATIVE for skinned draws: the occlusion test is SKIPPED
        // (the frustum fast-reject above stays — the deformed union is
        // cross-checked sound). The occluder-depth HZB is built by the
        // skinned occluder leg, and the fog evidence (a complete depth
        // silhouette while Main misses meshlets) pinned the visible-chunk
        // drops on the lossy filter stages — skinned geometry stays off the
        // occlusion verdict until the residual dropper is isolated.
        if (verdict != 0u
            && !skinned
            && (pcD.cullFlags & GFX_CULL_OCCLUSION) != 0
            && pcD.hizMips != 0u)
        {
            bool wantOcc = true;
            if ((pcD.cullFlags & GFX_CULL_HZBSKIPMASKED) != 0
                && (d.misc.z & GFX_DRAWSET_MASKED) != 0)
                wantOcc = false; // cutout draws: geometric tests only
            if (wantOcc)
            {
                GfxHzbCtx ctx;
                ctx.viewProj = pcD.viewProj;
                ctx.eye = pcD.eye;
                ctx.width = pcD.width;
                ctx.height = pcD.height;
                ctx.mips = pcD.hizMips;
                ctx.hzb = pcD.hzb;
                BoundProjection p = ProjectBound(bound, d.model, ctx);
                // Untestable (eye inside / near-plane cross / NaN) and
                // off-target footprints keep: the frustum stages own those.
                if (p.testable && p.onTarget && BoundHzbOccluded(p, ctx))
                    verdict = 0u;
            }
        }
    }

    pcD.cullState.Get().v[tid.x] = verdict;
}
