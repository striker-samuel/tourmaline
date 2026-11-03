// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// ---------------------------------------------------------------------------
// Engine globals - shader-side interface for per-frame global variables.
//
// REMOVED: g_engineGlobals StructuredBuffer (was binding 3, set 0).
// REPLACED: Frame constants moved to FramePSHot / FrameUniforms.
//   - time, deltaTime, fps: now in FramePSHot (via FrameUniforms::frameHot.Get())
//   - frameIndex: removed; no per-frame frame gating in shaders
// ---------------------------------------------------------------------------

// ---- derived helpers (take time explicitly since pc not available in all stages) --
// Sin / cos of time over `period` seconds; 0..1 phase over `period` seconds.
float g_sinTime(float time, float period)  { return sin(time * 6.2831853 / max(period, 1e-4)); }
float g_cosTime(float time, float period)  { return cos(time * 6.2831853 / max(period, 1e-4)); }
float g_timeFrac(float time, float period) { return frac(time / max(period, 1e-4)); }

// Scene shader convenience macros. These require the standard scene push
// constant `pc` to be in scope. Project lit hooks receive time separately
// through TMLitInput::time (their function scope does not have `pc`).
#define TM_TIME       (pc.frameUniforms.Get().frameHot.Get().time)
#define TM_DELTA_TIME (pc.frameUniforms.Get().frameHot.Get().deltaTime)
#define TM_FPS        (pc.frameUniforms.Get().frameHot.Get().fps)

// Explicit-value helpers for code without access to scene push constants.
// Pass the values from FramePSHot; no shader-side implicit clock exists.
float g_time(float time)           { return time; }
float g_deltaTime(float deltaTime) { return deltaTime; }
float g_fps(float fps)             { return fps; }
