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

#include <Engine/Rendergraph/hdr.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
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
    if (!std::isfinite(length) || length < 1e-6f ||
        !std::isfinite(plane.w))
        return false;
    out = plane / length;
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
    const glm::mat4& projection, const glm::vec3& eye,
    const glm::vec4& receiverPlane, PlanarReflectionCamera& out) {
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

// External capture owned by the host. 'logicalView' must be registered in
// WorldRenderer's RenderGraph and remain alive through execution; 'bindlessIndex' is a shader-readable Texture2D slot in the SAME
// ResourceManager. viewProjection must match the capture's camera (including
// its Vulkan Y flip). A producer must not sample the output of this pass.
struct PlanarReflectionCapture {
    LogicalImageViewHandle logicalView{};
    uint32_t bindlessIndex = UINT32_MAX;
    Format format = Format::RGBA16_Float;
    glm::mat4 viewProjection{1.0f};
    glm::vec4 receiverPlane{0.0f, 1.0f, 0.0f, 0.0f};
    float thickness = 0.05f;  // receiver depth distance tolerance, meters
    float strength = 1.0f;    // 0 = off; 1 = physically weighted Fresnel
    float f0 = 1.0f;          // 1 = mirror, ~0.04 = dielectric
};

// Must match planar_reflection.frag.hlsl. 128 bytes = minimum Vulkan
// push-constant guarantee. A frameUniforms BDA supplies invViewProj and
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
};
static_assert(sizeof(PlanarReflectionPush) == 128);
static_assert(offsetof(PlanarReflectionPush, reflectedViewProjection) == 16);
static_assert(offsetof(PlanarReflectionPush, receiverPlane) == 80);
static_assert(offsetof(PlanarReflectionPush, hdrTextureIndex) == 96);

inline constexpr const char* kPlanarReflectionVs =
    "content/shaders/tonemap.vert.hlsl.spv";
inline constexpr const char* kPlanarReflectionPs =
    "content/shaders/planar_reflection.frag.hlsl.spv";

struct PlanarReflections {
    HdrTarget output;
    Holder<ShaderModuleHandle> vsModule;
    Holder<ShaderModuleHandle> psModule;
    ShaderProgram program{};

    [[nodiscard]] bool Valid() const {
        return output.Valid() && !output.logicalView.empty() &&
            program.shaderCount == 2;
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
        Shader vs{ .module = vsModule, .entryPoint = "mainVS",
            .stage = ShaderStage::Vertex };
        Shader ps{ .module = psModule, .entryPoint = "mainPS",
            .stage = ShaderStage::Fragment };
        program = { .shaders = { vs, ps }, .shaderCount = 2 };
        return true;
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
    bool AddCompositePass(RenderGraph& rg, LogicalImageViewHandle hdrView,
        uint32_t hdrIndex, LogicalImageViewHandle captureView,
        uint32_t captureIndex, LogicalImageViewHandle depthView,
        uint32_t depthIndex, uint64_t frameUniformsAddress,
        uint32_t samplerIndex, const PlanarReflectionCapture& capture) {
        glm::vec4 plane{};
        if (!Valid() || hdrView.empty() || captureView.empty() ||
            hdrView == captureView || captureView == output.logicalView ||
            depthView.empty() || hdrIndex == UINT32_MAX ||
            captureIndex == UINT32_MAX || depthIndex == UINT32_MAX ||
            frameUniformsAddress == 0 || capture.strength <= 0.0f ||
            !NormalizePlanarPlane(capture.receiverPlane, plane))
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

        auto& pass = rg.AddPass("PlanarReflectionComposite");
        pass.SetOffset(0, 0)
            .SetExtent(output.extent)
            .SetInputImages({ hdrView, captureView, depthView })
            .SetColor({ output.logicalView },
                AttachmentLoadOp::DontCare, AttachmentStoreOp::Store);
        ShaderProgram programCopy = program;
        pass.SetRecordingFunc([programCopy, push](CommandBuffer& cmd) mutable {
            cmd.BindRenderPipeline(programCopy, VertexLayoutId::Empty,
                DepthSettings::Disabled(), StencilSettings::DefaultDisabled(),
                BlendSettings::Opaque(), MSAASettings::Count1(),
                RasterSettings{ .cullMode = CullMode::None });
            cmd.PushConstants(0, sizeof(PlanarReflectionPush), &push,
                ShaderStage::All);
            cmd.Draw(3, 1, 0, 0);
        });
        return true;
    }

    [[nodiscard]] bool ReloadShaders(ResourceManager& ctx,
        std::vector<ShaderModuleHandle>& retired) {
        bool changed = false;
        changed |= SwapShaderModule(ctx, kPlanarReflectionVs,
            "planarReflectionVS", vsModule, retired);
        changed |= SwapShaderModule(ctx, kPlanarReflectionPs,
            "planarReflectionPS", psModule, retired);
        if (changed) {
            program = ShaderProgram{};
            EnsureShaders(ctx);
        }
        return changed;
    }

    void Destroy(RenderGraph& rg) {
        output.Destroy(rg);
        program = ShaderProgram{};
        vsModule = {};
        psModule = {};
    }
};

} // namespace smvk
