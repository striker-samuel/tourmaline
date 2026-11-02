// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The ONE miniaudio implementation TU (the VMA/stb repo pattern: the
// impl section compiles in the consumer's tree, external/miniaudio
// stays pure upstream). Feature macros + the include live in
// miniaudio_config.h so every TU — this one and the engine TUs that
// use the API — sees the identical header configuration.
//
// Compiled WITHOUT the engine PCH on purpose (a 4 MB self-contained C
// header; the PCH's force-injected engine includes buy nothing here).
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio_config.h"
