// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// UV debug view, channel 1 (F10): the lightmap UV (chart-local TEXCOORD1
// + chart id, unpacked to atlas space by the filtered VS via the
// per-draw chart offset) as a checker + gradient + derivative glow.
// Chart-island layout, gutter bleed and seam-corner streaks read
// directly on the filtered path; the direct path shows chart space.
#define UVDBG_CHANNEL 1
#include "default_uv_debug.hlsli"
