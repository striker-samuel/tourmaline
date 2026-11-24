// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Renderer/Renderer.h>
#include <Engine/Renderer/vertex_layouts.h>
#include <Engine/Rendergraph/ForwardPlusRenderer.h>
#include <Engine/Rendergraph/FrameContext.h>

#include <Engine/App/world.h>
#include <Engine/Profile/profile.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Renderer/renderview.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace engine {

namespace {

// Per-flight slot payload + non-capturing record function for the depth
// prepass (static compiled ABI). `config` points to the ForwardPlusRenderer
// (its programs + fp builder are the stable side); per-frame values already
// on AppFrameContext are read directly from it, and the few derived values
// (visibility mask, main view, gbuffer/fallback flags) ride the frame slot.
struct PrepassFrame {
    std::span<const uint8_t> staticMask;
    smvk::RenderView mainView{};
    bool hasMainView = false;
    bool fallbackOnly = false;
    bool gbufferOn = false;
};

void RecordPrepass(smvk::CommandBuffer& cmd,
                   const smvk::FrameContext& base,
                   const smvk::PassData& pd) {
    const AppFrameContext& fc = static_cast<const AppFrameContext&>(base);
    auto* self =
        const_cast<ForwardPlusRenderer*>(static_cast<const ForwardPlusRenderer*>(pd.config));
    const PrepassFrame& f = fc.rg->FrameData().Get<PrepassFrame>(pd.frameSlot);

    const IRenderer::PushConstants& depthPC = fc.pc;
    smvk::World* const world = fc.world;
    const bool lodEnabled = fc.lodEnabled;
    const smvk::LodFrameInputs& lodInputs = fc.lodInputs;
    const std::span<const uint8_t>& staticMask = f.staticMask;
    const smvk::RenderView& mainView = f.mainView;
    const bool hasMainView = f.hasMainView;
    const bool fallbackOnly = f.fallbackOnly;
    const auto& lodFilterable = fc.gfxLodFilterable;
    const auto& gpuRasterOwnsNode = fc.gpuRasterOwnsNode;
    const bool gbufferOn = f.gbufferOn;
    const glm::vec3& ditherEye = fc.eye;
    const uint64_t skinBda = fc.skinDeformedBda;
    const uint32_t skinBase = fc.skinDeformedBase;
    const bool skinValid = fc.skinDeformedValid;

    smvk::CullMode boundMaskedCull = smvk::CullMode::FrontAndBack;
    auto ensureMaskedPipeline = [&](smvk::CullMode cull) {
        if (cull == boundMaskedCull)
            return;
        cmd.BindRenderPipeline(
            gbufferOn ? self->gbufferMaskedProgram : self->prepassMaskedProgram,
            ::tour::vertex_layouts::Static,
            smvk::DepthSettings{.depthFunc = smvk::CompareOp::GreaterEqual,
                                .writeEnable = true,
                                .depthTestEnable = true},
            smvk::StencilSettings{},
            smvk::BlendSettings{},
            smvk::MSAASettings{},
            smvk::RasterSettings{.cullMode = cull});
        cmd.PushConstants(0, sizeof(IRenderer::PushConstants), &depthPC, smvk::ShaderStage::All);
        boundMaskedCull = cull;
    };
    smvk::CullMode boundSolidCull = smvk::CullMode::FrontAndBack;
    auto ensureSolidPipeline = [&](smvk::CullMode cull) {
        if (cull == boundSolidCull)
            return;
        cmd.BindRenderPipeline(gbufferOn ? self->gbufferProgram : self->fp->depthProgram,
                               ::tour::vertex_layouts::Static,
                               smvk::DepthSettings{.depthFunc = smvk::CompareOp::GreaterEqual,
                                                   .writeEnable = true,
                                                   .depthTestEnable = true},
                               smvk::StencilSettings{},
                               smvk::BlendSettings{},
                               smvk::MSAASettings{},
                               smvk::RasterSettings{.cullMode = cull});
        cmd.PushConstants(0, sizeof(IRenderer::PushConstants), &depthPC, smvk::ShaderStage::All);
        boundSolidCull = cull;
    };
    auto drawPrepassNode = [&](const smvk::World::RenderItem& item, bool maskedPhase) {
        if (gpuRasterOwnsNode && gpuRasterOwnsNode(item.node))
            return; // depth/G-buffer already written by GPU scene
        const DitherCloseupState ditherState =
            DitherCloseupStateFor(world->scene, int(item.node), ditherEye);
        if (ditherState == DitherCloseupState::Hidden)
            return;
        const bool ditherFade = ditherState == DitherCloseupState::Fade;
        if (((item.flags & smvk::IteratorFlagBit_Masked) != 0 || ditherFade) != maskedPhase)
            return;
        if (fallbackOnly && lodFilterable && item.material != smvk::World::kInvalidIndex) {
            const uint32_t lod =
                lodEnabled ? world->LodIndexForNode(item.node, item.mesh, lodInputs) : 0u;
            if (lodFilterable(item.node, item.mesh, lod, item.material))
                return;
        }
        if (!staticMask.empty() && item.node < staticMask.size() && staticMask[item.node] == 0)
            return;
        if (item.node >= world->scene.globalTransform.size())
            return;
        auto& model = world->scene.globalTransform[item.node];
        if (hasMainView && (item.flags & smvk::IteratorFlagBit_Static) == 0) {
            if (smvk::StaticMesh* baseDesc = world->GetMesh(item.mesh)) {
                glm::vec3 c{0.0f};
                float r = 0.0f;
                smvk::MeshBoundsToWorld(
                    model, baseDesc->geometry.aabbMin, baseDesc->geometry.aabbMax, c, r);
                if (!smvk::RenderViewIntersectsSphere(mainView, c, r)) {
                    bool visible = false;
                    if (lodEnabled) {
                        const uint32_t lod =
                            world->LodIndexForNode(item.node, item.mesh, lodInputs);
                        if (smvk::StaticMesh* selDesc = world->GetLodMesh(item.mesh, lod);
                            selDesc && selDesc != baseDesc) {
                            smvk::MeshBoundsToWorld(model,
                                                    selDesc->geometry.aabbMin,
                                                    selDesc->geometry.aabbMax,
                                                    c,
                                                    r);
                            visible = smvk::RenderViewIntersectsSphere(mainView, c, r);
                        }
                    }
                    if (!visible)
                        return;
                }
            }
        }
        smvk::StaticMesh* meshDesc = nullptr;
        if (lodEnabled) {
            const uint32_t lod = world->LodIndexForNode(item.node, item.mesh, lodInputs);
            meshDesc = world->GetLodMesh(item.mesh, lod);
        } else {
            meshDesc = world->GetMesh(item.mesh);
        }
        if (!meshDesc)
            return;
        if (maskedPhase) {
            smvk::CullMode cull = smvk::CullMode::None;
            if (const smvk::Material* mat =
                    static_cast<const smvk::World&>(*world).GetMaterial(item.material))
                cull = mat->pipeline.raster.cullMode;
            ensureMaskedPipeline(cull);
        } else {
            smvk::CullMode cull = smvk::CullMode::Back;
            if (const smvk::Material* mat =
                    static_cast<const smvk::World&>(*world).GetMaterial(item.material))
                cull = mat->pipeline.raster.cullMode;
            ensureSolidPipeline(cull);
        }
        // Direct-path skin tail: a skinned draw swaps bind-pose inputs for the
        // frame's deformed words. Always pushed (zeroed on non-skinned) so a
        // stale tail never leaks. Must follow the phase ensures (clobber rule).
        DepthSkinTail skinTail{};
        if (meshDesc->skinned && skinValid) {
            skinTail.skinned = 1u;
            skinTail.deformedBase = skinBase;
            skinTail.vsDeformed = skinBda;
        }
        cmd.PushConstants(
            kDepthSkinTailOffset, sizeof(DepthSkinTail), &skinTail, smvk::ShaderStage::All);
        IRenderer::PushConstants objectPC{};
        std::copy_n(glm::value_ptr(model), 16, objectPC.model);
        objectPC.padEnd_[0] = ditherFade ? 1u : 0u;
        cmd.PushConstants(offsetof(IRenderer::PushConstants, padEnd_),
                          sizeof(objectPC.padEnd_[0]),
                          &objectPC.padEnd_[0],
                          smvk::ShaderStage::All);
        if (maskedPhase || gbufferOn) {
            objectPC.materialId = item.material;
            cmd.PushConstants(offsetof(IRenderer::PushConstants, materialId),
                              sizeof(objectPC.materialId),
                              &objectPC.materialId,
                              smvk::ShaderStage::All);
        }
        cmd.PushConstants(offsetof(IRenderer::PushConstants, model),
                          sizeof(objectPC.model),
                          objectPC.model,
                          smvk::ShaderStage::All);
        for (auto& draw_set : meshDesc->geometry.surfaces) {
            cmd.DrawIndexed(draw_set.indexCount, 1, draw_set.firstIndex, draw_set.baseVertex);
        };
    };
    ensureSolidPipeline(smvk::CullMode::Back);
    world->forEachRenderItem(smvk::IteratorFlagBit_Renderable | smvk::IteratorFlagBit_Opaque |
                                 smvk::IteratorFlagBit_StaticVertexFmt,
                             [&](const smvk::World::RenderItem& item) {
                                 drawPrepassNode(item, true);
                             });
    world->forEachRenderItem(smvk::IteratorFlagBit_Renderable | smvk::IteratorFlagBit_Opaque |
                                 smvk::IteratorFlagBit_StaticVertexFmt,
                             [&](const smvk::World::RenderItem& item) {
                                 drawPrepassNode(item, false);
                             });
}

// Per-flight slot payload + record functions for the GPU-scene indirect
// prepass legs (regular indexed + compact meshlet). These record the device-
// authored command buffer only; the stable programs ride the frame slot
// (they are per-call shader variants, not renderer members).
struct GpuScenePrepassFrame {
    const smvk::ShaderProgram* rasterDepth = nullptr;
    const smvk::ShaderProgram* maskedDepth = nullptr;
    const smvk::ShaderProgram* maskedGbuffer = nullptr;
    bool gbufferOn = false;
    IRenderer::PushConstants scenePush{};
    smvk::BufferHandle indirect{};
    smvk::BufferHandle countBuffer{};
    uint32_t commandCount = 0;
    tour::gpu_scene::RasterVsPush vertexPush{};
    uint32_t offsets[4] = {};
    uint32_t capacities[4] = {};
    bool hasDoubleSided = false;
    bool hasMasked = false;
    bool hasMaskedDoubleSided = false;
    bool hasProjectLit = false;
};

void RecordGpuScenePrepass(smvk::CommandBuffer& cmd,
                           const smvk::FrameContext& fc,
                           const smvk::PassData& pd) {
    GpuScenePrepassFrame& f = fc.rg->FrameData().Get<GpuScenePrepassFrame>(pd.frameSlot);
    smvk::ShaderProgram& rasterDepth = *const_cast<smvk::ShaderProgram*>(f.rasterDepth);
    smvk::ShaderProgram& maskedDepth = *const_cast<smvk::ShaderProgram*>(f.maskedDepth);
    smvk::ShaderProgram& maskedGbuffer = *const_cast<smvk::ShaderProgram*>(f.maskedGbuffer);
    if (!f.commandCount)
        return; // the pass still clears depth for empty views
    cmd.BindStaticVertexFmt();
    cmd.BindRenderPipeline(rasterDepth,
                           ::tour::vertex_layouts::Static,
                           smvk::DepthSettings{.depthFunc = smvk::CompareOp::GreaterEqual,
                                               .writeEnable = true,
                                               .depthTestEnable = true},
                           smvk::StencilSettings{},
                           smvk::BlendSettings{},
                           smvk::MSAASettings{},
                           smvk::RasterSettings{.cullMode = smvk::CullMode::Back});
    cmd.PushConstants(0, sizeof(f.scenePush), &f.scenePush, smvk::ShaderStage::All);
    cmd.PushConstants(tour::gpu_scene::kRasterVsPushOffset,
                      sizeof(f.vertexPush),
                      &f.vertexPush,
                      smvk::ShaderStage::All);
    constexpr uint32_t kChunk = 65535u;
    const uint32_t stride = f.commandCount / tour::gpu_scene::kRasterPipelineClassCount;
    const auto drawBin = [&](uint32_t bin) {
        const uint32_t base = bin * stride;
        for (uint32_t first = 0; first < stride; first += kChunk) {
            const uint32_t n = std::min(kChunk, stride - first);
            cmd.DrawIndexedIndirect(f.indirect,
                                    uint64_t(base + first) *
                                        sizeof(tour::gpu_scene::RasterIndirectCommand),
                                    n,
                                    sizeof(tour::gpu_scene::RasterIndirectCommand));
        }
    };
    drawBin(0u);
    if (f.hasProjectLit)
        drawBin(4u);
    if (f.hasDoubleSided) {
        cmd.BindRenderPipeline(rasterDepth,
                               ::tour::vertex_layouts::Static,
                               smvk::DepthSettings{.depthFunc = smvk::CompareOp::GreaterEqual,
                                                   .writeEnable = true,
                                                   .depthTestEnable = true},
                               smvk::StencilSettings{},
                               smvk::BlendSettings{},
                               smvk::MSAASettings{},
                               smvk::RasterSettings{.cullMode = smvk::CullMode::None});
        drawBin(1u);
        if (f.hasProjectLit)
            drawBin(5u);
    }
    smvk::ShaderProgram maskedProgram = f.gbufferOn ? maskedGbuffer : maskedDepth;
    if (f.hasMasked) {
        cmd.BindRenderPipeline(maskedProgram,
                               ::tour::vertex_layouts::Static,
                               smvk::DepthSettings{.depthFunc = smvk::CompareOp::GreaterEqual,
                                                   .writeEnable = true,
                                                   .depthTestEnable = true},
                               smvk::StencilSettings{},
                               smvk::BlendSettings{},
                               smvk::MSAASettings{},
                               smvk::RasterSettings{.cullMode = smvk::CullMode::Back});
        drawBin(2u);
        if (f.hasProjectLit)
            drawBin(6u);
    }
    if (f.hasMaskedDoubleSided) {
        cmd.BindRenderPipeline(maskedProgram,
                               ::tour::vertex_layouts::Static,
                               smvk::DepthSettings{.depthFunc = smvk::CompareOp::GreaterEqual,
                                                   .writeEnable = true,
                                                   .depthTestEnable = true},
                               smvk::StencilSettings{},
                               smvk::BlendSettings{},
                               smvk::MSAASettings{},
                               smvk::RasterSettings{.cullMode = smvk::CullMode::None});
        drawBin(3u);
        if (f.hasProjectLit)
            drawBin(7u);
    }
}

void RecordGpuSceneCompactPrepass(smvk::CommandBuffer& cmd,
                                  const smvk::FrameContext& fc,
                                  const smvk::PassData& pd) {
    GpuScenePrepassFrame& f = fc.rg->FrameData().Get<GpuScenePrepassFrame>(pd.frameSlot);
    smvk::ShaderProgram& rasterDepth = *const_cast<smvk::ShaderProgram*>(f.rasterDepth);
    smvk::ShaderProgram& maskedDepth = *const_cast<smvk::ShaderProgram*>(f.maskedDepth);
    smvk::ShaderProgram& maskedGbuffer = *const_cast<smvk::ShaderProgram*>(f.maskedGbuffer);
    cmd.BindStaticVertexFmt();
    cmd.BindRenderPipeline(rasterDepth,
                           ::tour::vertex_layouts::Static,
                           smvk::DepthSettings{.depthFunc = smvk::CompareOp::GreaterEqual,
                                               .writeEnable = true,
                                               .depthTestEnable = true},
                           smvk::StencilSettings{},
                           smvk::BlendSettings{},
                           smvk::MSAASettings{},
                           smvk::RasterSettings{.cullMode = smvk::CullMode::Back});
    cmd.PushConstants(0, sizeof(f.scenePush), &f.scenePush, smvk::ShaderStage::All);
    cmd.PushConstants(tour::gpu_scene::kRasterVsPushOffset,
                      sizeof(f.vertexPush),
                      &f.vertexPush,
                      smvk::ShaderStage::All);
    const auto drawBin = [&](uint32_t bin) {
        cmd.DrawIndexedIndirectCount(f.indirect,
                                     uint64_t(f.offsets[bin]) *
                                         sizeof(tour::gpu_scene::RasterIndirectCommand),
                                     f.countBuffer,
                                     uint64_t(bin) * sizeof(uint32_t),
                                     f.capacities[bin],
                                     sizeof(tour::gpu_scene::RasterIndirectCommand));
    };
    drawBin(0u);
    if (f.hasDoubleSided) {
        cmd.BindRenderPipeline(rasterDepth,
                               ::tour::vertex_layouts::Static,
                               smvk::DepthSettings{.depthFunc = smvk::CompareOp::GreaterEqual,
                                                   .writeEnable = true,
                                                   .depthTestEnable = true},
                               smvk::StencilSettings{},
                               smvk::BlendSettings{},
                               smvk::MSAASettings{},
                               smvk::RasterSettings{.cullMode = smvk::CullMode::None});
        drawBin(1u);
    }
    smvk::ShaderProgram maskedProgram = f.gbufferOn ? maskedGbuffer : maskedDepth;
    if (f.hasMasked) {
        cmd.BindRenderPipeline(maskedProgram,
                               ::tour::vertex_layouts::Static,
                               smvk::DepthSettings{.depthFunc = smvk::CompareOp::GreaterEqual,
                                                   .writeEnable = true,
                                                   .depthTestEnable = true},
                               smvk::StencilSettings{},
                               smvk::BlendSettings{},
                               smvk::MSAASettings{},
                               smvk::RasterSettings{.cullMode = smvk::CullMode::Back});
        drawBin(2u);
    }
    if (f.hasMaskedDoubleSided) {
        cmd.BindRenderPipeline(maskedProgram,
                               ::tour::vertex_layouts::Static,
                               smvk::DepthSettings{.depthFunc = smvk::CompareOp::GreaterEqual,
                                                   .writeEnable = true,
                                                   .depthTestEnable = true},
                               smvk::StencilSettings{},
                               smvk::BlendSettings{},
                               smvk::MSAASettings{},
                               smvk::RasterSettings{.cullMode = smvk::CullMode::None});
        drawBin(3u);
    }
}

// Per-flight slot payload + record function for the GPU-scene task+mesh
// prepass (indirect mesh-task dispatch). Programs are const-ref'd (they are
// caller-owned stable members), so the frame stores pointers.
struct GpuSceneTaskFrame {
    const smvk::ShaderProgram* taskDepth = nullptr;
    const smvk::ShaderProgram* taskGbuffer = nullptr;
    const smvk::ShaderProgram* maskedDepth = nullptr;
    const smvk::ShaderProgram* maskedGbuffer = nullptr;
    bool gbufferOn = false;
    IRenderer::PushConstants scenePush{};
    smvk::BufferHandle indirect{};
    tour::gpu_scene::TaskMeshGpuPush tail{};
    bool hasDoubleSided = false;
    bool hasMasked = false;
    bool hasMaskedDoubleSided = false;
};

void RecordGpuSceneTask(smvk::CommandBuffer& cmd,
                        const smvk::FrameContext& fc,
                        const smvk::PassData& pd) {
    GpuSceneTaskFrame& f = fc.rg->FrameData().Get<GpuSceneTaskFrame>(pd.frameSlot);
    smvk::ShaderProgram& taskDepth = *const_cast<smvk::ShaderProgram*>(f.taskDepth);
    smvk::ShaderProgram& taskGbuffer = *const_cast<smvk::ShaderProgram*>(f.taskGbuffer);
    smvk::ShaderProgram& maskedDepth = *const_cast<smvk::ShaderProgram*>(f.maskedDepth);
    smvk::ShaderProgram& maskedGbuffer = *const_cast<smvk::ShaderProgram*>(f.maskedGbuffer);
    const auto depth = smvk::DepthSettings{.depthFunc = smvk::CompareOp::GreaterEqual,
                                           .writeEnable = true,
                                           .depthTestEnable = true};
    const auto drawBin = [&](uint32_t cls, smvk::ShaderProgram& program, smvk::CullMode cull) {
        cmd.BindRenderPipeline(program,
                               ::tour::vertex_layouts::Empty,
                               depth,
                               smvk::StencilSettings{},
                               smvk::BlendSettings{},
                               smvk::MSAASettings{},
                               smvk::RasterSettings{.cullMode = cull});
        cmd.PushConstants(0, sizeof(f.scenePush), &f.scenePush, smvk::ShaderStage::All);
        auto push = f.tail;
        push.rasterClass = cls;
        cmd.PushConstants(192u, sizeof(push), &push, smvk::ShaderStage::All);
        cmd.DrawMeshTasksIndirect(f.indirect,
                                  uint64_t(cls) *
                                      sizeof(tour::gpu_scene::TaskMeshIndirectCommand),
                                  1u,
                                  sizeof(tour::gpu_scene::TaskMeshIndirectCommand));
    };
    auto unmasked = f.gbufferOn ? taskGbuffer : taskDepth;
    auto masked = f.gbufferOn ? maskedGbuffer : maskedDepth;
    drawBin(0u, unmasked, smvk::CullMode::Back);
    if (f.hasDoubleSided)
        drawBin(1u, unmasked, smvk::CullMode::None);
    if (f.hasMasked)
        drawBin(2u, masked, smvk::CullMode::Back);
    if (f.hasMaskedDoubleSided)
        drawBin(3u, masked, smvk::CullMode::None);
}

// Per-flight slot payload + non-capturing record function for the Hi-Z
// occluder prepass (static compiled ABI). `config` points to the
// ForwardPlusRenderer (occluderMinPixels/Radius + fp builder are stable).
struct OccluderFrame {
    std::span<const uint8_t> staticMask;
    smvk::RenderView mainView{};
    bool hasMainView = false;
    float viewportHeight = 0.0f;
    float tanHalfFov = 0.0f;
};

void RecordOccluder(smvk::CommandBuffer& cmd,
                    const smvk::FrameContext& base,
                    const smvk::PassData& pd) {
    const AppFrameContext& fc = static_cast<const AppFrameContext&>(base);
    auto* self =
        const_cast<ForwardPlusRenderer*>(static_cast<const ForwardPlusRenderer*>(pd.config));
    const OccluderFrame& f = fc.rg->FrameData().Get<OccluderFrame>(pd.frameSlot);

    const IRenderer::PushConstants& depthPC = fc.pc;
    smvk::World* const world = fc.world;
    const float minPixels = self->occluderMinPixels;
    const float minRadius = self->occluderMinRadius;
    const glm::vec3& eye = fc.eye;
    const bool lodEnabled = fc.lodEnabled;
    const smvk::LodFrameInputs& lodInputs = fc.lodInputs;
    const float viewportHeight = f.viewportHeight;
    const float tanHalfFov = f.tanHalfFov;
    const uint64_t skinBda = fc.skinDeformedBda;
    const uint32_t skinBase = fc.skinDeformedBase;
    const bool skinValid = fc.skinDeformedValid;
    const std::span<const uint8_t>& staticMask = f.staticMask;
    const smvk::RenderView& mainView = f.mainView;
    const bool hasMainView = f.hasMainView;

    auto drawOccluderNode = [&](const smvk::World::RenderItem& item) {
        // Solid opaque only: skip MASK cutout (holes) entirely.
        if ((item.flags & smvk::IteratorFlagBit_Masked) != 0)
            return;
        if (!staticMask.empty() && item.node < staticMask.size() && staticMask[item.node] == 0)
            return;
        if (!world || item.node >= world->scene.globalTransform.size())
            return;
        auto& model = world->scene.globalTransform[item.node];
        smvk::StaticMesh* baseDesc = world->GetMesh(item.mesh);
        if (!baseDesc)
            return;
        glm::vec3 c{0.0f};
        float r = 0.0f;
        smvk::MeshBoundsToWorld(
            model, baseDesc->geometry.aabbMin, baseDesc->geometry.aabbMax, c, r);
        if (!(r >= minRadius))
            return;
        if (hasMainView) {
            if (!smvk::RenderViewIntersectsSphere(mainView, c, r))
                return;
            const float dist = glm::length(c - eye);
            if (dist > 1e-3f && tanHalfFov > 1e-6f) {
                const float diamPx = (2.0f * r / dist) * (viewportHeight / (2.0f * tanHalfFov));
                if (!(diamPx >= minPixels))
                    return;
            }
        }
        smvk::StaticMesh* meshDesc = baseDesc;
        if (lodEnabled) {
            const uint32_t lod = world->LodIndexForNode(item.node, item.mesh, lodInputs);
            if (smvk::StaticMesh* sel = world->GetLodMesh(item.mesh, lod))
                meshDesc = sel;
        }
        if (!meshDesc)
            return;
        DepthSkinTail skinTail{};
        if (meshDesc->skinned && skinValid) {
            skinTail.skinned = 1u;
            skinTail.deformedBase = skinBase;
            skinTail.vsDeformed = skinBda;
        }
        cmd.PushConstants(
            kDepthSkinTailOffset, sizeof(DepthSkinTail), &skinTail, smvk::ShaderStage::All);
        IRenderer::PushConstants objectPC{};
        std::copy_n(glm::value_ptr(model), 16, objectPC.model);
        cmd.PushConstants(offsetof(IRenderer::PushConstants, model),
                          sizeof(objectPC.model),
                          objectPC.model,
                          smvk::ShaderStage::All);
        for (auto& draw_set : meshDesc->geometry.surfaces) {
            cmd.DrawIndexed(draw_set.indexCount, 1, draw_set.firstIndex, draw_set.baseVertex);
        };
    };
    cmd.BindRenderPipeline(self->fp->depthProgram,
                           ::tour::vertex_layouts::Static,
                           smvk::DepthSettings{.depthFunc = smvk::CompareOp::GreaterEqual,
                                               .writeEnable = true,
                                               .depthTestEnable = true},
                           smvk::StencilSettings{},
                           smvk::BlendSettings{},
                           smvk::MSAASettings{},
                           smvk::RasterSettings{.cullMode = smvk::CullMode::Back});
    cmd.PushConstants(0, sizeof(IRenderer::PushConstants), &depthPC, smvk::ShaderStage::All);
    world->forEachRenderItem(smvk::IteratorFlagBit_Renderable | smvk::IteratorFlagBit_Opaque |
                                 smvk::IteratorFlagBit_StaticVertexFmt,
                             [&](const smvk::World::RenderItem& item) {
                                 drawOccluderNode(item);
                             });
}

// Per-flight slot payload + non-capturing record function for the tile light
// cull (static compiled ABI). config points to the ForwardPlusRenderer
// (fp->cullProgram is the stable side).
struct TileCullFrame {
    smvk::ForwardPlusCullPush cullPC{};
    uint32_t tileCountX = 0;
    uint32_t tileCountY = 0;
};

void RecordTileCull(smvk::CommandBuffer& cmd,
                    const smvk::FrameContext& fc,
                    const smvk::PassData& pd) {
    auto* self =
        const_cast<ForwardPlusRenderer*>(static_cast<const ForwardPlusRenderer*>(pd.config));
    const TileCullFrame& f = fc.rg->FrameData().Get<TileCullFrame>(pd.frameSlot);
    cmd.BindComputePipeline(self->fp->cullProgram);
    cmd.PushConstants(
        0, sizeof(smvk::ForwardPlusCullPush), &f.cullPC, smvk::ShaderStage::All);
    cmd.Dispatch(f.tileCountX, f.tileCountY, 1);
}

} // namespace

bool ForwardPlusRenderer::Ensure(smvk::ResourceManager& ctx, smvk::RenderGraph& rg) {
    (void)ctx;
    (void)rg;
    // Sizing needs the per-frame extent + depth view; fp->Ensure is
    // resize-safe and runs at the top of AppendPasses. Shaders load there
    // too (EnsureShaders via Create/Ensure).
    return fp != nullptr;
}

void ForwardPlusRenderer::Destroy(smvk::RenderGraph& rg) {
    // Builder storage is owned by ViewportResources; nothing to free here.
    (void)rg;
}

void ForwardPlusRenderer::AppendPasses(smvk::FrameContext& base) {
    AppFrameContext& fc = static_cast<AppFrameContext&>(base);
    AppendPrepass(fc);
    AppendTileCull(fc);
}

void ForwardPlusRenderer::AppendPrepass(smvk::FrameContext& base, bool fallbackOnly) {
    AppFrameContext& fc = static_cast<AppFrameContext&>(base);
    if (!fp)
        return;
    smvk::ResourceManager& ctx = *fc.ctx;
    smvk::RenderGraph& rg = *fc.rg;

    SMVK_PROFILE_CATEGORY("ForwardPlusDepth", Optick::Category::Rendering);

    // Resize-safe (mirrors the old frame-top Ensure in main).
    fp->Ensure(ctx, rg, {fc.extent.width, fc.extent.height}, desc, fc.depthViewHandle);
    if (!fp->Valid())
        return;

    // Temporal seed already primed depth (Clear in the seed pass): the
    // direct prepass must Load it, not Clear it. It still draws every
    // *visible* opaque node the filter's temporal set does not cover —
    // the Northlight depth split passes fallbackOnly=true so the seed
    // (prev∩curr clusters) + remainder (newly visible clusters) own
    // their filterable items' depth (one alpha-tested draw per masked
    // cluster; no full masked walk), and this walk covers the rest:
    // unregistered meshes/LOD levels, unknown-material clusters and
    // shader-LOD vertex-lit instances. That keeps the 1x Main read-only
    // path and the tile-cull min/max over complete depth. Frustum
    // culling below only removes off-screen nodes, so on-screen fallback
    // geometry is still covered.
    const bool seeded = fc.seedDepthPrimed || fc.gpuSceneDepthPrimed;
    // Depth is opaque-only (solid + MASK cutout). AlphaMode::Blend
    // transparent is excluded in both modes (dithered and blended) so it
    // never pollutes depth-driven passes (SSAO, HZB, Forward+ tile
    // min/max): glass must reveal the AO/occlusion of what is behind it,
    // not occlude like a wall, and screen-door depth would add speckle.
    // Tradeoff (accepted): transparent gets no depth prepass benefits —
    // no inter-transparent depth ordering, no HZB culling of transparent,
    // and Forward+ min/max ignores it. That matches classic forward
    // behavior (transparent tests against opaque depth, writes none) and
    // is what SSAO correctness requires.

    smvk::World* world = fc.world;
    const smvk::LogicalImageViewHandle depthView = fc.depthView;
    // SSSR-style thin G-buffer (see gbuffer.frag.hlsl): on when wanted AND
    // the programs loaded. On temporal-split frames (fallbackOnly) the
    // G-buffer Loads last frame's values — the seed pass draws the
    // filterable majority without a G-buffer FS, and static normals never
    // change (dynamic surfaces read one frame stale, invisible under the
    // temporal SSR blend); on full-prepass frames it Clears (fresh
    // coverage: the prepass draws every visible opaque surface).
    const bool gbufferOn = fc.ssrGbufferWanted && gbufferProgram.shaderCount == 2 &&
                           gbufferMaskedProgram.shaderCount == 2 && !fc.gbufferView.empty();
    const smvk::LogicalImageViewHandle gbufferView =
        gbufferOn ? fc.gbufferView : smvk::LogicalImageViewHandle{};
    const smvk::AttachmentLoadOp gbufferLoad = (fallbackOnly || fc.gpuSceneDepthPrimed)
                                                   ? smvk::AttachmentLoadOp::Load
                                                   : smvk::AttachmentLoadOp::Clear;

    // ---- CPU frustum culling for the prepass ----
    // Static nodes: single ::tour::frustumCullBVH walk over the World's cached
    // static tree (built once in ResolveAndFixup_pages from frozen
    // static-subtree globals, LOD-union bounds). Dynamic nodes: per-item
    // sphere test below (they never enter the static tree).
    // Both tests are conservative (guarded main-view planes + LOD-union
    // spheres), so a culled node is guaranteed off-screen: skipping it
    // cannot punch holes in depth. When no view/BVH is available the mask
    // stays empty and the prepass falls back to drawing everything.
    smvk::RenderView mainView{};
    bool hasMainView = false;
    if (fc.views && fc.mainViewIndex != smvk::RenderViewSet::kInvalidIndex &&
        fc.mainViewIndex < fc.views->Count()) {
        mainView = fc.views->Get(fc.mainViewIndex);
        hasMainView = true;
    }
    std::shared_ptr<std::vector<uint8_t>> staticMask;
    if (hasMainView && world) {
        auto mask = world->StaticVisibilityMask(mainView.m_viewPlanes, 6);
        if (!mask.empty())
            staticMask = std::make_shared<std::vector<uint8_t>>(std::move(mask));
    }

    using NodeId = ::NodeId;

    // ---- Depth prepass: fills depth for tile min/max cull ----
    // Optional color: the SSSR thin G-buffer (oct normal + roughness +
    // metal, RGBA8 — gbuffer.frag.hlsl). Solid geometry uses the VS-only
    // fast path (or the G-buffer FS pair when on); MASK cutout uses the
    // alpha-tested program so holes keep background depth (the G-buffer
    // masked variant discards holes out of the G-buffer the same way).
    // Transparent (Blend) is intentionally skipped — see above.
    {
        auto& prepass = fp->AddDepthPrepass(
            rg,
            depthView,
            {fc.extent.width, fc.extent.height},
            "ForwardPlusDepth",
            seeded ? smvk::AttachmentLoadOp::Load : smvk::AttachmentLoadOp::Clear,
            gbufferView,
            gbufferLoad,
            gbufferOn ? fc.gbuffer2View : smvk::LogicalImageViewHandle{},
            gbufferLoad);
        PrepassFrame& frame = rg.PassFrameData<PrepassFrame>(prepass);
        frame.staticMask = staticMask ? rg.FrameData().CopyArray<uint8_t>(*staticMask)
                                      : std::span<const uint8_t>{};
        frame.mainView = mainView;
        frame.hasMainView = hasMainView;
        frame.fallbackOnly = fallbackOnly;
        frame.gbufferOn = gbufferOn;
        prepass.SetRecordFn(&RecordPrepass, this);
    }
}

bool ForwardPlusRenderer::AppendGpuScenePrepass(smvk::FrameContext& base,
                                                const smvk::ShaderProgram& rasterDepth,
                                                smvk::BufferHandle indirect,
                                                smvk::LogicalBufferHandle indirectLogical,
                                                smvk::LogicalBufferHandle workLogical,
                                                uint32_t commandCount,
                                                const tour::gpu_scene::RasterVsPush& vertexPush,
                                                bool hizOccluder,
                                                bool hasDoubleSided,
                                                const smvk::ShaderProgram& maskedDepth,
                                                const smvk::ShaderProgram& maskedGbuffer,
                                                bool hasMasked,
                                                bool hasMaskedDoubleSided,
                                                bool hasProjectLit) {
    AppFrameContext& fc = static_cast<AppFrameContext&>(base);
    if (!fp || (rasterDepth.shaderCount != 1 && rasterDepth.shaderCount != 2))
        return false;
    // A temporal seed must be VS-only and must NEVER write the Main
    // depth/G-buffer attachments: its pyramid represents the proven
    // previous-visible subset, not the complete set of current draws.
    if (hizOccluder && (rasterDepth.shaderCount != 1 || fc.hizDepthView.empty()))
        return false;
    const bool gbufferOn = !hizOccluder && fc.ssrGbufferWanted && rasterDepth.shaderCount == 2 &&
                           !fc.gbufferView.empty() && !fc.gbuffer2View.empty();
    // A requested G-buffer must not silently render depth alone.
    if (!hizOccluder && fc.ssrGbufferWanted && !gbufferOn)
        return false;
    // Masked depth always needs a fragment shader, even on the dedicated
    // Hi-Z seed target. Every color/depth path must agree on alpha holes.
    if ((hasMasked || hasMaskedDoubleSided) &&
        (maskedDepth.shaderCount != 2 || (gbufferOn && maskedGbuffer.shaderCount != 2)))
        return false;
    smvk::ResourceManager& ctx = *fc.ctx;
    smvk::RenderGraph& rg = *fc.rg;
    fp->Ensure(ctx, rg, {fc.extent.width, fc.extent.height}, desc, fc.depthViewHandle);
    if (!fp->Valid())
        return false;
    auto& pass = fp->AddDepthPrepass(
        rg,
        hizOccluder ? fc.hizDepthView : fc.depthView,
        {fc.extent.width, fc.extent.height},
        hizOccluder ? "GpuSceneHizSeed" : "GpuSceneDepth",
        hizOccluder
            ? smvk::AttachmentLoadOp::Clear
            : ((fc.seedDepthPrimed || fc.gpuSceneDepthPrimed) ? smvk::AttachmentLoadOp::Load
                                                              : smvk::AttachmentLoadOp::Clear),
        gbufferOn ? fc.gbufferView : smvk::LogicalImageViewHandle{},
        fc.gpuSceneDepthPrimed ? smvk::AttachmentLoadOp::Load : smvk::AttachmentLoadOp::Clear,
        gbufferOn ? fc.gbuffer2View : smvk::LogicalImageViewHandle{},
        fc.gpuSceneDepthPrimed ? smvk::AttachmentLoadOp::Load : smvk::AttachmentLoadOp::Clear);
    if (commandCount > 0) {
        smvk::BufferSyncRequest indirectRead{};
        indirectRead.barrierMasks.accessMask = smvk::AccessFlags::IndirectCommandRead;
        indirectRead.barrierMasks.stageMask = smvk::PipelineStageBit_DrawIndirect;
        pass.AddBufferHint(indirectLogical, indirectRead);
        smvk::BufferSyncRequest workRead{};
        workRead.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
        workRead.barrierMasks.stageMask = smvk::PipelineStageBit_Vertex;
        pass.AddBufferHint(workLogical, workRead);
    }
    const IRenderer::PushConstants scenePush = fc.pc;
    GpuScenePrepassFrame& frame = rg.PassFrameData<GpuScenePrepassFrame>(pass);
    frame.rasterDepth = &rasterDepth;
    frame.maskedDepth = &maskedDepth;
    frame.maskedGbuffer = &maskedGbuffer;
    frame.gbufferOn = gbufferOn;
    frame.scenePush = scenePush;
    frame.indirect = indirect;
    frame.commandCount = commandCount;
    frame.vertexPush = vertexPush;
    frame.hasDoubleSided = hasDoubleSided;
    frame.hasMasked = hasMasked;
    frame.hasMaskedDoubleSided = hasMaskedDoubleSided;
    frame.hasProjectLit = hasProjectLit;
    pass.SetRecordFn(&RecordGpuScenePrepass);
    return true;
}

bool ForwardPlusRenderer::AppendGpuSceneCompactPrepass(
    smvk::FrameContext& base,
    const smvk::ShaderProgram& rasterDepth,
    smvk::BufferHandle indirect,
    smvk::LogicalBufferHandle indirectLogical,
    smvk::LogicalBufferHandle countLogical,
    smvk::BufferHandle countBuffer,
    smvk::LogicalBufferHandle workLogical,
    const uint32_t binOffsets[4],
    const uint32_t binCapacities[4],
    uint32_t commandCount,
    const tour::gpu_scene::RasterVsPush& vertexPush,
    bool hasDoubleSided,
    const smvk::ShaderProgram& maskedDepth,
    const smvk::ShaderProgram& maskedGbuffer,
    bool hasMasked,
    bool hasMaskedDoubleSided,
    bool hizOccluder) {
    AppFrameContext& fc = static_cast<AppFrameContext&>(base);
    if (!fp || (rasterDepth.shaderCount != 1 && rasterDepth.shaderCount != 2))
        return false;
    // The occluder-fill lane writes ONLY depth (no G-buffer) into the Hi-Z
    // source target; it needs the depth-only program + the hizDepth view.
    if (hizOccluder && (rasterDepth.shaderCount != 1 || fc.hizDepthView.empty()))
        return false;
    // The compact twin otherwise serves the Main depth/G-buffer lane; the
    // temporal Hi-Z seed lane never runs compact (v1 cutover).
    const bool gbufferOn = !hizOccluder && fc.ssrGbufferWanted && rasterDepth.shaderCount == 2 &&
                           !fc.gbufferView.empty() && !fc.gbuffer2View.empty();
    if (!hizOccluder && fc.ssrGbufferWanted && !gbufferOn)
        return false;
    if ((hasMasked || hasMaskedDoubleSided) &&
        (maskedDepth.shaderCount != 2 || (gbufferOn && maskedGbuffer.shaderCount != 2)))
        return false;
    if (commandCount == 0 || countBuffer.empty() || countLogical.empty())
        return false;
    smvk::ResourceManager& ctx = *fc.ctx;
    smvk::RenderGraph& rg = *fc.rg;
    fp->Ensure(ctx, rg, {fc.extent.width, fc.extent.height}, desc, fc.depthViewHandle);
    if (!fp->Valid())
        return false;
    auto& pass = fp->AddDepthPrepass(
        rg,
        hizOccluder ? fc.hizDepthView : fc.depthView,
        {fc.extent.width, fc.extent.height},
        hizOccluder ? "GpuSceneMeshletHizSeed" : "GpuSceneDepth",
        hizOccluder ? smvk::AttachmentLoadOp::Clear
                    : (fc.gpuSceneDepthPrimed ? smvk::AttachmentLoadOp::Load
                                              : smvk::AttachmentLoadOp::Clear),
        gbufferOn ? fc.gbufferView : smvk::LogicalImageViewHandle{},
        fc.gpuSceneDepthPrimed ? smvk::AttachmentLoadOp::Load : smvk::AttachmentLoadOp::Clear,
        gbufferOn ? fc.gbuffer2View : smvk::LogicalImageViewHandle{},
        fc.gpuSceneDepthPrimed ? smvk::AttachmentLoadOp::Load : smvk::AttachmentLoadOp::Clear);
    {
        smvk::BufferSyncRequest indirectRead{};
        indirectRead.barrierMasks.accessMask = smvk::AccessFlags::IndirectCommandRead;
        indirectRead.barrierMasks.stageMask = smvk::PipelineStageBit_DrawIndirect;
        pass.AddBufferHint(indirectLogical, indirectRead);
        // The draw COUNT is device-read too: the classCounts word at
        // bin*4 feeds vkCmdDrawIndexedIndirectCount's countBuffer.
        pass.AddBufferHint(countLogical, indirectRead);
        smvk::BufferSyncRequest workRead{};
        workRead.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
        workRead.barrierMasks.stageMask = smvk::PipelineStageBit_Vertex;
        pass.AddBufferHint(workLogical, workRead);
    }
    const IRenderer::PushConstants scenePush = fc.pc;
    const uint32_t offsets[4] = {binOffsets[0], binOffsets[1], binOffsets[2], binOffsets[3]};
    const uint32_t capacities[4] = {
        binCapacities[0], binCapacities[1], binCapacities[2], binCapacities[3]};
    GpuScenePrepassFrame& frame = rg.PassFrameData<GpuScenePrepassFrame>(pass);
    frame.rasterDepth = &rasterDepth;
    frame.maskedDepth = &maskedDepth;
    frame.maskedGbuffer = &maskedGbuffer;
    frame.gbufferOn = gbufferOn;
    frame.scenePush = scenePush;
    frame.indirect = indirect;
    frame.countBuffer = countBuffer;
    frame.vertexPush = vertexPush;
    for (uint32_t i = 0; i < 4; ++i) {
        frame.offsets[i] = offsets[i];
        frame.capacities[i] = capacities[i];
    }
    frame.hasDoubleSided = hasDoubleSided;
    frame.hasMasked = hasMasked;
    frame.hasMaskedDoubleSided = hasMaskedDoubleSided;
    pass.SetRecordFn(&RecordGpuSceneCompactPrepass);
    return true;
}

bool ForwardPlusRenderer::AppendGpuSceneTaskPrepass(smvk::FrameContext& base,
                                                    const smvk::ShaderProgram& taskDepth,
                                                    const smvk::ShaderProgram& taskGbuffer,
                                                    const smvk::ShaderProgram& maskedDepth,
                                                    const smvk::ShaderProgram& maskedGbuffer,
                                                    smvk::BufferHandle indirect,
                                                    smvk::LogicalBufferHandle indirectLogical,
                                                    smvk::LogicalBufferHandle groupsLogical,
                                                    smvk::LogicalBufferHandle offsetsLogical,
                                                    smvk::LogicalBufferHandle workLogical,
                                                    const tour::gpu_scene::TaskMeshGpuPush& tail,
                                                    bool hasDoubleSided,
                                                    bool hasMasked,
                                                    bool hasMaskedDoubleSided,
                                                    bool hizOccluder,
                                                    smvk::LogicalBufferHandle historyCurrent,
                                                    smvk::LogicalBufferHandle historyPrevious) {
    SMVK_PROFILE_CATEGORY("GpuSceneTaskMesh", Optick::Category::Rendering);
    AppFrameContext& fc = static_cast<AppFrameContext&>(base);
    if (!fp || taskDepth.shaderCount != 2 || indirect.empty() || indirectLogical.empty() ||
        groupsLogical.empty() || offsetsLogical.empty() || workLogical.empty())
        return false;
    const bool gbufferOn = !hizOccluder && fc.ssrGbufferWanted;
    if (hizOccluder && fc.hizDepthView.empty())
        return false;
    if (gbufferOn &&
        (taskGbuffer.shaderCount != 3 || fc.gbufferView.empty() || fc.gbuffer2View.empty()))
        return false;
    if ((hasMasked || hasMaskedDoubleSided) &&
        (maskedDepth.shaderCount != 3 || (gbufferOn && maskedGbuffer.shaderCount != 3)))
        return false;
    smvk::ResourceManager& ctx = *fc.ctx;
    smvk::RenderGraph& rg = *fc.rg;
    fp->Ensure(ctx, rg, {fc.extent.width, fc.extent.height}, desc, fc.depthViewHandle);
    if (!fp->Valid())
        return false;
    const auto depthTarget = hizOccluder ? fc.hizDepthView : fc.depthView;
    const auto load = !hizOccluder && fc.gpuSceneDepthPrimed ? smvk::AttachmentLoadOp::Load
                                                             : smvk::AttachmentLoadOp::Clear;
    auto& pass =
        fp->AddDepthPrepass(rg,
                            depthTarget,
                            {fc.extent.width, fc.extent.height},
                            hizOccluder ? "GpuSceneTaskTemporalSeed" : "GpuSceneTaskMeshDepth",
                            load,
                            gbufferOn ? fc.gbufferView : smvk::LogicalImageViewHandle{},
                            load,
                            gbufferOn ? fc.gbuffer2View : smvk::LogicalImageViewHandle{},
                            load);
    smvk::BufferSyncRequest indirectRead{};
    indirectRead.barrierMasks.stageMask = smvk::PipelineStageBit_DrawIndirect;
    indirectRead.barrierMasks.accessMask = smvk::AccessFlags::IndirectCommandRead;
    pass.AddBufferHint(indirectLogical, indirectRead);
    smvk::BufferSyncRequest shaderRead{};
    shaderRead.barrierMasks.stageMask = smvk::PipelineStageBit_Task | smvk::PipelineStageBit_Mesh;
    shaderRead.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
    pass.AddBufferHint(groupsLogical, shaderRead);
    pass.AddBufferHint(offsetsLogical, shaderRead);
    pass.AddBufferHint(workLogical, shaderRead);
    if (!historyPrevious.empty())
        pass.AddBufferHint(historyPrevious, shaderRead);
    if (!historyCurrent.empty()) {
        auto visibility = shaderRead;
        visibility.barrierMasks.accessMask =
            smvk::AccessFlags::ShaderRead | smvk::AccessFlags::ShaderWrite;
        pass.AddBufferHint(historyCurrent, visibility);
    }
    const IRenderer::PushConstants scenePush = fc.pc;
    GpuSceneTaskFrame& frame = rg.PassFrameData<GpuSceneTaskFrame>(pass);
    frame.taskDepth = &taskDepth;
    frame.taskGbuffer = &taskGbuffer;
    frame.maskedDepth = &maskedDepth;
    frame.maskedGbuffer = &maskedGbuffer;
    frame.gbufferOn = gbufferOn;
    frame.scenePush = scenePush;
    frame.indirect = indirect;
    frame.tail = tail;
    frame.hasDoubleSided = hasDoubleSided;
    frame.hasMasked = hasMasked;
    frame.hasMaskedDoubleSided = hasMaskedDoubleSided;
    pass.SetRecordFn(&RecordGpuSceneTask);
    return true;
}

void ForwardPlusRenderer::AppendOccluderPrepass(smvk::FrameContext& base) {
    AppFrameContext& fc = static_cast<AppFrameContext&>(base);
    if (!fp)
        return;
    smvk::ResourceManager& ctx = *fc.ctx;
    smvk::RenderGraph& rg = *fc.rg;

    SMVK_PROFILE_CATEGORY("OccluderDepth", Optick::Category::Rendering);

    if (fc.hizDepthView.empty() || fc.hizDepthViewHandle.empty())
        return; // legacy single-depth path: HZB reads the full depth
    fp->Ensure(ctx, rg, {fc.extent.width, fc.extent.height}, desc, fc.depthViewHandle);
    if (!fp->Valid())
        return;

    // Snapshots for the recording lambda (recorded now, executed later at
    // Execute(); nothing here may reference the frame-local fc).
    smvk::World* world = fc.world;
    const float viewportHeight =
        fc.lodInputs.viewportHeight > 0.0f ? fc.lodInputs.viewportHeight : (float)fc.extent.height;
    const float tanHalfFov = std::tan(std::max(fc.lodInputs.fovY, 1e-3f) * 0.5f);

    // CPU frustum culling mirrors the full prepass (conservative static
    // mask + dynamic LOD-union sphere test): skipping a true occluder
    // only loses culling, but the mask walk is shared work anyway.
    smvk::RenderView mainView{};
    bool hasMainView = false;
    if (fc.views && fc.mainViewIndex != smvk::RenderViewSet::kInvalidIndex &&
        fc.mainViewIndex < fc.views->Count()) {
        mainView = fc.views->Get(fc.mainViewIndex);
        hasMainView = true;
    }
    std::shared_ptr<std::vector<uint8_t>> staticMask;
    if (hasMainView && world) {
        auto mask = world->StaticVisibilityMask(mainView.m_viewPlanes, 6);
        if (!mask.empty())
            staticMask = std::make_shared<std::vector<uint8_t>>(std::move(mask));
    }

    using NodeId = ::NodeId;

    // ---- Occluder depth: big solid occluders at crudest LOD ----
    // Own target (Clear): independent of the seed/full-depth state.
    // No masked phase: cutout foliage has holes (poor occluders) and
    // needs the FS; the full prepass still depths it for Main/SSAO.
    {
        auto& occluder = fp->AddDepthPrepass(rg,
                                             fc.hizDepthView,
                                             {fc.extent.width, fc.extent.height},
                                             "OccluderDepth",
                                             smvk::AttachmentLoadOp::Clear);
        OccluderFrame& frame = rg.PassFrameData<OccluderFrame>(occluder);
        frame.staticMask = staticMask ? rg.FrameData().CopyArray<uint8_t>(*staticMask)
                                      : std::span<const uint8_t>{};
        frame.mainView = mainView;
        frame.hasMainView = hasMainView;
        frame.viewportHeight = viewportHeight;
        frame.tanHalfFov = tanHalfFov;
        occluder.SetRecordFn(&RecordOccluder, this);
    }
}

void ForwardPlusRenderer::AppendTileCull(smvk::FrameContext& base,
                                         uint32_t lightCountOverride,
                                         float fogMaxDistance,
                                         bool fogListsWanted) {
    AppFrameContext& fc = static_cast<AppFrameContext&>(base);
    if (!fp)
        return;
    smvk::ResourceManager& ctx = *fc.ctx;
    smvk::RenderGraph& rg = *fc.rg;

    SMVK_PROFILE_CATEGORY("ForwardPlusCull", Optick::Category::Rendering);
    SMVK_PROFILE_TAG("tileCountX", fp->tileCountX);
    SMVK_PROFILE_TAG("tileCountY", fp->tileCountY);
    SMVK_PROFILE_TAG("lightCount", fc.fpLightCount);

    fp->Ensure(ctx, rg, {fc.extent.width, fc.extent.height}, desc, fc.depthViewHandle);
    if (!fp->Valid())
        return;

    // ---- Tile cull: depth + light volumes -> per-tile lists ----
    // Runs after depth is complete (prepass, or seed + pass 2 when the
    // temporal pipeline primed depth) so no light is wrongly culled.
    {
        smvk::ForwardPlusCullPush cullPC{};
        cullPC.frameUniforms = fc.pc.frameUniforms;
        cullPC.lights = fc.pc.fpLights;
        cullPC.tileCounts = fp->TileCountsAddress(ctx);
        cullPC.tileIndices = fp->TileIndicesAddress(ctx);
        cullPC.volumeMasks = fp->VolumeTileMasksAddress(ctx);
        cullPC.depthTextureIndex = fp->depthBindlessIndex;
        cullPC.tileCountX = fp->tileCountX;
        cullPC.tileCountY = fp->tileCountY;
        cullPC.lightCount =
            (lightCountOverride != UINT32_MAX) ? lightCountOverride : fc.fpLightCount;
        cullPC.tileSize = desc.tileSize;
        cullPC.maxLightsPerTile = desc.maxLightsPerTile;
        // Fog lists (march-range candidates — see ForwardPlusCullPush):
        // the dedicated per-tile buffers built every dispatch + the march
        // range the volumetric pass will run with. GATED on
        // fogListsWanted: the per-light fog candidacy + sorted insertion
        // per tile is real work (not free dead stores) — skip it when the
        // volumetric pass won't run this frame. The cull shader skips the
        // fog track + writeback; the volumetric pass's zero-address
        // fallback (walk [0, fpLightCount) directly) is the consumer's
        // safety net when the lists are absent.
        cullPC.fogTileCounts = fp->FogTileCountsAddress(ctx);
        cullPC.fogTileIndices = fp->FogTileIndicesAddress(ctx);
        // Match AddVolumetricPass's ray clamp ((std::max)(1.0f, ...)): a
        // sub-1 maxDistance would cut fog candidates the march can still
        // reach inside its clamped range.
        cullPC.fogMaxDistance = (std::max)(1.0f, fogMaxDistance);
        cullPC.maxFogPerTile = desc.maxFogPerTile;
        cullPC.fogListsWanted = fogListsWanted ? 1u : 0u;
        // Hierarchical phase-1 artifacts: all four fields arrive as one
        // group from the frame context. Enforce the address<->count
        // consistency HERE (the single push site) so a torn state can
        // never reach the shader: any zero address demotes the whole set
        // to the linear fallback (maskLightCount 0).
        const bool masksOk = fc.fpMaskLightCount > 0 && fc.fpZBins != 0 && fc.fpTileMasks != 0 &&
                             fc.fpMaskGroupCountX > 0;
        cullPC.zbins = masksOk ? fc.fpZBins : 0;
        cullPC.tileMasks = masksOk ? fc.fpTileMasks : 0;
        cullPC.maskLightCount = masksOk ? fc.fpMaskLightCount : 0;
        cullPC.maskGroupCountX = masksOk ? fc.fpMaskGroupCountX : 0;
        auto& cullPass = fp->AddCullPass(
            rg, fc.depthView, fc.fpLights, "ForwardPlusCull", fc.decals, fc.frameIndex);
        TileCullFrame& frame = rg.PassFrameData<TileCullFrame>(cullPass);
        frame.cullPC = cullPC;
        frame.tileCountX = fp->tileCountX;
        frame.tileCountY = fp->tileCountY;
        cullPass.SetRecordFn(&RecordTileCull, this);
    }
}

} // namespace engine
