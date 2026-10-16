// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Flat UV view, lightmap channel: the mesh unwrapped by its atlas UV
// (chart-local TEXCOORD1 + chart id, unpacked in the VS via the per-mesh
// chart tail pushed at byte 192) with the baked lightmap content behind
// it (needs a loaded .sglm atlas, like the F8 view).
#define UVFLAT_CHANNEL 1
#include "uvflat.vert.hlsli"
