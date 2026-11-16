// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Included inside WorldRenderer's frame construction, after GPUScene Main
// work publication and before directional shadow RenderGraph passes.
//
// Each cascade gets independent GPUScene visibility, work and draw commands.
// Main-view visibility cannot represent light-space/off-camera casters.
struct GpuCascadeSubmission {
    bool ready = false;
    smvk::BufferHandle commands{};
    tour::gpu_scene::RasterVsPush vs{};
    uint32_t capacity = 0;
    smvk::LogicalBufferHandle indirectLogical{}, workLogical{}, viewLogical{};
};
std::array<GpuCascadeSubmission, smvk::kMaxCascades> gpuCascades{};

static const bool s_gpuSceneShadows = [] {
    const char* e = std::getenv("TOURMALINE_GPU_SCENE_SHADOWS");
    return !e || std::strcmp(e, "0") != 0;
}();
if (s_gpuSceneShadows && m_gpuSceneRasterEnabled && m_gpuSceneDatabaseReady &&
    m_passes.directionalShadows && m_world &&
    cascadeViewBase != smvk::RenderViewSet::kInvalidIndex) {
    // The same shader module is the VS of both shadow PSOs.
    if (m_gpuSceneShadowDepthProgram.shaderCount != 1 ||
        m_gpuSceneShadowMaskedProgram.shaderCount != 2) {
        try {
            if (m_gpuSceneShadowVsModule.empty())
                m_gpuSceneShadowVsModule = m_context->CreateShaderModule(
                    smvk::shaders::Read("content/shaders/gpu_scene_shadow.vert.hlsl.spv"),
                    "gpuSceneShadowVS");
            if (m_gpuSceneShadowFsModule.empty())
                m_gpuSceneShadowFsModule = m_context->CreateShaderModule(
                    smvk::shaders::Read("content/shaders/gpu_scene_shadow.frag.hlsl.spv"),
                    "gpuSceneShadowMaskedFS");
            if (!m_gpuSceneShadowVsModule.empty() && !m_gpuSceneShadowFsModule.empty()) {
                smvk::Shader vs{.module = m_gpuSceneShadowVsModule,
                                .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex};
                smvk::Shader fs{.module = m_gpuSceneShadowFsModule,
                                .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment};
                m_gpuSceneShadowDepthProgram = {.shaders = {vs}, .shaderCount = 1};
                m_gpuSceneShadowMaskedProgram = {.shaders = {vs, fs}, .shaderCount = 2};
            }
        } catch (...) {
            // The full direct caster path remains authoritative when modules
            // are not compiled or cannot be loaded. Never drop ownership.
        }
    }
    bool programsReady = m_gpuSceneShadowDepthProgram.shaderCount == 1 &&
                         m_gpuSceneShadowMaskedProgram.shaderCount == 2 &&
                         m_gpuSceneCullProgram.shaderCount == 1 &&
                         m_gpuSceneRasterBuildProgram.shaderCount == 1;
    for (const auto& program : m_gpuSceneCompactPrograms)
        programsReady &= program.shaderCount == 1;
    for (const auto& program : m_gpuSceneWorkPrograms)
        programsReady &= program.shaderCount == 1;
    for (const auto& program : m_gpuSceneWorkScanPrograms)
        programsReady &= program.shaderCount == 1;

    const uint32_t flight = fb.frameIndex;
    const bool ownerReady = !m_gpuSceneRasterOwnerMaskBuffers[flight].empty() &&
                            !m_gpuSceneRasterOwnerMaskLogical[flight].empty();
    const uint64_t ownerMaskBda = ownerReady
        ? m_context->GetBufferDeviceAddress(m_gpuSceneRasterOwnerMaskBuffers[flight]) : 0u;
    const auto addresses = m_gpuSceneUploader.DeviceAddresses(*m_context, flight);
    if (programsReady && ownerMaskBda != 0 && addresses.instances != 0) {
        // Coarse ticket selection is spatial, never a World::drawItems walk.
        // FrameArena allocations remain valid until the recording callbacks
        // have copied the entire compute push data.
        const auto tickets = m_gpuSceneSync.LeafTicketsFrame(m_gpuSceneDatabase);
        const glm::dvec3 origin(m_cameraPositioner.cameraPosition_);
        // SHARED COARSE SELECTION (one pass, not four). A conservative union
        // of the cascade frusta: the CSM boxes all sit within the split far
        // of the shared origin, so a bounding box with that half-extent admits
        // every caster any cascade can reach while still rejecting the distant
        // leaves no cascade sees. The union only gates which leaves reach the
        // GPU at all — each cascade keeps its own frustum test below, so
        // off-camera casters survive the per-view cull.
        float shadowFar = m_cascadeSplitsDbg[NUM_CASCADES];
        if (!(shadowFar > 0.0f) || !std::isfinite(shadowFar))
            shadowFar = CascadeFar();
        const double cascadeRadius = double(shadowFar);
        const tour::gpu_scene::RelativeFrustum unionFrustum{
            origin,
            {glm::dvec4(1.0, 0.0, 0.0, cascadeRadius),
             glm::dvec4(-1.0, 0.0, 0.0, cascadeRadius),
             glm::dvec4(0.0, 1.0, 0.0, cascadeRadius),
             glm::dvec4(0.0, -1.0, 0.0, cascadeRadius),
             glm::dvec4(0.0, 0.0, 1.0, cascadeRadius),
             glm::dvec4(0.0, 0.0, -1.0, cascadeRadius)}};
        const auto selectStart = std::chrono::steady_clock::now();
        const auto selected = tour::gpu_scene::SelectPassedInstanceRangesFrame(
            m_gpuSceneDatabase, tickets, unionFrustum,
            tour::gpu_scene::CandidateMode::Frustum, 65536u, 1048560u);
        const double selectMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - selectStart)
                .count();
        const auto prepStart = std::chrono::steady_clock::now();
        // Capacity overflow is *never* clamped: use the proven direct
        // path instead, so shadow coverage remains complete.
        if (selected.Valid() && selected.candidateCount > 0) {
        for (uint32_t ci = 0; ci < NUM_CASCADES; ++ci) {
            const uint32_t sourceView = cascadeViewBase + ci;
            if (sourceView >= m_renderViews.Count())
                break;
            const auto packed = tour::gpu_scene::MakeGpuSceneRenderView(
                m_renderViews.Get(sourceView), sourceView, origin);
            if (!packed.valid)
                continue;
            const auto frustum = tour::gpu_scene::LegacyRelativeFrustum(
                m_renderViews.Get(sourceView), origin);
            auto& validator = m_gpuSceneCascadeValidators[ci];
            tour::gpu_scene::InstanceCullGpuPush cull{};
            tour::gpu_scene::CompactCountPush count{};
            tour::gpu_scene::CompactPrefixPush prefix{};
            tour::gpu_scene::CompactScatterPush scatter{};
            tour::gpu_scene::WorkSelectPush workSelect{};
            tour::gpu_scene::WorkPrefixPush workPrefix{};
            tour::gpu_scene::WorkScatterPush workScatter{};
            tour::gpu_scene::ParallelWorkBlockPush workBlock{};
            tour::gpu_scene::ParallelWorkGroupPush workGroups{};
            tour::gpu_scene::ParallelWorkAddPush workAdd{};
            tour::gpu_scene::RasterWorkDrawBuildPush raster{};
            smvk::BufferHandle commands{};
            const uint32_t candidates = selected.candidateCount;
            if (!validator.Prepare(*m_context, flight, m_gpuSceneDatabase, addresses,
                                   selected, frustum, cull, false))
                continue;
            cull.ownerGenerations = ownerMaskBda;
            cull.counts2.w = 1u;
            cull.counts2.z |= 2u; // shadow-caster-only cull
            if (!validator.PrepareCompaction(*m_context, flight, candidates,
                                             count, prefix, scatter) ||
                !validator.PrepareWorkExpansion(*m_context, flight, m_gpuSceneDatabase,
                                                addresses, candidates,
                                                workSelect, workPrefix, workScatter) ||
                !validator.PrepareParallelWorkPrefix(*m_context, flight,
                                                     workBlock, workGroups, workAdd) ||
                !validator.PrepareRasterCommands(
                    *m_context, flight,
                    m_gpuScenePersistentGeometry.DeviceAddress(*m_context, flight),
                    m_gpuScenePersistentGeometry.Count(flight),
                    m_gpuScenePersistentGeometry.AliasAddress(*m_context, flight),
                    m_gpuScenePersistentGeometry.AliasCount(flight),
                    raster, commands))
                continue;
            // Initially keep Main's resident-LOD policy while varying the
            // visibility frustum. A separate shadow LOD budget is future work.
            workSelect.lodEyeBudget =
                glm::vec4(m_cameraPositioner.cameraPosition_, m_lodPixelBudget);
            workSelect.lodProjection =
                glm::vec4(float(renderExtent.height),
                          glm::radians(kCameraFovYDegrees),
                          m_lodEnabled ? 1.0f : 0.0f, 0.0f);

            auto& resources = m_gpuSceneCascadeFlights[ci][flight];
            if (resources.viewBuffer.empty()) {
                resources.viewBuffer = m_context->CreateBuffer(
                    sizeof(tour::gpu_scene::RasterViewData), smvk::BufferType_Storage,
                    "GPUScene shadow raster RenderView");
                if (!resources.viewBuffer.empty())
                    resources.viewLogical = m_rendergraph->AddExternalBuffer(
                        resources.viewBuffer, "GPUScene shadow raster RenderView");
            }
            const smvk::BufferHandle work = validator.RasterWorkBuffer(flight);
            const uint64_t workBda = validator.RasterWorkAddress(*m_context, flight);
            if (resources.viewBuffer.empty() || resources.viewLogical.empty() ||
                work.empty() || workBda == 0 || commands.empty())
                continue;
            if (resources.indirectBacking != commands) {
                if (!resources.indirectLogical.empty())
                    m_rendergraph->FreeBuffer(resources.indirectLogical);
                resources.indirectLogical =
                    m_rendergraph->AddExternalBuffer(commands, "GPUScene shadow indirect");
                resources.indirectBacking = commands;
            }
            if (resources.workBacking != work) {
                if (!resources.workLogical.empty())
                    m_rendergraph->FreeBuffer(resources.workLogical);
                resources.workLogical =
                    m_rendergraph->AddExternalBuffer(work, "GPUScene shadow work");
                resources.workBacking = work;
            }
            if (resources.indirectLogical.empty() || resources.workLogical.empty())
                continue;
            m_context->UploadDataToBuffer(
                resources.viewBuffer, &packed.raster, sizeof(packed.raster),
                smvk::PipelineStageBit_Vertex, smvk::AccessFlags::ShaderRead);
            const uint64_t viewBda =
                m_context->GetBufferDeviceAddress(resources.viewBuffer);
            if (!viewBda)
                continue;
            auto& result = gpuCascades[ci];
            result.ready = true;
            result.commands = commands;
            result.capacity = raster.commandCapacity;
            result.indirectLogical = resources.indirectLogical;
            result.workLogical = resources.workLogical;
            result.viewLogical = resources.viewLogical;
            result.vs = {workBda, addresses.instances, viewBda,
                         uint32_t(m_gpuSceneDatabase.instances.Size()),
                         raster.commandCapacity};

            // All scratch is owned per flight by the candidate validator.
            // This pass must precede the first cascade render pass.
            auto& pass = m_rendergraph->AddComputePass("GPUSceneCascadeBuild");
            smvk::BufferSyncRequest write{};
            write.barrierMasks.stageMask = smvk::PipelineStageBit_Compute;
            write.barrierMasks.accessMask = smvk::AccessFlags::ShaderWrite;
            pass.AddBufferHint(result.indirectLogical, write);
            pass.AddBufferHint(result.workLogical, write);
            smvk::BufferSyncRequest ownerRead{};
            ownerRead.barrierMasks.stageMask = smvk::PipelineStageBit_Compute;
            ownerRead.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
            pass.AddBufferHint(m_gpuSceneRasterOwnerMaskLogical[flight], ownerRead);
            auto cullProgram = m_gpuSceneCullProgram;
            auto compactPrograms = m_gpuSceneCompactPrograms;
            auto workPrograms = m_gpuSceneWorkPrograms;
            auto workScanPrograms = m_gpuSceneWorkScanPrograms;
            auto rasterProgram = m_gpuSceneRasterBuildProgram;
            pass.SetRecordingFunc(
                [cullProgram, compactPrograms, workPrograms, workScanPrograms,
                 rasterProgram, cull, count, prefix, scatter, workSelect,
                 workBlock, workGroups, workAdd, workScatter, raster,
                 candidates](smvk::CommandBuffer& cmd) mutable {
                    const uint32_t groups = (candidates + 63u) / 64u;
                    cmd.BindComputePipeline(cullProgram);
                    cmd.PushConstants(0, sizeof(cull), &cull, smvk::ShaderStage::Compute);
                    cmd.Dispatch(groups, 1, 1);
                    cmd.EmitGlobalMemoryBarrier();
                    cmd.BindComputePipeline(compactPrograms[0]);
                    cmd.PushConstants(0, sizeof(count), &count, smvk::ShaderStage::Compute);
                    cmd.Dispatch(groups, 1, 1);
                    cmd.EmitGlobalMemoryBarrier();
                    cmd.BindComputePipeline(compactPrograms[1]);
                    cmd.PushConstants(0, sizeof(prefix), &prefix, smvk::ShaderStage::Compute);
                    cmd.Dispatch(1, 1, 1);
                    cmd.EmitGlobalMemoryBarrier();
                    cmd.BindComputePipeline(compactPrograms[2]);
                    cmd.PushConstants(0, sizeof(scatter), &scatter, smvk::ShaderStage::Compute);
                    cmd.Dispatch(groups, 1, 1);
                    cmd.EmitGlobalMemoryBarrier();
                    cmd.BindComputePipeline(workPrograms[0]);
                    cmd.PushConstants(0, sizeof(workSelect), &workSelect,
                                      smvk::ShaderStage::Compute);
                    cmd.Dispatch(groups, 1, 1);
                    cmd.EmitGlobalMemoryBarrier();
                    cmd.BindComputePipeline(workScanPrograms[0]);
                    cmd.PushConstants(0, sizeof(workBlock), &workBlock,
                                      smvk::ShaderStage::Compute);
                    cmd.Dispatch((candidates + 255u) / 256u, 1, 1);
                    cmd.EmitGlobalMemoryBarrier();
                    cmd.BindComputePipeline(workScanPrograms[1]);
                    cmd.PushConstants(0, sizeof(workGroups), &workGroups,
                                      smvk::ShaderStage::Compute);
                    cmd.Dispatch(1, 1, 1);
                    cmd.EmitGlobalMemoryBarrier();
                    cmd.BindComputePipeline(workScanPrograms[2]);
                    cmd.PushConstants(0, sizeof(workAdd), &workAdd,
                                      smvk::ShaderStage::Compute);
                    cmd.Dispatch((candidates + 255u) / 256u, 1, 1);
                    cmd.EmitGlobalMemoryBarrier();
                    cmd.BindComputePipeline(workPrograms[2]);
                    cmd.PushConstants(0, sizeof(workScatter), &workScatter,
                                      smvk::ShaderStage::Compute);
                    cmd.Dispatch(groups, 1, 1);
                    cmd.EmitGlobalMemoryBarrier();
                    cmd.BindComputePipeline(rasterProgram);
                    cmd.PushConstants(0, sizeof(raster), &raster,
                                      smvk::ShaderStage::Compute);
                    cmd.Dispatch((raster.commandCapacity + 127u) / 128u, 1, 1);
                });
            validator.MarkDispatched(flight);
        }
        }
        const double prepMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepStart)
                .count();
        if (m_frameCounter % 60 == 0) {
            std::fprintf(stderr,
                         "[GpuSceneShadow] coarse %.3f ms (%u candidates, radius %.1f m) | "
                         "prep %.3f ms\n",
                         selectMs, selected.candidateCount, shadowFar, prepMs);
        }
    }
}

const auto recordGpuCascade =
    [this](smvk::CommandBuffer& cmd, const IRenderer::PushConstants& scenePC,
           const GpuCascadeSubmission& sub, const smvk::MSAASettings& msaa) {
        if (!sub.ready) return;
        const smvk::DepthSettings depth{.depthFunc = smvk::CompareOp::LessEqual,
                                        .writeEnable = true, .depthTestEnable = true};
        cmd.BindStaticVertexFmt();
        const auto bind = [&](smvk::ShaderProgram& program, smvk::CullMode cull) {
            cmd.BindRenderPipeline(program, ::tour::vertex_layouts::Static, depth,
                                   smvk::StencilSettings{}, smvk::BlendSettings{},
                                   msaa, smvk::RasterSettings{.cullMode = cull});
            cmd.PushConstants(0, sizeof(scenePC), &scenePC, smvk::ShaderStage::All);
            cmd.PushConstants(tour::gpu_scene::kRasterVsPushOffset,
                              sizeof(sub.vs), &sub.vs, smvk::ShaderStage::All);
        };
        const uint32_t stride =
            sub.capacity / tour::gpu_scene::kRasterPipelineClassCount;
        const auto drawBin = [&](uint32_t bin) {
            constexpr uint32_t kChunk = 65535u;
            for (uint32_t first = 0; first < stride; first += kChunk)
                cmd.DrawIndexedIndirect(
                    sub.commands,
                    uint64_t(bin * stride + first) *
                        sizeof(tour::gpu_scene::RasterIndirectCommand),
                    std::min(kChunk, stride - first),
                    sizeof(tour::gpu_scene::RasterIndirectCommand));
        };
        // Solid casters use backfaces for acne suppression; masked casters
        // use material culling (Back or None), just like the direct path.
        bind(m_gpuSceneShadowDepthProgram, smvk::CullMode::Front);
        drawBin(0); drawBin(1); drawBin(4); drawBin(5);
        bind(m_gpuSceneShadowMaskedProgram, smvk::CullMode::Back);
        drawBin(2); drawBin(6);
        bind(m_gpuSceneShadowMaskedProgram, smvk::CullMode::None);
        drawBin(3); drawBin(7);
    };

const auto hintGpuCascade =
    [](smvk::RenderPass& pass, const GpuCascadeSubmission& sub) {
        if (!sub.ready) return;
        smvk::BufferSyncRequest indirectRead{};
        indirectRead.barrierMasks.stageMask = smvk::PipelineStageBit_DrawIndirect;
        indirectRead.barrierMasks.accessMask = smvk::AccessFlags::IndirectCommandRead;
        pass.AddBufferHint(sub.indirectLogical, indirectRead);
        smvk::BufferSyncRequest vertexRead{};
        vertexRead.barrierMasks.stageMask = smvk::PipelineStageBit_Vertex;
        vertexRead.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
        pass.AddBufferHint(sub.workLogical, vertexRead);
        pass.AddBufferHint(sub.viewLogical, vertexRead);
    };
