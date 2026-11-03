// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Separate opt-in direct vertex stage. Reuses the same varyings and FS
// family; no extra chart branches or storage reads in default.vert.hlsl.
#define DIRECT_LIGHTMAP_PASS 1
#include "default.vert.hlsl"
