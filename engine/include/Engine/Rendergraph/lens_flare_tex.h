// SPDX-License-Identifier: CC0-1.0
// Original optical lookup artwork dedicated to the public domain under CC0 1.0.
// https://creativecommons.org/publicdomain/zero/1.0/
// No third-party images or textures were used.

#pragma once
#include <cstdint>

namespace smvk::lensflare {
inline constexpr uint32_t kWidth = 16;
inline constexpr uint32_t kHeight = 2;

// RGBA8 linear: row 0 = spectral ghost tint, row 1 = radial halo mask/tint.
// Packed as raw pixels to avoid an image decoder or external asset dependency.
inline constexpr uint8_t kTexels[] = {
    // Chromatic gradient.
    255, 134, 76, 255,
    255, 158, 91, 255,
    255, 193, 124, 255,
    240, 219, 153, 255,
    193, 239, 185, 255,
    132, 239, 218, 255,
    98, 216, 255, 255,
    99, 180, 255, 255,
    132, 151, 252, 255,
    180, 140, 247, 255,
    224, 146, 224, 255,
    255, 163, 189, 255,
    255, 181, 151, 255,
    255, 199, 126, 255,
    255, 177, 100, 255,
    255, 134, 76, 255,
    // Aperture falloff (Gaussian envelope centered at normalized radius 0.4).
    0, 0, 0, 255,
    2, 3, 3, 255,
    11, 14, 16, 255,
    38, 47, 53, 255,
    90, 112, 127, 255,
    151, 188, 214, 255,
    180, 224, 255, 255,
    151, 188, 214, 255,
    90, 112, 127, 255,
    38, 47, 53, 255,
    11, 14, 16, 255,
    2, 3, 3, 255,
    0, 0, 0, 255,
    0, 0, 0, 255,
    0, 0, 0, 255,
    0, 0, 0, 255,
};
static_assert(sizeof(kTexels) == kWidth * kHeight * 4);
} // namespace smvk::lensflare
