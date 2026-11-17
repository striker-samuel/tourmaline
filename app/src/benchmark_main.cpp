// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Dedicated benchmark entry point: no game/sample element, no interactive
// unbounded run. The shared pak host still owns content and renderer setup.
#include "Host.h"

#include <Engine/App/RunConfig.h>
#include <Engine/Utils/shader_library.h>

#include <cstdlib>
#include <iostream>
#include <utility>
#include <cstring>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

// Apply before host startup so newly created worker threads inherit the mask.
static bool ApplyBenchmarkAffinity() {
    const char* value = std::getenv("TOURMALINE_BENCH_CPU_LIMIT");
    if (!value) return true;
    char* end = nullptr;
    const long count = std::strtol(value, &end, 10);
    if (end == value || *end || count < 1 || count > 1024) return false;
    long selected = 0;
#ifdef _WIN32
    DWORD_PTR allowed = 0, system = 0, mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system)) return false;
    for (unsigned i = 0; i < sizeof(allowed) * 8 && selected < count; ++i) {
        const DWORD_PTR bit = DWORD_PTR(1) << i;
        if (allowed & bit) { mask |= bit; ++selected; }
    }
    if (selected != count || !SetProcessAffinityMask(GetCurrentProcess(), mask)) return false;
#elif defined(__linux__)
    cpu_set_t allowed, mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return false;
    for (int i = 0; i < CPU_SETSIZE && selected < count; ++i) {
        if (CPU_ISSET(i, &allowed)) { CPU_SET(i, &mask); ++selected; }
    }
    if (selected != count || sched_setaffinity(0, sizeof(mask), &mask) != 0) return false;
#else
    return false;
#endif
    std::cerr << "[Benchmark] logical CPU limit=" << selected << '\n';
    return true;
}

int main(int argc, const char* argv[]) {
    if (!ApplyBenchmarkAffinity()) {
        std::cerr << "[Benchmark] cannot apply requested logical CPU limit\n";
        return EXIT_FAILURE;
    }
    app::PakAppConfig cfg;
    cfg.run = engine::RunConfig::Parse();
    cfg.run.Log();
    engine::RunConfig::WarnUnknown();
    int exitCode = 0;
    if (!app::ParsePakAppArgs(argc, argv, cfg.cli, exitCode))
        return exitCode;
    if (cfg.cli.benchFrames <= 0) {
        std::cerr << "[Benchmark] --bench <measured frames> is required; "
                     "interactive runs belong in App.\n";
        return EXIT_FAILURE;
    }
    tour::shaders::Init();
    // Unlike the viewer, no audio/gameplay sample extensions are attached.
    return app::RunPakApp(std::move(cfg));
}
