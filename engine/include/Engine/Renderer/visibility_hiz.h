// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Renderer-neutral reversed-Z hierarchical depth-pyramid storage contract.
// Both the GfxFilter cluster culler and the GPU-scene instance culler consume
// this format: mip 0 is full-resolution depth and higher mips MIN-reduce.
// Ownership, layout, and flight-buffer allocation do not depend on gfx::State.
#include <Engine/RHI/resource_manager.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace tour::visibility {
inline constexpr uint32_t kHizMaxMips = 16u;
inline constexpr uint32_t kHizSrcDepth = 0xffffffffu;

// Same GPU-addressable format used by hzb.hlsli / hzb_spd.cs.hlsl.
struct HizResources {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipCount = 0;
    uint32_t mipOffsets[kHizMaxMips] = {};
    smvk::Holder<smvk::BufferHandle> buffer;
    smvk::LogicalBufferHandle logical{};
};

[[nodiscard]] inline uint32_t HzbMipCount(uint32_t w, uint32_t h) noexcept {
    if (w == 0 || h == 0) return 0u;
    uint32_t count = 1u;
    while ((w > 1u || h > 1u) && count < kHizMaxMips) {
        w = std::max(1u, (w + 1u) >> 1);
        h = std::max(1u, (h + 1u) >> 1);
        ++count;
    }
    return count;
}
inline void HzbMipDims(uint32_t w, uint32_t h, uint32_t mip,
                       uint32_t& outW, uint32_t& outH) noexcept {
    for (uint32_t k = 0u; k < mip; ++k) {
        w = std::max(1u, (w + 1u) >> 1);
        h = std::max(1u, (h + 1u) >> 1);
    }
    outW = w;
    outH = h;
}
[[nodiscard]] inline uint32_t HzbMipOffsetFloats(uint32_t w, uint32_t h,
                                                  uint32_t mip) noexcept {
    uint32_t off = 0u;
    for (uint32_t k = 0u; k < mip; ++k) {
        uint32_t kw, kh;
        HzbMipDims(w, h, k, kw, kh);
        off += kw * kh;
    }
    return off;
}
[[nodiscard]] inline uint32_t HzbTotalFloats(uint32_t w, uint32_t h) noexcept {
    const uint32_t count = HzbMipCount(w, h);
    if (!count) return 0u;
    uint32_t lw, lh;
    HzbMipDims(w, h, count - 1u, lw, lh);
    return HzbMipOffsetFloats(w, h, count - 1u) + lw * lh;
}
[[nodiscard]] inline float HzbReduce2x2(float a, float b,
                                         float c, float d) noexcept {
    return std::min(std::min(a, b), std::min(c, d));
}

// Per-mip builder push contract. Must remain byte-identical to
// hzb.hlsli and the existing GfxFilter compute pipeline.
struct HzbPush {
    uint64_t hzb = 0;
    uint32_t srcMip = kHizSrcDepth;
    uint32_t dstMip = 0;
    uint32_t srcWidth = 0;
    uint32_t srcHeight = 0;
    uint32_t dstWidth = 0;
    uint32_t dstHeight = 0;
    uint32_t srcOffset = 0;
    uint32_t dstOffset = 0;
    uint32_t depthTextureIndex = UINT32_MAX;
    uint32_t pad = 0;
};
static_assert(sizeof(HzbPush) == 48);

// AMD SPD single-dispatch contract. The same mip buffer layout is used
// by SPD and the per-mip fallback (byte-identical under finite inputs).
struct HzbSpdPush {
    uint64_t hzb = 0;
    uint64_t counter = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipCount = 0;
    uint32_t depthTextureIndex = UINT32_MAX;
    uint32_t mipOffsets[kHizMaxMips] = {};
    uint32_t groupCount = 0;
    uint32_t pad = 0;
};
static_assert(sizeof(HzbSpdPush) == 104);

// Pure CPU ABI helpers: no render graph, GfxFilter state or command buffer.
// False means the requested level cannot be built; callers must not
// dispatch a shader with invalid BDA / dimensions.
[[nodiscard]] inline bool MakeHizMipPush(
    const HizResources& hz, uint64_t address, uint32_t dstMip,
    uint32_t depthTextureIndex, HzbPush& out) noexcept {
    out = {};
    if (!address || !hz.width || !hz.height ||
        dstMip >= hz.mipCount || hz.mipCount > kHizMaxMips)
        return false;
    out.hzb = address;
    out.dstMip = dstMip;
    HzbMipDims(hz.width, hz.height, dstMip, out.dstWidth, out.dstHeight);
    out.dstOffset = hz.mipOffsets[dstMip];
    out.depthTextureIndex = depthTextureIndex;
    if (dstMip == 0u) {
        out.srcMip = kHizSrcDepth;
        out.srcWidth = hz.width;
        out.srcHeight = hz.height;
    } else {
        out.srcMip = dstMip - 1u;
        HzbMipDims(hz.width, hz.height, dstMip - 1u,
                   out.srcWidth, out.srcHeight);
        out.srcOffset = hz.mipOffsets[dstMip - 1u];
    }
    return true;
}
[[nodiscard]] inline bool MakeHizSpdPush(
    const HizResources& hz, uint64_t address, uint64_t counter,
    uint32_t depthTextureIndex, HzbSpdPush& out) noexcept {
    out = {};
    if (!address || !counter || !hz.width || !hz.height ||
        !hz.mipCount || hz.mipCount > kHizMaxMips)
        return false;
    out.hzb = address;
    out.counter = counter;
    out.width = hz.width;
    out.height = hz.height;
    out.mipCount = hz.mipCount;
    out.depthTextureIndex = depthTextureIndex;
    for (uint32_t k = 0; k < kHizMaxMips; ++k)
        out.mipOffsets[k] = hz.mipOffsets[k];
    out.groupCount = ((hz.width + 63u) / 64u) *
                     ((hz.height + 63u) / 64u);
    return true;
}

// Build/replace the acquired flight slice. New storage is initialized to
// far depth (0.0 in reversed-Z), so uninitialized memory cannot over-cull.
// On failure, mipCount is zero and both consumers must fail open.
[[nodiscard]] inline bool RebuildHizSlice(smvk::ResourceManager& ctx,
                                          smvk::RenderGraph& rg,
                                          HizResources& hz,
                                          uint32_t w, uint32_t h,
                                          const char* name) {
    if (!hz.logical.empty()) rg.FreeBuffer(hz.logical);
    hz.logical = {};
    hz.buffer = {};
    hz.width = w;
    hz.height = h;
    hz.mipCount = HzbMipCount(w, h);
    for (uint32_t m = 0; m < hz.mipCount; ++m)
        hz.mipOffsets[m] = HzbMipOffsetFloats(w, h, m);
    for (uint32_t m = hz.mipCount; m < kHizMaxMips; ++m)
        hz.mipOffsets[m] = 0u;
    const size_t bytes = size_t(HzbTotalFloats(w, h)) * sizeof(float);
    if (!bytes) {
        hz.width = hz.height = hz.mipCount = 0;
        return false;
    }
    hz.buffer = ctx.CreateBuffer(bytes, smvk::BufferType_Storage, name);
    if (hz.buffer.empty()) {
        hz.width = hz.height = hz.mipCount = 0;
        return false;
    }
    std::vector<uint8_t> zeros(bytes, 0u);
    ctx.UploadDataToBuffer(hz.buffer, zeros.data(), zeros.size(),
                           smvk::PipelineStage(smvk::PipelineStageBit_Compute),
                           smvk::AccessFlags::ShaderRead | smvk::AccessFlags::ShaderWrite);
    hz.logical = rg.AddExternalBuffer(hz.buffer, name);
    if (hz.logical.empty()) {
        hz.buffer = {};
        hz.width = hz.height = hz.mipCount = 0;
        return false;
    }
    return true;
}

// A reusable graph edge for any consumer building a reversed-Z pyramid.
// Mip zero reads depth; later mips only read/write the buffer.
inline void AddHizBuildHints(smvk::RenderPass& pass,
                             smvk::LogicalBufferHandle pyramid,
                             smvk::LogicalImageViewHandle depth,
                             bool readsDepth) {
    smvk::BufferSyncRequest rw{};
    rw.barrierMasks.accessMask =
        smvk::AccessFlags::ShaderRead | smvk::AccessFlags::ShaderWrite;
    rw.barrierMasks.stageMask = smvk::PipelineStageBit_Compute;
    pass.AddBufferHint(pyramid, rw);
    if (readsDepth && !depth.empty()) {
        smvk::ImageSyncRequest req{};
        req.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
        req.barrierMasks.stageMask = smvk::PipelineStageBit_Compute;
        req.layout = smvk::ImageLayout::DepthStencilReadOnlyOptimal;
        pass.AddImageHint(depth, req);
    }
}
} // namespace tour::visibility
