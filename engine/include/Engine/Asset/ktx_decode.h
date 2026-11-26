// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Shared KTX2 -> RGBA8 CPU decode (the one decode the GI bakers and the SDF
// color bake both need). Extracted from GiDecodeKtxTexelData so the two
// consumers share the acceptance + transcode + mip-selection rules instead
// of duplicating the libktx call sequence.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace tour::asset {

// Decoded RGBA8 texel data (uint8, row-major, 4 channels). RGB is the RAW
// decoded byte stream (post-Basis-transcode; the KTX2 transfer function is
// NOT applied — `srgb` reports it so callers convert themselves). alpha = 255
// for RGB-only sources.
struct DecodedRgba8 {
    uint32_t width = 0;
    uint32_t height = 0;
    bool srgb = false;         // KTX2 transfer function (sRGB = true)
    std::vector<uint8_t> rgba; // width * height * 4, row-major
};

// Decode a KTX2 payload to RGBA8 (mip 0, or the smallest mip whose width
// fits maxDim — never upscale a small texture). Basis-supercompressed
// payloads transcode to RGBA32 first. nullopt on decode failure / < 3
// components (the same acceptance rules as GiAverageKtxTexel).
std::optional<DecodedRgba8> DecodeKtx2ToRgba8(std::span<const uint8_t> ktxBytes, uint32_t maxDim);

} // namespace tour::asset
