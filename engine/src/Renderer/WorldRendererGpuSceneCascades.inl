// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Included inside WorldRenderer's frame construction, after GPUScene Main
// work publication and before directional shadow RenderGraph passes.
//
// Each cascade gets independent GPUScene visibility, work and draw commands.
// Main-view visibility cannot represent light-space/off-camera casters.
// GpuCascadeSubmission is a nested WorldRenderer type (hoisted to the header);
// RecordGpuCascade / HintGpuCascade are WorldRenderer member/static helpers.
std::array<WorldRenderer::GpuCascadeSubmission, smvk::kMaxCascades> gpuCascades{};

static const bool s_gpuSceneShadows = [] {
    const char* e = std::getenv("TOURMALINE_GPU_SCENE_SHADOWS");
    return !e || std::strcmp(e, "0") != 0;
}();
// Swept-volume HZB occlusion for shadow casters (default OFF; =1 to enable).
// Uses the PREVIOUS frame's main-view pyramid (the current frame's is built
// after the shadow passes), so it is one frame stale — see the block below.
// Measured to cull nothing on the flat grid and bistro: the cascade frustum
// already removes casters whose shadows are occluded, so this is opt-in.
static const bool s_shadowHzb = [] {
    const char* e = std::getenv("TOURMALINE_SHADOW_HZB");
    return e && std::strcmp(e, "1") == 0;
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
        const auto ticketStart = std::chrono::steady_clock::now();
        const auto tickets = m_gpuSceneSync.LeafTicketsFrame(m_gpuSceneDatabase);
        const double ticketMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ticketStart)
                .count();
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

        // ---- Shadow-caster HZB occlusion context (built once per frame) ----
        // Projects each caster's swept shadow volume into the MAIN view and
        // tests it against the main view's reversed-Z MIN pyramid. The shadow
        // passes run before this frame's HZB build, so this reads the PREVIOUS
        // frame's pyramid (temporals[slot].hiz[1 - frameIndex]) — one frame
        // stale. A stale pyramid can only under-occlude on disocclusion (a
        // freshly-revealed receiver briefly keeps its caster), never drop a
        // still-visible shadow, so the test stays conservative.
        tour::gpu_scene::GpuSceneShadowHizView shadowHizCtx{};
        bool shadowHizReady = false;
        smvk::LogicalBufferHandle shadowHizSrcLogical{};
        if (s_shadowHzb) {
            const uint32_t hizSlot = m_gfxRenderer.LastSlot();
            if (hizSlot != UINT32_MAX &&
                hizSlot < m_gfxRenderer.gfx.temporals.size() &&
                hizSlot < m_gfxRenderer.gfx.views.size()) {
                const uint32_t prevFlight = 1u - m_gfxRenderer.gfx.views[hizSlot].frameIndex;
                const auto& prevHz = m_gfxRenderer.gfx.temporals[hizSlot].hiz[prevFlight];
                const uint64_t hizBda = prevHz.buffer.empty()
                    ? 0u
                    : m_context->GetBufferDeviceAddress(prevHz.buffer);
                const bool hizOcclusion =
                    (uint32_t(m_gfxRenderer.config.cullFlags) &
                     uint32_t(smvk::gfx::CullFlagBit_HzbOcclusion)) != 0u;
                const uint32_t mainView = m_renderViews.MainViewIndex();
                if (hizBda != 0 && prevHz.mipCount != 0u && prevHz.width != 0u &&
                    prevHz.height != 0u && hizOcclusion && m_sdsmStatsValid &&
                    mainView != smvk::RenderViewSet::kInvalidIndex) {
                    const auto packedMain = tour::gpu_scene::MakeGpuSceneRenderView(
                        m_renderViews.Get(mainView), mainView, origin);
                    if (packedMain.valid) {
                        shadowHizCtx.mainViewProj = packedMain.raster.relativeViewProjection;
                        shadowHizCtx.sunDirAndSweep =
                            glm::vec4(glm::normalize(m_sunLightDir), shadowFar);
                        shadowHizCtx.depthRange =
                            glm::vec4(default_near_clip, m_sdsmMax, 0.0f, 0.0f);
                        shadowHizCtx.hzbDesc =
                            glm::uvec4(prevHz.width, prevHz.height, prevHz.mipCount, 1u);
                        shadowHizCtx.hzb = hizBda;
                        shadowHizSrcLogical = prevHz.logical;
                        shadowHizReady = true;
                    }
                }
            }
        }
        // Always create + upload the context (128 B) so the shader never sees a
        // null pointer; hzbDesc.w = 0 disarms the test when not ready.
        if (m_gpuSceneShadowHizBuffers[flight].empty()) {
            m_gpuSceneShadowHizBuffers[flight] = m_context->CreateBuffer(
                sizeof(tour::gpu_scene::GpuSceneShadowHizView),
                smvk::BufferType_Storage, "GPUScene shadow HZB context");
            if (!m_gpuSceneShadowHizBuffers[flight].empty())
                m_gpuSceneShadowHizLogical[flight] = m_rendergraph->AddExternalBuffer(
                    m_gpuSceneShadowHizBuffers[flight], "GPUScene shadow HZB context");
        }
        uint64_t shadowHizBda = 0u;
        if (!m_gpuSceneShadowHizBuffers[flight].empty()) {
            if (!shadowHizReady)
                shadowHizCtx = {}; // disarmed (hzbDesc.w = 0)
            m_context->UploadDataToBuffer(
                m_gpuSceneShadowHizBuffers[flight], &shadowHizCtx, sizeof(shadowHizCtx),
                smvk::PipelineStageBit_Compute, smvk::AccessFlags::ShaderRead);
            shadowHizBda = m_context->GetBufferDeviceAddress(
                m_gpuSceneShadowHizBuffers[flight]);
        }
        // One-shot HZB cull readiness diagnostic (frame 30).
        {
            static bool loggedHzb = false;
            if (!loggedHzb && m_frameCounter == 30u) {
                loggedHzb = true;
                const uint32_t hzSlot = m_gfxRenderer.LastSlot();
                const bool slotOk = hzSlot != UINT32_MAX &&
                    hzSlot < m_gfxRenderer.gfx.temporals.size() &&
                    hzSlot < m_gfxRenderer.gfx.views.size();
                uint32_t pw = 0, ph = 0, pm = 0;
                if (slotOk) {
                    const uint32_t pf = 1u - m_gfxRenderer.gfx.views[hzSlot].frameIndex;
                    const auto& pz = m_gfxRenderer.gfx.temporals[hzSlot].hiz[pf];
                    pw = pz.width; ph = pz.height; pm = pz.mipCount;
                }
                const bool hizOcc = (uint32_t(m_gfxRenderer.config.cullFlags) &
                    uint32_t(smvk::gfx::CullFlagBit_HzbOcclusion)) != 0u;
                std::fprintf(stderr,
                             "[ShadowHiz] ready=%d hizBda=%llu prevHz=%ux%u mips=%u "
                             "hizOcc=%d sdsmValid=%d zMax=%.1f\n",
                             shadowHizReady ? 1 : 0,
                             (unsigned long long)shadowHizBda,
                             pw, ph, pm, hizOcc ? 1 : 0,
                             m_sdsmStatsValid ? 1 : 0,
                             shadowHizCtx.depthRange.y);
            }
        }

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
            if (shadowHizReady && shadowHizBda != 0u) {
                cull.shadowHiz = shadowHizBda;
                cull.counts2.z |= 4u; // swept-volume HZB occlusion armed
            }
            // Furthest-visible-depth shadow early-out: cull casters whose
            // shadow (extruded along the sun direction) can only land deeper
            // than the furthest visible surface. Armed only when the sun
            // travels AWAY from the camera (the shadow deepens) — otherwise
            // a deep caster's shadow can fall back onto a visible surface and
            // must be kept. Stale SDSM stats (teleport cut) disarm it.
            {
                const glm::vec3 camFwd = glm::normalize(m_cameraPositioner.getForward());
                const bool shadowDeepens = glm::dot(m_sunLightDir, camFwd) > 0.0f;
                const float furthestDepth =
                    (m_sdsmStatsValid && m_sdsmCutCooldown == 0 && shadowDeepens)
                        ? m_sdsmMax * 1.05f
                        : 0.0f;
                cull.shadowCullParams = glm::vec4(camFwd, furthestDepth);
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    std::fprintf(stderr,
                                 "[ShadowCull] sun=(%.3f,%.3f,%.3f) fwd=(%.3f,%.3f,%.3f) "
                                 "deepens=%d maxDepth=%.1f earlyOut=%.1f\n",
                                 m_sunLightDir.x, m_sunLightDir.y, m_sunLightDir.z,
                                 camFwd.x, camFwd.y, camFwd.z, shadowDeepens ? 1 : 0,
                                 m_sdsmMax, furthestDepth);
                }
            }
            if (!validator.PrepareCompaction(*m_context, flight, candidates,
                                             count, prefix, scatter) ||
                !validator.PrepareWorkExpansion(*m_context, flight, m_gpuSceneDatabase,
                                                addresses, candidates,
                                                workSelect, workPrefix, workScatter) ||
                !validator.PrepareParallelWorkPrefix(*m_context, flight,
                                                     workBlock, workGroups, workAdd,
                                                     workScatter) ||
                !validator.PrepareRasterCommands(
                    *m_context, flight,
                    m_gpuScenePersistentGeometry.DeviceAddress(*m_context, flight),
                    m_gpuScenePersistentGeometry.Count(flight),
                    m_gpuScenePersistentGeometry.AliasAddress(*m_context, flight),
                    m_gpuScenePersistentGeometry.AliasCount(flight),
                    raster, commands))
                continue;
            // Shadow casters use a COARSER LOD budget than the lit view
            // (m_shadowLodPixelBudget), sharing the Main camera distance so
            // the shadow LOD is monotonic (never finer than the view) and the
            // silhouettes stay coherent. A coarser budget also reduces LOD
            // transitions, so shadows don't pop.
            workSelect.lodEyeBudget =
                glm::vec4(m_cameraPositioner.cameraPosition_, m_shadowLodPixelBudget);
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
            // Shadow HZB occlusion inputs: the per-flight context buffer (CPU
            // upload) and the previous frame's main-view pyramid (cross-frame,
            // fence-complete — a read hint keeps the graph barrier honest).
            if (shadowHizReady && shadowHizBda != 0u) {
                smvk::BufferSyncRequest hizRead{};
                hizRead.barrierMasks.stageMask = smvk::PipelineStageBit_Compute;
                hizRead.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
                pass.AddBufferHint(m_gpuSceneShadowHizLogical[flight], hizRead);
                if (!shadowHizSrcLogical.empty())
                    pass.AddBufferHint(shadowHizSrcLogical, hizRead);
            }
            GpuSceneCascadeBuildFrame& buildFrame =
                m_rendergraph->PassFrameData<GpuSceneCascadeBuildFrame>(pass);
            buildFrame.cullProgram = &m_gpuSceneCullProgram;
            buildFrame.compactPrograms = m_gpuSceneCompactPrograms.data();
            buildFrame.workPrograms = m_gpuSceneWorkPrograms.data();
            buildFrame.workScanPrograms = m_gpuSceneWorkScanPrograms.data();
            buildFrame.rasterProgram = &m_gpuSceneRasterBuildProgram;
            buildFrame.cull = cull;
            buildFrame.count = count;
            buildFrame.prefix = prefix;
            buildFrame.scatter = scatter;
            buildFrame.workSelect = workSelect;
            buildFrame.workBlock = workBlock;
            buildFrame.workGroups = workGroups;
            buildFrame.workAdd = workAdd;
            buildFrame.workScatter = workScatter;
            buildFrame.raster = raster;
            buildFrame.candidates = candidates;
            pass.SetRecordFn(&RecordGpuSceneCascadeBuild);
            validator.MarkDispatched(flight);
        }
        }
        const double prepMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepStart)
                .count();
        if (m_frameCounter % 60 == 0) {
            std::fprintf(stderr,
                         "[GpuSceneShadow] ticket %.3f ms | coarse %.3f ms (%u candidates, radius %.1f m) | "
                         "prep %.3f ms\n",
                         ticketMs, selectMs, selected.candidateCount, shadowFar, prepMs);
        }
    }
}

// Fused multi-view cascade path: the shared fused verdict stream (Main +
// cascades) drives each cascade's expand -> compact -> work -> scan -> raster,
// so the per-cascade CPU coarse selector + cull are skipped. Each cascade's
// expand reads the SAME fused verdicts with its own view bit and the
// shadow-caster filter (CastShadow).
if (s_gpuSceneVisibilityProd && s_gpuSceneShadows && m_gpuSceneRasterEnabled &&
    m_gpuSceneDatabaseReady && m_passes.directionalShadows && m_world &&
    cascadeViewBase != smvk::RenderViewSet::kInvalidIndex) {
    const uint32_t flight = fb.frameIndex;
    const uint32_t instanceCount = m_gpuSceneDatabase.instances.Size();
    const uint64_t fusedBda = m_gpuVisibilityRuntime.FusedVerdictAddress(*m_context, flight);
    const glm::dvec3 origin(m_cameraPositioner.cameraPosition_);
    const auto addresses = m_gpuSceneUploader.DeviceAddresses(*m_context, flight);
    if (fusedBda != 0 && instanceCount > 0u && addresses.instances != 0) {
        for (uint32_t ci = 0; ci < NUM_CASCADES; ++ci) {
            const uint32_t sourceView = cascadeViewBase + ci;
            if (sourceView >= m_renderViews.Count())
                break;
            const auto packed = tour::gpu_scene::MakeGpuSceneRenderView(
                m_renderViews.Get(sourceView), sourceView, origin);
            if (!packed.valid)
                continue;
            auto& validator = m_gpuSceneCascadeValidators[ci];
            tour::gpu_scene::FusedExpandPush expandPush{};
            tour::gpu_scene::CompactCountPush countPush{};
            tour::gpu_scene::CompactPrefixPush prefixPush{};
            tour::gpu_scene::CompactScatterPush scatterPush{};
            tour::gpu_scene::WorkSelectPush workSelect{};
            tour::gpu_scene::WorkPrefixPush workPrefix{};
            tour::gpu_scene::WorkScatterPush workScatter{};
            tour::gpu_scene::ParallelWorkBlockPush workBlock{};
            tour::gpu_scene::ParallelWorkGroupPush workGroups{};
            tour::gpu_scene::ParallelWorkAddPush workAdd{};
            tour::gpu_scene::RasterWorkDrawBuildPush rasterPush{};
            smvk::BufferHandle rasterCommands{};
            if (!validator.PrepareFusedVerdicts(*m_context, flight, instanceCount) ||
                !validator.PrepareCompaction(*m_context, flight, instanceCount, countPush,
                                             prefixPush, scatterPush) ||
                !validator.PrepareWorkExpansion(*m_context, flight, m_gpuSceneDatabase,
                                                addresses, instanceCount, workSelect,
                                                workPrefix, workScatter) ||
                !validator.PrepareParallelWorkPrefix(*m_context, flight, workBlock,
                                                     workGroups, workAdd, workScatter) ||
                !validator.PrepareRasterCommands(
                    *m_context, flight,
                    m_gpuScenePersistentGeometry.DeviceAddress(*m_context, flight),
                    m_gpuScenePersistentGeometry.Count(flight),
                    m_gpuScenePersistentGeometry.AliasAddress(*m_context, flight),
                    m_gpuScenePersistentGeometry.AliasCount(flight),
                    rasterPush, rasterCommands))
                continue;
            workSelect.lodEyeBudget =
                glm::vec4(m_cameraPositioner.cameraPosition_, m_shadowLodPixelBudget);
            workSelect.lodProjection =
                glm::vec4(float(renderExtent.height), glm::radians(kCameraFovYDegrees),
                          m_lodEnabled ? 1.0f : 0.0f, 0.0f);
            const uint64_t denseBda =
                m_context->GetBufferDeviceAddress(validator.VerdictBuffer(flight));
            if (denseBda == 0)
                continue;
            expandPush.fused = fusedBda;
            expandPush.verdicts = denseBda;
            expandPush.counts =
                glm::uvec4(instanceCount, 1u << (1u + ci), 1u /* shadow filter */, 0u);

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
                work.empty() || workBda == 0 || rasterCommands.empty())
                continue;
            if (resources.indirectBacking != rasterCommands) {
                if (!resources.indirectLogical.empty())
                    m_rendergraph->FreeBuffer(resources.indirectLogical);
                resources.indirectLogical = m_rendergraph->AddExternalBuffer(
                    rasterCommands, "GPUScene shadow indirect");
                resources.indirectBacking = rasterCommands;
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
            m_context->UploadDataToBuffer(resources.viewBuffer, &packed.raster,
                                          sizeof(packed.raster),
                                          smvk::PipelineStageBit_Vertex,
                                          smvk::AccessFlags::ShaderRead);
            const uint64_t viewBda = m_context->GetBufferDeviceAddress(resources.viewBuffer);
            if (!viewBda)
                continue;
            auto& result = gpuCascades[ci];
            result.ready = true;
            result.commands = rasterCommands;
            result.capacity = rasterPush.commandCapacity;
            result.indirectLogical = resources.indirectLogical;
            result.workLogical = resources.workLogical;
            result.viewLogical = resources.viewLogical;
            result.vs = {workBda, addresses.instances, viewBda, instanceCount,
                         rasterPush.commandCapacity};

            const uint32_t groups = (instanceCount + 63u) / 64u;
            auto& pass = m_rendergraph->AddComputePass("GpuSceneCascadeBuildFused");
            smvk::BufferSyncRequest write{};
            write.barrierMasks.stageMask = smvk::PipelineStageBit_Compute;
            write.barrierMasks.accessMask = smvk::AccessFlags::ShaderWrite;
            pass.AddBufferHint(result.indirectLogical, write);
            pass.AddBufferHint(result.workLogical, write);
            GpuSceneCascadeBuildFusedFrame& fusedFrame =
                m_rendergraph->PassFrameData<GpuSceneCascadeBuildFusedFrame>(pass);
            fusedFrame.expandProgram = &m_gpuVisibilityExpandProgram;
            fusedFrame.compactPrograms = m_gpuSceneCompactPrograms.data();
            fusedFrame.workPrograms = m_gpuSceneWorkPrograms.data();
            fusedFrame.workScanPrograms = m_gpuSceneWorkScanPrograms.data();
            fusedFrame.rasterProgram = &m_gpuSceneRasterBuildProgram;
            fusedFrame.expandPush = expandPush;
            fusedFrame.countPush = countPush;
            fusedFrame.prefixPush = prefixPush;
            fusedFrame.scatterPush = scatterPush;
            fusedFrame.workSelect = workSelect;
            fusedFrame.workPrefix = workPrefix;
            fusedFrame.workScatter = workScatter;
            fusedFrame.workBlock = workBlock;
            fusedFrame.workGroups = workGroups;
            fusedFrame.workAdd = workAdd;
            fusedFrame.rasterPush = rasterPush;
            fusedFrame.groups = groups;
            fusedFrame.instanceCount = instanceCount;
            pass.SetRecordFn(&RecordGpuSceneCascadeBuildFused);
            validator.MarkDispatched(flight);
        }
    }
}

// recordGpuCascade / hintGpuCascade are now WorldRenderer member/static
// helpers (RecordGpuCascade / HintGpuCascade); the shadow passes call them
// through `this` and no longer capture a local callable.

