// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Debug-trace gating.
//
// Verbose diagnostics (gfx-filter dashboards, HZB audits, import summaries,
// test details) print only in debug builds. In release (NDEBUG) the process
// stays silent; hard-failure paths (init errors that return false / throw)
// keep their error lines regardless, so breakage is still visible.
//
// Usage for printf-style traces:
//   #include <Engine/debug_log.h>
//   SMVK_DEBUG_LOG("[GfxFilter] view: %u draws\n", draws);
// Usage for iostream info lines:
//   #ifndef NDEBUG
//   std::cout << "[App] ..." << std::endl;
//   #endif
//
// SMVK_TRACE_LOG: PERIODIC dashboards (every-N-frame status lines —
// [GfxProbe]/[GfxFilter] view + dispatch, [exposure] record/download).
// Unlike SMVK_DEBUG_LOG these stay Release-visible: they are the only
// per-run visibility into the live pipeline for the App / bench flows.
// They can be muted at RUNTIME though (SetTraceEnabled(false)) — the
// interactive EDITOR does this at startup so its terminal stays clean;
// dev tooling reads the same lines from App runs. Fault/warning lines
// (throttled or one-shot errors) stay unconditional either way.

#include <cstdio>

#ifdef NDEBUG
#define SMVK_DEBUG_LOG(...) ((void)0)
#else
#define SMVK_DEBUG_LOG(...) std::fprintf(stderr, __VA_ARGS__)
#endif

namespace smvk {

// Runtime trace gate (see SMVK_TRACE_LOG above). Default on. The static
// local in an inline function with external linkage is ONE flag for the
// whole process — every module (engine libs, host app) shares it.
inline bool& TraceEnabledRef() {
	static bool enabled = true;
	return enabled;
}
inline void SetTraceEnabled(bool enabled) { TraceEnabledRef() = enabled; }
inline bool TraceEnabled() { return TraceEnabledRef(); }

} // namespace smvk

#define SMVK_TRACE_LOG(...) \
	do { if (smvk::TraceEnabled()) std::fprintf(stderr, __VA_ARGS__); } while (0)
