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

int main(int argc, const char* argv[]) {
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
