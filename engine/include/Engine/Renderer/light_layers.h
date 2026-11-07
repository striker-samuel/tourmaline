// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace smvk::lighting {

// A 32-bit receiver/light linking mask. Bits are user-defined channels,
// not GPU light-culling tiles or shadow flags. Unauthored scenes retain
// their historical lighting by defaulting both sides to all channels.
using LayerMask = uint32_t;
inline constexpr LayerMask kAllLayers = UINT32_MAX;
inline constexpr LayerMask kNoLayers = 0u;
inline constexpr LayerMask kDefaultLayer = 1u;

// A light affects a receiver if they share at least one enabled channel.
// A zero mask intentionally opts out of direct lighting from linked lights.
// No implication for shadows, GI, or reflections: those are separate passes.
[[nodiscard]] constexpr bool Affects(LayerMask lightLayers,
                                     LayerMask receiverLayers) noexcept {
    return (lightLayers & receiverLayers) != 0u;
}

[[nodiscard]] constexpr LayerMask Layer(unsigned bit) noexcept {
    return bit < 32u ? (LayerMask{1} << bit) : kNoLayers;
}

static_assert(Affects(kAllLayers, kAllLayers));
static_assert(Affects(kDefaultLayer, kAllLayers));
static_assert(!Affects(kNoLayers, kAllLayers));
static_assert(!Affects(Layer(1), Layer(2)));
static_assert(Affects(Layer(31), Layer(31)));
static_assert(Layer(32) == kNoLayers);

} // namespace smvk::lighting
