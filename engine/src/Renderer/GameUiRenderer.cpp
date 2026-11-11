// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Renderer/GameUiRenderer.h>
#include <Engine/Renderer/vertex_layouts.h>
#include <Engine/Utils/shader_library.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace tour {
namespace {

bool Finite(glm::vec2 p) {
    return std::isfinite(p.x) && std::isfinite(p.y);
}
bool Finite(glm::vec4 p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::isfinite(p.w);
}
bool Valid(UiRect2D r) {
    return Finite(r.origin) && Finite(r.size) && r.size.x > 0.0f && r.size.y > 0.0f &&
           std::isfinite(r.Right()) && std::isfinite(r.Bottom());
}

} // namespace

std::vector<GameUiPreparedRect> PrepareGameUiRects(std::span<const UiDrawRect2D> rects,
                                                   smvk::Extent2D viewport) {
    std::vector<GameUiPreparedRect> prepared;
    if (!viewport.width || !viewport.height)
        return prepared;
    prepared.reserve(rects.size());
    const UiRect2D canvas{{0.0f, 0.0f}, {float(viewport.width), float(viewport.height)}};
    for (const auto& d : rects) {
        if (!Valid(d.bounds) || !Finite(d.tint) || !Finite(d.uvMin) || !Finite(d.uvMax))
            continue;
        UiRect2D clip = canvas;
        if (d.clip) {
            if (!Valid(*d.clip))
                continue;
            clip = IntersectUiRects(clip, *d.clip);
        }
        clip = IntersectUiRects(clip, d.bounds);
        if (clip.size.x <= 0.0f || clip.size.y <= 0.0f)
            continue;
        GameUiPreparedRect p{};
        auto& instance = p.instance;
        instance.origin[0] = d.bounds.origin.x;
        instance.origin[1] = d.bounds.origin.y;
        instance.size[0] = d.bounds.size.x;
        instance.size[1] = d.bounds.size.y;
        instance.uvMin[0] = d.uvMin.x;
        instance.uvMin[1] = d.uvMin.y;
        instance.uvMax[0] = d.uvMax.x;
        instance.uvMax[1] = d.uvMax.y;
        instance.tint[0] = d.tint.r;
        instance.tint[1] = d.tint.g;
        instance.tint[2] = d.tint.b;
        instance.tint[3] = d.tint.a;
        instance.clipMin[0] = clip.origin.x;
        instance.clipMin[1] = clip.origin.y;
        instance.clipMax[0] = clip.Right();
        instance.clipMax[1] = clip.Bottom();
        instance.pad = d.effect; // 72-byte instance ABI: previously unused word
        p.texture = d.texture;
        prepared.push_back(p);
    }
    return prepared;
}

// SpriteSet is metadata only: callers keep owning/loading the atlas texture.
// No repacking or extra descriptor is required for atlas-backed GameUI rects.
bool GameUiRenderer::DrawSprite(const asset::SpriteSet& set,
                                std::string_view name,
                                UiRect2D bounds,
                                smvk::TextureHandle texture,
                                glm::vec4 tint,
                                std::optional<UiRect2D> clip) {
    const auto* sprite = set.Find(name);
    if (!sprite || texture.empty() || !Valid(bounds) || !Finite(tint) || (clip && !Valid(*clip)) ||
        !set.width || !set.height || m_rects.size() >= kMaxRects)
        return false;
    // A trimmed region retains its original logical placement. For ordinary
    // HUD art this maps its visible crop onto the requested untrimmed bounds.
    if (!sprite->sourceWidth || !sprite->sourceHeight)
        return false;
    UiDrawRect2D d{};
    d.bounds = bounds;
    d.bounds.origin += glm::vec2(bounds.size.x * (float(sprite->trimX) / sprite->sourceWidth),
                                 bounds.size.y * (float(sprite->trimY) / sprite->sourceHeight));
    d.bounds.size *= glm::vec2(float(sprite->rect.width) / sprite->sourceWidth,
                               float(sprite->rect.height) / sprite->sourceHeight);
    if (!Valid(d.bounds))
        return false;
    const auto uv = set.UV(*sprite);
    d.uvMin = {uv[0], uv[1]};
    d.uvMax = {uv[2], uv[3]};
    d.texture = texture;
    d.tint = tint;
    d.clip = clip;
    DrawRect(d);
    return true;
}

bool GameUiRenderer::DrawNineSlice(const asset::SpriteSet& set,
                                   std::string_view name,
                                   UiRect2D bounds,
                                   smvk::TextureHandle texture,
                                   glm::vec4 tint,
                                   std::optional<UiRect2D> clip) {
    const auto* sprite = set.Find(name);
    if (!sprite || texture.empty() || !Valid(bounds) || !Finite(tint) || (clip && !Valid(*clip)) ||
        !set.width || !set.height)
        return false;
    const auto& r = sprite->rect;
    const auto& b = sprite->border; // left, top, right, bottom
    if (!r.width || !r.height || !sprite->sourceWidth || !sprite->sourceHeight ||
        uint64_t(r.x) + r.width > set.width || uint64_t(r.y) + r.height > set.height ||
        uint64_t(b[0]) + b[2] > r.width || uint64_t(b[1]) + b[3] > r.height)
        return false;
    if (!(b[0] || b[1] || b[2] || b[3]))
        return DrawSprite(set, name, bounds, texture, tint, clip);
    // The asset contract prohibits trimming nine-sliced art.
    if (sprite->trimX || sprite->trimY || sprite->sourceWidth != r.width ||
        sprite->sourceHeight != r.height)
        return false;

    const float srcX[4] = {float(r.x),
                           float(uint64_t(r.x) + b[0]),
                           float(uint64_t(r.x) + r.width - b[2]),
                           float(uint64_t(r.x) + r.width)};
    const float srcY[4] = {float(r.y),
                           float(uint64_t(r.y) + b[1]),
                           float(uint64_t(r.y) + r.height - b[3]),
                           float(uint64_t(r.y) + r.height)};
    const auto scaleBorders = [](float a, float z, float extent) {
        const float sum = a + z;
        return sum > extent && sum > 0.0f ? extent / sum : 1.0f;
    };
    const float sx = scaleBorders(float(b[0]), float(b[2]), bounds.size.x);
    const float sy = scaleBorders(float(b[1]), float(b[3]), bounds.size.y);
    const float dstX[4] = {bounds.origin.x,
                           bounds.origin.x + float(b[0]) * sx,
                           (float(b[0]) + float(b[2]) >= bounds.size.x)
                               ? bounds.origin.x + float(b[0]) * sx
                               : bounds.Right() - float(b[2]) * sx,
                           bounds.Right()};
    const float dstY[4] = {bounds.origin.y,
                           bounds.origin.y + float(b[1]) * sy,
                           (float(b[1]) + float(b[3]) >= bounds.size.y)
                               ? bounds.origin.y + float(b[1]) * sy
                               : bounds.Bottom() - float(b[3]) * sy,
                           bounds.Bottom()};
    // Preflight to guarantee atomic submission at the fixed instance limit.
    size_t count = 0;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            if (dstX[x + 1] > dstX[x] && dstY[y + 1] > dstY[y] && srcX[x + 1] > srcX[x] &&
                srcY[y + 1] > srcY[y])
                ++count;
    if (count > kMaxRects - m_rects.size())
        return false;
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            if (dstX[x + 1] <= dstX[x] || dstY[y + 1] <= dstY[y] || srcX[x + 1] <= srcX[x] ||
                srcY[y + 1] <= srcY[y])
                continue;
            UiDrawRect2D d{};
            d.bounds = {{dstX[x], dstY[y]}, {dstX[x + 1] - dstX[x], dstY[y + 1] - dstY[y]}};
            d.uvMin = {srcX[x] / set.width, srcY[y] / set.height};
            d.uvMax = {srcX[x + 1] / set.width, srcY[y + 1] / set.height};
            d.tint = tint;
            d.texture = texture;
            d.clip = clip;
            DrawRect(d);
        }
    }
    return true;
}

bool GameUiRenderer::EnsureShaders(smvk::ResourceManager& ctx) {
    if (m_program.shaderCount == 2)
        return true;
    try {
        if (m_vs.empty())
            m_vs = ctx.CreateShaderModule(shaders::Read("content/shaders/gameui.vert.hlsl.spv"),
                                          "gameuiVS");
        if (m_fs.empty())
            m_fs = ctx.CreateShaderModule(shaders::Read(m_fragmentShaderPath), "gameuiPS");
    } catch (...) {
        return false;
    }
    if (m_vs.empty() || m_fs.empty())
        return false;
    const smvk::Shader vs{
        .module = m_vs, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex};
    const smvk::Shader ps{
        .module = m_fs, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment};
    m_program = {.shaders = {vs, ps}, .shaderCount = 2};
    return true;
}

bool GameUiRenderer::AppendPass(smvk::ResourceManager& ctx,
                                smvk::RenderGraph& rg,
                                smvk::LogicalImageViewHandle colorTarget,
                                smvk::Extent2D viewport,
                                uint32_t frameSlot,
                                bool clear) {
    if (colorTarget.empty() || !viewport.width || !viewport.height || frameSlot >= m_flights.size())
        return false;

    auto prepared = PrepareGameUiRects(m_rects, viewport);
    if (prepared.empty()) {
        if (clear) {
            rg.AddPass("GameUI clear")
                .SetExtent(viewport)
                .SetColor({colorTarget},
                          smvk::AttachmentLoadOp::Clear,
                          smvk::AttachmentStoreOp::Store,
                          smvk::ClearColor(0, 0, 0, 0))
                .SetRecordingFunc([](smvk::CommandBuffer&) {});
        }
        return true;
    }
    if (prepared.size() > UINT32_MAX || !EnsureShaders(ctx))
        return false;

    // Resolve texture handles every frame: a destroyed/reloaded texture
    // cannot accidentally keep an old bindless descriptor index.
    std::vector<GameUiRectInstance> packed;
    packed.reserve(prepared.size());
    for (auto& p : prepared) {
        if (!p.texture.empty()) {
            p.instance.textureIndex = ctx.GetTextureBindlessIndex(p.texture);
            if (p.instance.textureIndex == UINT32_MAX)
                return false; // never silently render a missing image as a panel
        }
        packed.push_back(p.instance);
    }

    if (m_sampler == UINT32_MAX) {
        smvk::SamplerCreateInfo sampler{};
        sampler.addressU = smvk::AddressMode::ClampToEdge;
        sampler.addressV = smvk::AddressMode::ClampToEdge;

        sampler.addressW = smvk::AddressMode::ClampToEdge;
        if (m_pixelArtSampling) {
            sampler.minFilter = smvk::Filter::Nearest;
            sampler.magFilter = smvk::Filter::Nearest;
            sampler.mipmapMode = smvk::MipmapMode::Nearest;
            sampler.maxLod = 0.0f;
        }
        m_sampler = ctx.GetOrCreateSampler(sampler);
        if (m_sampler == UINT32_MAX)
            return false;
    }

    Flight& flight = m_flights[frameSlot];
    const size_t bytes = packed.size() * sizeof(GameUiRectInstance);
    if (flight.instances.empty() || bytes > flight.capacity) {
        size_t capacity = std::max<size_t>(flight.capacity, 4096);
        while (capacity < bytes) {
            if (capacity > std::numeric_limits<size_t>::max() / 2)
                return false;
            capacity *= 2;
        }
        auto buffer = ctx.CreateBuffer(capacity, smvk::BufferType_Storage, "gameuiInstances");
        if (buffer.empty())
            return false;
        const auto logical = rg.AddExternalBuffer(buffer, "gameuiInstances");
        if (logical.empty())
            return false;
        if (!flight.logical.empty())
            rg.FreeBuffer(flight.logical);
        flight.instances = std::move(buffer);
        flight.logical = logical;
        flight.capacity = capacity;
    }
    ctx.UploadDataToBuffer(flight.instances,
                           packed.data(),
                           bytes,
                           smvk::PipelineStageBit_Vertex,
                           smvk::AccessFlags::ShaderRead);
    ctx.FlushUploads();
    Push push{};
    push.instances = ctx.GetBufferDeviceAddress(flight.instances);
    if (!push.instances)
        return false;
    push.viewport[0] = float(viewport.width);
    push.viewport[1] = float(viewport.height);
    push.samplerIndex = m_sampler;

    auto program = m_program;
    auto& pass = rg.AddPass("GameUI");
    pass.SetExtent(viewport).SetColor({colorTarget},
                                      clear ? smvk::AttachmentLoadOp::Clear
                                            : smvk::AttachmentLoadOp::Load,
                                      smvk::AttachmentStoreOp::Store,
                                      smvk::ClearColor(0, 0, 0, 0));
    smvk::BufferSyncRequest read{};
    read.barrierMasks.stageMask = smvk::PipelineStageBit_Vertex;
    read.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
    pass.AddBufferHint(flight.logical, read);
    const auto instanceCount = static_cast<uint32_t>(packed.size());
    pass.SetRecordingFunc([program, push, instanceCount](smvk::CommandBuffer& cmd) mutable {
        cmd.BindRenderPipeline(program,
                               ::tour::vertex_layouts::Empty,
                               smvk::DepthSettings::Disabled(),
                               smvk::StencilSettings::DefaultDisabled(),
                               smvk::BlendSettings::AlphaBlend(),
                               smvk::MSAASettings::Count1(),
                               smvk::RasterSettings{.cullMode = smvk::CullMode::None});
        cmd.PushConstants(0, sizeof(Push), &push, smvk::ShaderStage::AllGraphics);
        cmd.Draw(6, instanceCount, 0, 0);
    });
    return true;
}

void GameUiRenderer::Destroy(smvk::RenderGraph& rg) {
    for (auto& f : m_flights) {
        if (!f.logical.empty())
            rg.FreeBuffer(f.logical);
        f = {};
    }
    // NOTE: the paren form (never `= {}` / `= ShaderProgram{}`) - GCC
    // gimplify rejects brace-init of this aggregate (smvk.h ShaderProgram).
    m_program = smvk::ShaderProgram();
    m_vs = {};
    m_fs = {};
    m_sampler = UINT32_MAX;
}

} // namespace tour
