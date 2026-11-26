// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Asset/ktx_decode.h>

#include <algorithm>

#include "ktx.h"

namespace tour::asset {

std::optional<DecodedRgba8> DecodeKtx2ToRgba8(std::span<const uint8_t> ktxBytes, uint32_t maxDim) {
    // Shared acceptance + decode + mip-selection rules (extracted from
    // GiDecodeKtxTexelData): libktx CPU decode, Basis transcoded to RGBA32,
    // mip 0 or the smallest mip that fits maxDim. RGB is returned RAW (no
    // transfer conversion) — `srgb` reports the KTX2 transfer function.
    if (ktxBytes.empty() || maxDim == 0)
        return std::nullopt;
    ktxTexture2* raw = nullptr;
    if (ktxTexture2_CreateFromMemory(reinterpret_cast<const ktx_uint8_t*>(ktxBytes.data()),
                                     static_cast<ktx_size_t>(ktxBytes.size()),
                                     KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
                                     &raw) != KTX_SUCCESS)
        return std::nullopt;
    struct Guard {
        ktxTexture2* t;
        ~Guard() {
            if (t)
                ktxTexture2_Destroy(t);
        }
    } guard{raw};
    if (ktxTexture2_NeedsTranscoding(raw)) {
        if (ktxTexture2_TranscodeBasis(raw, KTX_TTF_RGBA32, 0) != KTX_SUCCESS)
            return std::nullopt;
    }
    const ktx_uint32_t baseWidth = raw->baseWidth;
    const ktx_uint32_t baseHeight = std::max<ktx_uint32_t>(raw->baseHeight, 1);
    const ktx_uint32_t levels = std::max<ktx_uint32_t>(raw->numLevels, 1);
    // The smallest mip that fits maxDim (mip 0 when the base fits — never
    // UPSCALE a small texture).
    ktx_uint32_t level = 0;
    while (level + 1 < levels && (baseWidth >> (level + 1)) > maxDim)
        ++level;
    const ktx_uint32_t w = std::max<ktx_uint32_t>(baseWidth >> level, 1);
    const ktx_uint32_t h = std::max<ktx_uint32_t>(baseHeight >> level, 1);
    ktx_size_t offset = 0;
    if (ktxTexture2_GetImageOffset(raw, level, 0, 0, &offset) != KTX_SUCCESS)
        return std::nullopt;
    const ktx_size_t rowPitch = ktxTexture_GetRowPitch(reinterpret_cast<ktxTexture*>(raw), level);
    const ktx_uint32_t components = ktxTexture2_GetNumComponents(raw);
    if (components < 3 || rowPitch < ktx_size_t(w) * components)
        return std::nullopt;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(raw->pData) + offset;
    DecodedRgba8 out;
    out.width = w;
    out.height = h;
    out.srgb = ktxTexture2_GetTransferFunction_e(raw) == KHR_DF_TRANSFER_SRGB;
    out.rgba.resize(size_t(w) * size_t(h) * 4);
    size_t k = 0;
    for (ktx_uint32_t y = 0; y < h; ++y) {
        const uint8_t* row = base + y * rowPitch;
        for (ktx_uint32_t x = 0; x < w; ++x, ++k) {
            const uint8_t* px = row + x * components;
            out.rgba[k * 4 + 0] = px[0];
            out.rgba[k * 4 + 1] = px[1];
            out.rgba[k * 4 + 2] = px[2];
            out.rgba[k * 4 + 3] = components >= 4 ? px[3] : 255u;
        }
    }
    return out;
}

} // namespace tour::asset
