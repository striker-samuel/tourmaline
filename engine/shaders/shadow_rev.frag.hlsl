// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Alpha-tested cutout depth/FS for REVERSED-Z passes: the Forward+ depth
// prepass masked phase, the temporal main-view seed and the remainder pass
// (clear 0, GreaterEqual). Conservative depth: SV_DepthLessEqual (raw float
// output <= interpolated == never nearer) so the depth test may run before
// the alpha-test shader body — seeded depth then culls occluded masked
// fragments without the baseColor fetch.
#include "common.hlsli"
#include "shadow_body.hlsli"
