// Main forward lighting FS (classic alpha-test cutout variant).
// The shared body (default_lit_body.hlsli) holds everything; ALPHA_TO_COVERAGE
// is undefined here so Masked materials keep the discard alpha test.
#include "light.hlsli"
#include "common.hlsli"
#include "environment.hlsli"
#include "sggi.hlsli"
#include "sg_lightmap.hlsli"
#include "sg_regular.hlsli"
#include "default_lit_body.hlsli"
