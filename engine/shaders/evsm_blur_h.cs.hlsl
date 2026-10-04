// EVSM prefilter, HORIZONTAL leg (moments -> temp). Separate entry point
// from evsm_blur_v.cs.hlsl so the blur axis never survives as a runtime
// branch — the shared kernel body + the cost budget live in
// evsm_blur.hlsli. This leg sanitizes its loads (the V leg trusts the H
// output); the store goes to the ping-pong temp image the V leg reads.

#define EVSM_BLUR_PASS
#define EVSM_BLUR_AXIS 0
#include "common.hlsli"
#include "evsm_blur.hlsli"

[numthreads(EVSM_BLUR_BLOCK, EVSM_BLUR_BLOCK, 1)]
void mainCS(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    EvsmBlurTile(gtid, gid);
}
