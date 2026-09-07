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

#include <cstdio>

#ifdef NDEBUG
#define SMVK_DEBUG_LOG(...) ((void)0)
#else
#define SMVK_DEBUG_LOG(...) std::fprintf(stderr, __VA_ARGS__)
#endif
