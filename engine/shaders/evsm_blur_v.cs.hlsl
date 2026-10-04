// EVSM prefilter, VERTICAL leg (temp -> blurred). Separate entry point
// from evsm_blur_h.cs.hlsl so the blur axis never survives as a runtime
// branch — the shared kernel body + the cost budget live in
// evsm_blur.hlsli. This leg does NOT sanitize its loads: its source is
// the H leg's output, a convex combination of finite clamped values
// (the sanitize + its floor-care comment live in the H leg).

#define EVSM_BLUR_PASS
#define EVSM_BLUR_AXIS 1
#include "common.hlsli"
#include "evsm_blur.hlsli"

[numthreads(EVSM_BLUR_BLOCK, EVSM_BLUR_BLOCK, 1)]
void mainCS(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    EvsmBlurTile(gtid, gid);
}
