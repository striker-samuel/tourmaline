// Alpha-tested cutout depth/FS for NORMAL-Z passes: directional shadow
// cascades, spot-shadow atlases and their temporal seeds (clear 1.0,
// LessEqual). Conservative depth: SV_DepthGreaterEqual (never farther) so
// the depth test may run before the alpha-test shader body.
#define SHADOW_NORMAL_Z 1
#include "common.hlsli"
#include "shadow_body.hlsli"
