#pragma once
// ---------------------------------------------------------------------------
// Engine globals - shader-side interface for per-frame global variables.
//
// REMOVED: g_engineGlobals StructuredBuffer (was binding 3, set 0).
// REPLACED: Frame constants moved to FramePSHot / FrameUniforms.
//   - time, deltaTime, fps: now in FramePSHot (via FrameUniforms::frameHot.Get())
//   - frameIndex: removed; texture streaming uses textureFeedbackEnabled flag
//   - g_frameIndex() removed; use pc.textureFeedbackEnabled instead
// ---------------------------------------------------------------------------

// ---- derived helpers (take time explicitly since pc not available in all stages) --
// Sin / cos of time over `period` seconds; 0..1 phase over `period` seconds.
float g_sinTime(float time, float period)  { return sin(time * 6.2831853 / max(period, 1e-4)); }
float g_cosTime(float time, float period)  { return cos(time * 6.2831853 / max(period, 1e-4)); }
float g_timeFrac(float time, float period) { return frac(time / max(period, 1e-4)); }

// Legacy accessor shims (take time explicitly)
float g_time(float time)       { return time; }
float g_deltaTime(float)       { return 0.0f; }
float g_fps(float)             { return 0.0f; }
