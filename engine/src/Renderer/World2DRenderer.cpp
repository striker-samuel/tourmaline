// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Renderer/World2DRenderer.h>
#include <Engine/Renderer/vertex_layouts.h>
#include <Engine/Utils/shader_library.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <utility>

namespace tour {

SpriteAtlas2D::SpriteAtlas2D(uint32_t pageSize) : m_pageSize(std::max(pageSize, 3u)) {}

uint32_t SpriteAtlas2D::Insert(uint32_t width, uint32_t height, std::span<const uint8_t> rgba8) {
    if (!width || !height || width > m_pageSize - 2 || height > m_pageSize - 2 ||
        m_regions.size() >= UINT32_MAX || uint64_t(width) * height * 4 != rgba8.size())
        return kInvalidSprite2D;

    const uint32_t paddedW = width + 2;
    const uint32_t paddedH = height + 2;
    uint32_t pageIndex = UINT32_MAX;
    uint32_t placeX = 0, placeY = 0;
    for (uint32_t i = 0; i < m_pages.size(); ++i) {
        const Page& p = m_pages[i];
        const bool newRow = p.x + paddedW > m_pageSize;
        const uint32_t y = newRow ? p.y + p.rowHeight : p.y;
        if (y + paddedH <= m_pageSize) {
            pageIndex = i;
            placeX = newRow ? 0 : p.x;
            placeY = y;
            break;
        }
    }
    if (pageIndex == UINT32_MAX) {
        if (m_pages.size() >= UINT32_MAX)
            return kInvalidSprite2D;
        pageIndex = static_cast<uint32_t>(m_pages.size());
        Page p{};
        p.pixels.resize(size_t(m_pageSize) * m_pageSize * 4);
        m_pages.push_back(std::move(p));
    }
    Page& p = m_pages[pageIndex];
    if (placeX == 0)
        p.rowHeight = 0;
    p.x = placeX + paddedW;
    p.y = placeY;
    p.rowHeight = std::max(p.rowHeight, paddedH);

    // Edge extrusion including the four corners.
    for (uint32_t dy = 0; dy < paddedH; ++dy) {
        const uint32_t sy = std::clamp(int(dy) - 1, 0, int(height) - 1);
        for (uint32_t dx = 0; dx < paddedW; ++dx) {
            const uint32_t sx = std::clamp(int(dx) - 1, 0, int(width) - 1);
            const size_t dst = (size_t(placeY + dy) * m_pageSize + placeX + dx) * 4;
            const size_t src = (size_t(sy) * width + sx) * 4;
            std::memcpy(p.pixels.data() + dst, rgba8.data() + src, 4);
        }
    }
    const float inv = 1.0f / float(m_pageSize);
    m_regions.push_back(
        {pageIndex,
         glm::vec2(float(placeX + 1) * inv, float(placeY + 1) * inv),
         glm::vec2(float(placeX + 1 + width) * inv, float(placeY + 1 + height) * inv),
         glm::uvec2(width, height)});
    ++m_revision;
    return static_cast<uint32_t>(m_regions.size() - 1);
}

const SpriteRegion2D* SpriteAtlas2D::Region(uint32_t id) const {
    return id < m_regions.size() ? &m_regions[id] : nullptr;
}
std::span<const uint8_t> SpriteAtlas2D::PagePixels(uint32_t page) const {
    if (page >= m_pages.size())
        return {};
    return m_pages[page].pixels;
}
void SpriteAtlas2D::Clear() {
    m_regions.clear();
    m_pages.clear();
    ++m_revision;
}

SpriteBatch2D BuildSpriteBatch2D(const SpriteAtlas2D& atlas, std::span<const Sprite2D> sprites) {
    SpriteBatch2D batch;
    std::vector<size_t> order;
    order.reserve(sprites.size());
    for (size_t i = 0; i < sprites.size(); ++i) {
        const auto& s = sprites[i];
        if (atlas.Region(s.region) && std::isfinite(s.z) && std::isfinite(s.rotation) &&
            std::isfinite(s.position.x) && std::isfinite(s.position.y) && std::isfinite(s.size.x) &&
            std::isfinite(s.size.y) && s.size.x > 0.0f && s.size.y > 0.0f &&
            std::isfinite(s.color.x) && std::isfinite(s.color.y) && std::isfinite(s.color.z) &&
            std::isfinite(s.color.w))
            order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return sprites[a].z < sprites[b].z;
    });
    batch.instances.reserve(order.size());
    for (size_t i : order) {
        const auto& s = sprites[i];
        const auto& r = *atlas.Region(s.region);
        SpriteInstance2D gpu{{s.position.x, s.position.y},
                             {s.size.x, s.size.y},
                             {r.uvMin.x, r.uvMin.y},
                             {r.uvMax.x, r.uvMax.y},
                             {s.color.x, s.color.y, s.color.z, s.color.w},
                             s.rotation,
                             s.z};
        const uint32_t n = static_cast<uint32_t>(batch.instances.size());
        batch.instances.push_back(gpu);
        if (batch.runs.empty() || batch.runs.back().page != r.page)
            batch.runs.push_back({r.page, n, 1u});
        else
            ++batch.runs.back().count;
    }
    if (!batch.instances.empty()) {
        batch.minZ = batch.instances.front().z;
        batch.maxZ = batch.instances.back().z;
    }
    return batch;
}

bool World2DRenderer::EnsureShaders(smvk::ResourceManager& ctx) {
    if (m_program.shaderCount == 2)
        return true;
    try {
        if (m_vs.empty())
            m_vs = ctx.CreateShaderModule(shaders::Read("content/shaders/world2d.vert.hlsl.spv"),
                                          "world2dVS");
        if (m_fs.empty())
            m_fs = ctx.CreateShaderModule(shaders::Read("content/shaders/world2d.frag.hlsl.spv"),
                                          "world2dPS");
    } catch (...) {
        return false;
    }
    if (m_vs.empty() || m_fs.empty())
        return false;
    smvk::Shader vs{.module = m_vs, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex};
    smvk::Shader ps{.module = m_fs, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment};
    m_program = {.shaders = {vs, ps}, .shaderCount = 2};
    return true;
}

bool World2DRenderer::UploadAtlas(smvk::ResourceManager& ctx) {
    if (m_uploadedRevision == m_atlas.Revision())
        return !m_pages.empty();
    if (m_atlas.PageCount() == 0) {
        m_pages.clear();
        m_uploadedRevision = m_atlas.Revision();
        return false;
    }
    std::vector<PageGpu> pending;
    pending.reserve(m_atlas.PageCount());
    smvk::ImageCreateInfo ci{};
    ci.extent = {m_atlas.PageSize(), m_atlas.PageSize(), 1};
    ci.format = smvk::Format::RGBA8_UNorm;
    ci.usage = smvk::ImageUsage::Sampled | smvk::ImageUsage::TransferDst;
    ci.mipLevels = 1;
    ci.name = "world2dAtlas";
    for (uint32_t p = 0; p < m_atlas.PageCount(); ++p) {
        PageGpu gpu;
        auto pixels = m_atlas.PagePixels(p);
        gpu.texture = ctx.CreateTexture(ci, pixels.data());
        if (gpu.texture.empty())
            return false;
        gpu.bindless = ctx.GetTextureBindlessIndex(gpu.texture);
        if (gpu.bindless == UINT32_MAX)
            return false;
        pending.push_back(std::move(gpu));
    }
    smvk::SamplerCreateInfo sampler{};
    sampler.addressU = smvk::AddressMode::ClampToEdge;
    sampler.addressV = smvk::AddressMode::ClampToEdge;
    sampler.addressW = smvk::AddressMode::ClampToEdge;
    sampler.maxLod = 0.0f; // no mips; UV gutters guard linear filtering
    const uint32_t samplerIndex = ctx.GetOrCreateSampler(sampler);
    if (samplerIndex == UINT32_MAX)
        return false;
    ctx.FlushUploads();
    m_pages = std::move(pending);
    m_sampler = samplerIndex;
    m_uploadedRevision = m_atlas.Revision();
    return true;
}

bool World2DRenderer::AppendPass(smvk::ResourceManager& ctx,
                                 smvk::RenderGraph& rg,
                                 smvk::LogicalImageViewHandle colorTarget,
                                 smvk::Extent2D extent,
                                 uint32_t frameSlot,
                                 bool clear) {
    if (colorTarget.empty() || !extent.width || !extent.height || frameSlot >= m_flights.size() ||
        !(m_pixelsPerUnit > 0.0f) || !std::isfinite(m_pixelsPerUnit))
        return false;
    const SpriteBatch2D batch = BuildSpriteBatch2D(m_atlas, m_sprites);
    if (batch.instances.empty())
        return true;
    if (batch.instances.size() > UINT32_MAX || m_uploadedRevision != m_atlas.Revision() ||
        !EnsureShaders(ctx))
        return false;

    Flight& flight = m_flights[frameSlot];
    const size_t bytes = batch.instances.size() * sizeof(SpriteInstance2D);
    if (bytes > flight.capacity || flight.instances.empty()) {
        size_t newCapacity = std::max<size_t>(flight.capacity, 4096);
        while (newCapacity < bytes) {
            if (newCapacity > std::numeric_limits<size_t>::max() / 2)
                return false;
            newCapacity *= 2;
        }
        auto newBuffer =
            ctx.CreateBuffer(newCapacity, smvk::BufferType_Storage, "world2dInstances");
        if (newBuffer.empty())
            return false;
        const auto logical = rg.AddExternalBuffer(newBuffer, "world2dInstances");
        if (logical.empty())
            return false;
        if (!flight.logical.empty())
            rg.FreeBuffer(flight.logical);
        flight.instances = std::move(newBuffer);
        flight.logical = logical;
        flight.capacity = newCapacity;
    }
    ctx.UploadDataToBuffer(flight.instances,
                           batch.instances.data(),
                           bytes,
                           smvk::PipelineStageBit_Vertex,
                           smvk::AccessFlags::ShaderRead);
    ctx.FlushUploads();

    Push push{};
    push.instances = ctx.GetBufferDeviceAddress(flight.instances);
    if (!push.instances)
        return false;
    push.camera[0] = m_camera.x;
    push.camera[1] = m_camera.y;
    push.viewport[0] = float(extent.width);
    push.viewport[1] = float(extent.height);
    push.pixelsPerUnit = m_pixelsPerUnit;
    push.minZ = batch.minZ;
    push.zRange = batch.maxZ > batch.minZ ? batch.maxZ - batch.minZ : 1.0f;
    push.samplerIndex = m_sampler;
    auto runs = batch.runs;
    auto pages = std::vector<uint32_t>{};
    pages.reserve(m_pages.size());
    for (const auto& p : m_pages)
        pages.push_back(p.bindless);
    auto program = m_program;
    auto& pass = rg.AddPass("World2D");
    pass.SetExtent(extent).SetColor({colorTarget},
                                    clear ? smvk::AttachmentLoadOp::Clear
                                          : smvk::AttachmentLoadOp::Load,
                                    smvk::AttachmentStoreOp::Store,
                                    smvk::ClearColor(0, 0, 0, 0));
    smvk::BufferSyncRequest read{};
    read.barrierMasks.stageMask = smvk::PipelineStageBit_Vertex;
    read.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
    pass.AddBufferHint(flight.logical, read);
    pass.SetRecordingFunc([runs = std::move(runs), pages = std::move(pages), push, program](
                              smvk::CommandBuffer& cmd) mutable {
        cmd.BindRenderPipeline(program,
                               tour::vertex_layouts::Empty,
                               smvk::DepthSettings::Disabled(),
                               smvk::StencilSettings::DefaultDisabled(),
                               smvk::BlendSettings::AlphaBlend(),
                               smvk::MSAASettings::Count1(),
                               smvk::RasterSettings{.cullMode = smvk::CullMode::None});
        for (const auto& run : runs) {
            Push batchPush = push;
            batchPush.textureIndex = pages[run.page];
            cmd.PushConstants(0, sizeof(Push), &batchPush, smvk::ShaderStage::AllGraphics);
            cmd.Draw(6, run.count, 0, run.firstInstance);
        }
    });
    return true;
}

void World2DRenderer::Destroy(smvk::RenderGraph& rg) {
    for (auto& f : m_flights) {
        if (!f.logical.empty())
            rg.FreeBuffer(f.logical);
        f = {};
    }
    m_pages.clear();
    m_uploadedRevision = UINT64_MAX;
    m_sampler = UINT32_MAX;
    // NOTE: the paren form (never `= {}` / `= ShaderProgram{}`) - GCC
    // gimplify rejects brace-init of this aggregate (smvk.h ShaderProgram).
    m_program = smvk::ShaderProgram();
    m_vs = {};
    m_fs = {};
}

} // namespace tour
