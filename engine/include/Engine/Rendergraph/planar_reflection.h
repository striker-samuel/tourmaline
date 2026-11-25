// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Planar reflection: reflected-camera math + a pre-tonemap HDR compositor.
//
// The caller renders the reflected camera into an RGBA16F sampled image,
// clipping geometry behind the mirror plane and reversing front-face culling
// (the reflection transform has negative determinant). WorldRenderer consumes
// that external image; it does not recursively render the world.
//
// The composite uses the main camera's reversed-Z depth to select pixels
// belonging to the receiver plane and projects their world positions through
// the reflected camera's view-projection. It therefore does not rely on SSR
// hits, and can show geometry outside the main camera frustum.

#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/vertex_layouts.h>
#include <Engine/Rendergraph/hdr.h>
#include <Engine/Rendergraph/depth.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/Rendergraph/renderer.h>
#include <Engine/Utils/shader_library.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace smvk {

// A world-space plane: dot(normal, point) + w == 0. The orientation should
// face the main camera, but either orientation produces the same reflection.
inline bool NormalizePlanarPlane(const glm::vec4& plane, glm::vec4& out) {
    const float length = glm::length(glm::vec3(plane));
    if (!std::isfinite(length) || length < 1e-6f || !std::isfinite(plane.w))
        return false;
    const glm::vec4 normalized = plane / length;
    if (!std::isfinite(normalized.w))
        return false;
    out = normalized;
    return true;
}

struct PlanarReflectionCamera {
    glm::mat4 view{1.0f};
    glm::mat4 viewProjection{1.0f};
    glm::vec3 eye{0.0f};
    glm::vec4 plane{0.0f, 1.0f, 0.0f, 0.0f};
    // Reflections reverse the determinant. Capture rasterizers must flip
    // culling (or use none), and clip away the half-space behind the plane.
    bool reverseWinding = true;
};

// Reflected camera using the SAME projection (including Vulkan's Y flip and
// any jitter) as the main view. No oblique projection is silently applied:
// capture producers choose a GPU clip plane / oblique near clip explicitly.
inline bool MakePlanarReflectionCamera(const glm::mat4& view,
                                       const glm::mat4& projection,
                                       const glm::vec3& eye,
                                       const glm::vec4& receiverPlane,
                                       PlanarReflectionCamera& out) {
    glm::vec4 p{};
    if (!NormalizePlanarPlane(receiverPlane, p))
        return false;

    // Reflection across n.p + d = 0: p' = p - 2*n*(dot(n,p)+d).
    glm::mat4 reflection(1.0f);
    for (int column = 0; column < 3; ++column)
        for (int row = 0; row < 3; ++row)
            reflection[column][row] -= 2.0f * p[column] * p[row];
    for (int row = 0; row < 3; ++row)
        reflection[3][row] = -2.0f * p.w * p[row];

    out.view = view * reflection;
    out.viewProjection = projection * out.view;
    out.eye = glm::vec3(reflection * glm::vec4(eye, 1.0f));
    out.plane = p;
    return true;
}

// Replace the near plane of a Vulkan reversed-Z projection (0 <= z <= w).
// keepPlane is in world space; its positive half-space remains visible.
// The reflected eye must be on the negative side. This works with finite
// and infinite far perspective projections; it changes the far clip plane.
// Use the resulting VP for both capture rendering and compositing.
inline bool MakePlanarObliqueProjection(const glm::mat4& view,
                                        const glm::mat4& projection,
                                        const glm::vec4& keepPlane,
                                        glm::mat4& out) {
    glm::vec4 plane{};
    if (!NormalizePlanarPlane(keepPlane, plane))
        return false;
    const glm::vec4 cameraPlane = glm::transpose(glm::inverse(view)) * plane;
    if (!(cameraPlane.w < -1e-6f) || !std::isfinite(cameraPlane.w))
        return false;

    // Pick the far clip corner most deeply inside the retained half-space.
    // Evaluating all four corners also accommodates flipped/asymmetric projections.
    const glm::mat4 invProjection = glm::inverse(projection);
    float best = 0.0f;
    for (int x : {-1, 1}) {
        for (int y : {-1, 1}) {
            const glm::vec4 q = invProjection * glm::vec4(float(x), float(y), 0.0f, 1.0f);
            const float distance = glm::dot(cameraPlane, q);
            if (std::isfinite(distance))
                best = std::max(best, distance);
        }
    }
    if (!(best > 1e-6f))
        return false;

    // Near clipping in reversed Z is w - z >= 0. Set w - z to the
    // scaled camera plane, preserving x, y and w (and thus projected UV).
    glm::mat4 candidate = projection;
    for (int column = 0; column < 4; ++column) {
        candidate[column][2] = projection[column][3] - cameraPlane[column] / best;
        if (!std::isfinite(candidate[column][2]))
            return false;
    }
    out = candidate;
    return true;
}

// External capture owned by the host. Supply a physicalView (the renderer
// imports it) or a logicalView already registered in its graph. The source
// view must stay alive through all in-flight frames; 'bindlessIndex' is a shader-readable Texture2D
// slot in the SAME ResourceManager. viewProjection must match the capture's camera (including its
// Vulkan Y flip). A producer must not sample the output of this pass.
// One opt-in, bounded-cost internal capture. A positive receiverRadius
// enables conservative receiver visibility culling (world-space sphere).
struct PlanarReflectionSettings {
    glm::vec4 receiverPlane{0, 1, 0, 0};
    glm::vec3 receiverCenter{0};
    float receiverRadius = 0;
    float maxDistance = 80;
    uint32_t maxDraws = 512;
    uint32_t resolutionDivisor = 2;
    bool includeMasked = false;
    // Always use the lowest resident cooked mesh LOD.
    float thickness = 0.05f;
    float strength = 1;
    float f0 = 0.04f;
};
inline Extent2D PlanarCaptureExtent(Extent2D main, uint32_t divisor) {
    divisor = std::clamp(divisor, 2u, 4u);
    return {std::max(1u, main.width / divisor), std::max(1u, main.height / divisor)};
}

// Clip a conservative square enclosing the circular receiver against the
// camera clip volume, then crop XY. The square's projected area collapses
// at grazing angles. Near-plane crossings are clipped before division.
struct PlanarReceiverCrop {
    glm::mat4 transform{1.0f};
    Extent2D extent{};
};
inline bool MakePlanarReceiverCrop(const glm::mat4& vp, const glm::vec4& plane,
                                   const glm::vec3& center, float radius,
                                   Extent2D fullExtent, PlanarReceiverCrop& out) {
    out = {glm::mat4(1), fullExtent};
    if (!(radius > 0))
        return true;
    const glm::vec3 n(plane);
    const glm::vec3 c = center - n * (glm::dot(n, center) + plane.w);
    const glm::vec3 u = glm::normalize(glm::cross(n,
        std::abs(n.y) < 0.9f ? glm::vec3(0,1,0) : glm::vec3(1,0,0)));
    const glm::vec3 v = glm::cross(n, u);
    std::vector<glm::vec4> poly{
        vp * glm::vec4(c - radius*u - radius*v, 1),
        vp * glm::vec4(c + radius*u - radius*v, 1),
        vp * glm::vec4(c + radius*u + radius*v, 1),
        vp * glm::vec4(c - radius*u + radius*v, 1)};
    const glm::vec4 clipPlanes[] = {{1,0,0,1},{-1,0,0,1},
        {0,1,0,1},{0,-1,0,1},{0,0,1,0},{0,0,-1,1}};
    for (const auto& p : clipPlanes) {
        if (poly.empty())
            return false;
        std::vector<glm::vec4> next;
        auto prev = poly.back();
        float a = glm::dot(p, prev);
        for (const auto& curr : poly) {
            const float b = glm::dot(p, curr);
            if ((a >= 0) != (b >= 0))
                next.push_back(glm::mix(prev, curr, a / (a-b)));
            if (b >= 0)
                next.push_back(curr);
            prev = curr;
            a = b;
        }
        poly.swap(next);
    }
    if (poly.empty())
        return false;
    glm::vec2 lo(1), hi(-1);
    for (const auto& p : poly) {
        if (!(p.w > 1e-6f))
            return true; // conservative full-view fallback at the eye
        const glm::vec2 ndc = glm::vec2(p) / p.w;
        lo = glm::min(lo, ndc);
        hi = glm::max(hi, ndc);
    }
    // Pad by two capture texels for jitter/filtering. Quantize to 32 pixel
    // steps to avoid resizing on every small camera movement.
    const glm::vec2 size(float(fullExtent.width), float(fullExtent.height));
    lo = glm::max(glm::vec2(-1), lo - 4.0f / size);
    hi = glm::min(glm::vec2(1), hi + 4.0f / size);
    const glm::vec2 span = hi - lo;
    if (!(span.x > 0 && span.y > 0))
        return false;
    const glm::vec2 scale = 2.0f / span;
    out.transform[0][0] = scale.x;
    out.transform[1][1] = scale.y;
    out.transform[3][0] = -(hi.x + lo.x) / span.x;
    out.transform[3][1] = -(hi.y + lo.y) / span.y;
    auto quantize = [](float px, uint32_t limit) {
        return std::min(limit, std::max(1u, uint32_t(std::ceil(px / 32.0f)) * 32u));
    };
    out.extent = {quantize(span.x * size.x * 0.5f, fullExtent.width),
                  quantize(span.y * size.y * 0.5f, fullExtent.height)};
    return true;
}

struct PlanarReflectionCapture {
    // Optional ready-to-use logical view. Otherwise WorldRenderer imports the
    // host's physical image view once into its own rendergraph.
    LogicalImageViewHandle logicalView{};
    ImageViewHandle physicalView{};
    uint32_t bindlessIndex = UINT32_MAX;
    Format format = Format::RGBA16_Float;
    glm::mat4 viewProjection{1.0f};
    glm::vec4 receiverPlane{0.0f, 1.0f, 0.0f, 0.0f};
    float thickness = 0.05f; // receiver depth distance tolerance, meters
    float strength = 1.0f;   // 0 = off; 1 = physically weighted Fresnel
    float f0 = 1.0f;         // 1 = mirror, ~0.04 = dielectric
    bool coverageAlpha = false; // internal capture: alpha zero leaves SSR/IBL intact
    glm::vec3 receiverCenter{0};
    float receiverRadius = 0; // optional finite circular receiver bound
};

// Must match planar_reflection.frag.hlsl. 144 bytes, within the engine's
// existing 256-byte push-constant requirement. A frameUniforms BDA supplies invViewProj and
// cameraPos to avoid two extra matrices in the push.
struct PlanarReflectionPush {
    uint64_t frameUniforms = 0;
    uint64_t reserved = 0;
    glm::mat4 reflectedViewProjection{1.0f};
    glm::vec4 receiverPlane{0.0f, 1.0f, 0.0f, 0.0f};
    uint32_t hdrTextureIndex = UINT32_MAX;
    uint32_t captureTextureIndex = UINT32_MAX;
    uint32_t depthTextureIndex = UINT32_MAX;
    uint32_t samplerIndex = 0;
    float strength = 1.0f;
    float thickness = 0.05f;
    float f0 = 1.0f;
    float pad0 = 0.0f;
    glm::vec4 receiverBounds{0.0f};
};
static_assert(sizeof(PlanarReflectionPush) == 144);
static_assert(offsetof(PlanarReflectionPush, receiverBounds) == 128);
static_assert(offsetof(PlanarReflectionPush, reflectedViewProjection) == 16);
static_assert(offsetof(PlanarReflectionPush, receiverPlane) == 80);
static_assert(offsetof(PlanarReflectionPush, hdrTextureIndex) == 96);

inline constexpr const char* kPlanarReflectionVs = "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kPlanarReflectionPs =
    "content/shaders/planar_reflection.frag.hlsl.spv";

struct PlanarReflections {
    HdrTarget output;
    HdrTarget internalColor;
    DepthTarget internalDepth;
    Holder<ShaderModuleHandle> vsModule;
    Holder<ShaderModuleHandle> psModule;
    ShaderProgram program{};
    Holder<ShaderModuleHandle> captureVsModule;
    Holder<ShaderModuleHandle> capturePsModule;
    ShaderProgram captureProgram{};
    Holder<ShaderModuleHandle> terrainVs, terrainFarVs, terrainPs;
    ShaderProgram terrainNearProgram{}, terrainFarProgram{};

    struct Frame {
        ShaderProgram* program = nullptr;
        PlanarReflectionPush push{};
    };
    static void Record(CommandBuffer& cmd, const FrameContext& fc, const PassData& pd) {
        const Frame& f = fc.rg->FrameData().Get<Frame>(pd.frameSlot);
        cmd.BindRenderPipeline(*f.program,
                               ::tour::vertex_layouts::Empty,
                               DepthSettings::Disabled(),
                               StencilSettings::DefaultDisabled(),
                               BlendSettings::Opaque(),
                               MSAASettings::Count1(),
                               RasterSettings{.cullMode = CullMode::None});
        cmd.PushConstants(0, sizeof(PlanarReflectionPush), &f.push, ShaderStage::All);
        cmd.Draw(3, 1, 0, 0);
    }

    [[nodiscard]] bool Valid() const {
        return output.Valid() && !output.logicalView.empty() && program.shaderCount == 2;
    }
    [[nodiscard]] LogicalImageViewHandle OutputView() const {
        return output.logicalView;
    }
    [[nodiscard]] uint32_t OutputBindless() const {
        return output.bindlessIndex;
    }

    bool EnsureShaders(ResourceManager& ctx) {
        if (program.shaderCount == 2)
            return true;
        try {
            if (vsModule.empty())
                vsModule = ctx.CreateShaderModule(shaders::Read(kPlanarReflectionVs),
                                                  "planarReflectionVS");
            if (psModule.empty())
                psModule = ctx.CreateShaderModule(shaders::Read(kPlanarReflectionPs),
                                                  "planarReflectionPS");
        } catch (...) {
            return false; // optional feature: no shader deployment, no pass
        }
        if (vsModule.empty() || psModule.empty())
            return false;
        Shader vs{.module = vsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex};
        Shader ps{.module = psModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment};
        program = {.shaders = {vs, ps}, .shaderCount = 2};
        return true;
    }

    bool EnsureCaptureShaders(ResourceManager& ctx) {
        if (captureProgram.shaderCount == 2)
            return true;
        try {
            if (captureVsModule.empty())
                captureVsModule = ctx.CreateShaderModule(
                    shaders::Read("content/shaders/default.vert.hlsl.spv"), "planarCaptureVS");
            if (capturePsModule.empty())
                capturePsModule = ctx.CreateShaderModule(
                    shaders::Read("content/shaders/planar_capture.frag.hlsl.spv"), "planarCapturePS");
        } catch (...) {
            return false;
        }
        if (captureVsModule.empty() || capturePsModule.empty())
            return false;
        captureProgram = {.shaders = {
            Shader{.module = captureVsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex},
            Shader{.module = capturePsModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment}},
            .shaderCount = 2};
        return true;
    }

    bool EnsureTerrainShaders(ResourceManager& ctx) {
        if (terrainNearProgram.shaderCount == 2 && terrainFarProgram.shaderCount == 2)
            return true;
        try {
            if (terrainVs.empty())
                terrainVs = ctx.CreateShaderModule(shaders::Read("content/shaders/terrain.vert.hlsl.spv"), "planarTerrainVS");
            if (terrainFarVs.empty())
                terrainFarVs = ctx.CreateShaderModule(shaders::Read("content/shaders/terrainfar.vert.hlsl.spv"), "planarTerrainFarVS");
            if (terrainPs.empty())
                terrainPs = ctx.CreateShaderModule(shaders::Read("content/shaders/planar_terrain.frag.hlsl.spv"), "planarTerrainPS");
        } catch (...) { return false; }
        if (terrainVs.empty() || terrainFarVs.empty() || terrainPs.empty())
            return false;
        Shader ps{.module = terrainPs, .entryPoint = "mainPS", .stage = ShaderStage::Fragment};
        terrainNearProgram = {.shaders = {
            Shader{.module = terrainVs, .entryPoint = "mainVS", .stage = ShaderStage::Vertex}, ps}, .shaderCount = 2};
        terrainFarProgram = {.shaders = {
            Shader{.module = terrainFarVs, .entryPoint = "mainVS", .stage = ShaderStage::Vertex}, ps}, .shaderCount = 2};
        return true;
    }

    bool EnsureCapture(ResourceManager& ctx, RenderGraph& rg, Extent2D extent, Format depthFormat) {
        HdrDesc color{};
        color.name = "planarCapture";
        color.viewName = "planarCaptureView";
        color.logicalName = "planarCaptureLogical";
        DepthDesc depth{};
        depth.format = depthFormat;
        depth.name = "planarCaptureDepth";
        depth.viewName = "planarCaptureDepthView";
        depth.logicalName = "planarCaptureDepthLogical";
        return EnsureCaptureShaders(ctx) && internalColor.Ensure(ctx, rg, extent, color) &&
               internalDepth.Ensure(ctx, rg, extent, depth);
    }

    bool Ensure(ResourceManager& ctx, RenderGraph& rg, Extent2D extent) {
        HdrDesc desc{};
        desc.name = "planarReflection";
        desc.viewName = "planarReflectionView";
        desc.logicalName = "planarReflectionLogical";
        const bool targetOk = output.Ensure(ctx, rg, extent, desc);
        return EnsureShaders(ctx) && targetOk;
    }

    // Emit one screen-space receiver composite. All three sources are
    // declared for synchronization; HDR is read from a DIFFERENT image
    // from the output, so no read/write feedback loop is introduced.
    bool AddCompositePass(RenderGraph& rg,
                          LogicalImageViewHandle hdrView,
                          uint32_t hdrIndex,
                          LogicalImageViewHandle captureView,
                          uint32_t captureIndex,
                          LogicalImageViewHandle depthView,
                          uint32_t depthIndex,
                          uint64_t frameUniformsAddress,
                          uint32_t samplerIndex,
                          const PlanarReflectionCapture& capture) {
        glm::vec4 plane{};
        if (!Valid() || hdrView.empty() || captureView.empty() || hdrView == captureView ||
            captureView == output.logicalView || depthView.empty() || hdrIndex == UINT32_MAX ||
            captureIndex == UINT32_MAX || depthIndex == UINT32_MAX || frameUniformsAddress == 0 ||
            capture.strength <= 0.0f || !NormalizePlanarPlane(capture.receiverPlane, plane))
            return false;

        PlanarReflectionPush push{};
        push.frameUniforms = frameUniformsAddress;
        push.reflectedViewProjection = capture.viewProjection;
        push.receiverPlane = plane;
        push.hdrTextureIndex = hdrIndex;
        push.captureTextureIndex = captureIndex;
        push.depthTextureIndex = depthIndex;
        push.samplerIndex = samplerIndex;
        push.strength = std::clamp(capture.strength, 0.0f, 1.0f);
        push.thickness = std::max(0.001f, capture.thickness);
        push.f0 = std::clamp(capture.f0, 0.0f, 1.0f);
        push.pad0 = capture.coverageAlpha ? 1.0f : 0.0f;
        push.receiverBounds = glm::vec4(capture.receiverCenter, capture.receiverRadius);

        auto& pass = rg.AddPass("PlanarReflectionComposite");
        pass.SetOffset(0, 0)
            .SetExtent(output.extent)
            .SetInputImages({hdrView, captureView, depthView})
            .SetColor({output.logicalView}, AttachmentLoadOp::DontCare, AttachmentStoreOp::Store);
        Frame& frame = rg.PassFrameData<Frame>(pass);
        frame.program = &program;
        frame.push = push;
        pass.SetRecordFn(&Record);
        return true;
    }

    [[nodiscard]] bool ReloadShaders(ResourceManager& ctx,
                                     std::vector<ShaderModuleHandle>& retired) {
        bool changed = false;
        changed |=
            SwapShaderModule(ctx, kPlanarReflectionVs, "planarReflectionVS", vsModule, retired);
        changed |=
            SwapShaderModule(ctx, kPlanarReflectionPs, "planarReflectionPS", psModule, retired);
        if (changed) {
            program = ShaderProgram{};
            EnsureShaders(ctx);
        }
        if (captureProgram.shaderCount == 2) {
            bool captureChanged = false;
            captureChanged |= SwapShaderModule(ctx, "content/shaders/default.vert.hlsl.spv",
                                                "planarCaptureVS", captureVsModule, retired);
            captureChanged |= SwapShaderModule(ctx, "content/shaders/planar_capture.frag.hlsl.spv",
                                                "planarCapturePS", capturePsModule, retired);
            if (captureChanged) {
                captureProgram = {};
                EnsureCaptureShaders(ctx);
            }
            changed |= captureChanged;
        }
        if (terrainNearProgram.shaderCount == 2) {
            bool terrainChanged = false;
            terrainChanged |= SwapShaderModule(ctx, "content/shaders/terrain.vert.hlsl.spv", "planarTerrainVS", terrainVs, retired);
            terrainChanged |= SwapShaderModule(ctx, "content/shaders/terrainfar.vert.hlsl.spv", "planarTerrainFarVS", terrainFarVs, retired);
            terrainChanged |= SwapShaderModule(ctx, "content/shaders/planar_terrain.frag.hlsl.spv", "planarTerrainPS", terrainPs, retired);
            if (terrainChanged) {
                terrainNearProgram = {};
                terrainFarProgram = {};
                EnsureTerrainShaders(ctx);
            }
            changed |= terrainChanged;
        }
        return changed;
    }

    void Destroy(RenderGraph& rg) {
        terrainNearProgram = {};
        terrainFarProgram = {};
        terrainVs = {};
        terrainFarVs = {};
        terrainPs = {};
        captureProgram = {};
        captureVsModule = {};
        capturePsModule = {};
        internalDepth.Destroy(rg);
        internalColor.Destroy(rg);
        output.Destroy(rg);
        program = ShaderProgram{};
        vsModule = {};
        psModule = {};
    }
};

} // namespace smvk
