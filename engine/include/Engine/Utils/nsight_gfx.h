// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Nsight Graphics SDK integration: procedural Graphics Capture / GPU Trace
// triggering and Vulkan frame-boundary marking, without launching the Nsight
// GUI or CLI. The SDK is header-only and vendored under
// external/nsight_graphics_sdk (Apache-2.0); this header is the engine-facing
// seam and deliberately exposes NO NGFX types, so it is safe to include
// everywhere. The whole body compiles only with -DTOURMALINE_NGFX_SDK=ON
// (the diagnostics build profile); otherwise every function is a no-op and
// shipping builds never touch the NVIDIA headers.
#include <cstdint>

namespace tour::nsight_gfx {

// Initialize the SDK. Sets the library-load callback (NoVerification for
// dev/diagnostics) and initializes exactly ONE activity, chosen from the
// TOURMALINE_NGFX_* pins read at this point:
//   - TOURMALINE_NGFX_CAPTURE_FRAME set  -> Graphics Capture
//   - TOURMALINE_NGFX_TRACE_START set    -> GPU Trace
//   - neither                            -> no activity (frame boundaries only,
//                                           for GUI-attached capture)
// MUST be called once, BEFORE the Vulkan instance is created. Returns true
// when the SDK is compiled in and the load callback installed (even when no
// activity is selected or the app is running unattached).
bool Initialize();

// True when the SDK is compiled in (TOURMALINE_NGFX_SDK). Constant-foldable.
bool Available();

// Mark the end of a frame on the given Vulkan queue (VkQueue, passed as an
// opaque pointer to keep the header NGFX-free). Called once per frame; no-op
// unless a frame boundary is enabled by a pin or an initialized activity.
void FrameBoundary(void* vkQueue);

// Per-frame procedural trigger poll (1-based frameIndex). Applies:
//   - TOURMALINE_NGFX_CAPTURE_FRAME=<n>: request a 1-frame Graphics Capture
//     at frame n (delimiter = frame boundary).
//   - TOURMALINE_NGFX_TRACE_START=<n> / TOURMALINE_NGFX_TRACE_END=<m>: start
//     a GPU Trace at frame n and stop it at frame m.
void Poll(uint64_t frameIndex);

} // namespace tour::nsight_gfx
