// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Compatibility forwarding header. Shared memory implementation lives in Core.
#include <Core/Memory/arena.h>

// Engine owns frame and GPU-flight lifetime management, not Core.
namespace smvk {
[[nodiscard]] Arena& AppArena();
[[nodiscard]] Arena& FrameArena();
void ResetFrameArena();
[[nodiscard]] Arena& FlightArena(uint32_t flightIndex);
void ResetFlightArena(uint32_t flightIndex);
}
