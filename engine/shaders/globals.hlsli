#pragma once
// ---------------------------------------------------------------------------
// Engine globals - shader-side interface for per-frame global variables.
//
// One EngineGlobals instance is uploaded by the engine every frame (one
// slice per frame in flight) and exposed through bindless set 0, binding
// kBinding_EngineGlobals (3). Available in EVERY stage of EVERY pass - no
// push-constant plumbing. Values are computed engine-side from the frame
// acquire rhythm:
//   time       seconds since engine start
//   deltaTime  seconds since the previous frame (0 on the first frame)
//   frameIndex absolute frame counter (wraps at 2^32)
//   fps        smoothed (EMA) frames per second
//
// Struct must mirror C++ smvk::EngineGlobals (engine/include/Engine/smvk.h);
// DXC (HLSL 2018 mode) rejects static_assert at global scope, so keep both
// sides in sync manually (16 bytes: 4 scalars).
// ---------------------------------------------------------------------------

struct EngineGlobals
{
    float time;        // 0
    float deltaTime;   // 4
    uint  frameIndex; // 8
    float fps;         // 12
};

[[vk::binding(3, 0)]]
StructuredBuffer<EngineGlobals> g_engineGlobals;

// ---- accessors -------------------------------------------------------------
float g_time()       { return g_engineGlobals[0].time; }
float g_deltaTime()  { return g_engineGlobals[0].deltaTime; }
uint  g_frameIndex() { return g_engineGlobals[0].frameIndex; }
float g_fps()        { return g_engineGlobals[0].fps; }

// ---- derived helpers ---------------------------------------------------------
// Sin / cos of time over `period` seconds; 0..1 phase over `period` seconds.
float g_sinTime(float period)  { return sin(g_time() * 6.2831853 / max(period, 1e-4)); }
float g_cosTime(float period)  { return cos(g_time() * 6.2831853 / max(period, 1e-4)); }
float g_timeFrac(float period) { return frac(g_time() / max(period, 1e-4)); }
