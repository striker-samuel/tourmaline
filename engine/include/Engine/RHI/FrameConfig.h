// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

// RHI-OWNED FRAME / PRESENTATION / WINDOW configuration. None of these
// are part of the shader ABI (shaders only cite the frames-in-flight
// margin in comments), so they stay plain C++ constants — no shared
// .hlsli needed. Exposed through smvk.h (the RHI umbrella header): every
// consumer may depend on these downward (rendergraph, renderer, editor,
// tests all legally reference the RHI's frame scheduling).
namespace smvk::limits {

// CPU-GPU pipeline depth: how many frames may have submitted-but-
// unfinished GPU work. Sizes the per-frame command pools, timeline
// semaphore margins and the retirement FIFO pairing (retires key at
// published + MaxFramesInFlight).
inline constexpr uint32_t MaxFramesInFlight = 2;

// Swapchain image count REQUEST (the surface may clamp it); the actual
// framesInFlight() the engine schedules derives from the acquired count.
inline constexpr uint32_t DesiredSwapchainImageCount = 2;

// Window creation defaults (the RHI owns the OS window; overridden by
// host flags/config when present).
inline constexpr uint32_t WindowDefaultWidth = 1920;
inline constexpr uint32_t WindowDefaultHeight = 1080;

} // namespace smvk::limits
