#include <App/renderer.h>
#include <App/frame_context.h>
#include <App/push_constants.h>

#include <Engine/Renderer/mesh.h>
#include <Engine/platform.h>

#include <algorithm>
#include <cstdio>

namespace app {

static constexpr const char* kGfxSeedVsPath = "content/shaders/gfxseed.vert.hlsl.spv";
static constexpr const char* kGfxSeedMaskedVsPath = "content/shaders/gfxseedmasked.vert.hlsl.spv";

bool GfxRenderer::Ensure(smvk::ResourceManager& ctx, smvk::RenderGraph& rg) {
	const char* vsPath = smvk::gfx::kGfxFilteredVsPath;
	if (!smvk::gfx::Ensure(gfx, ctx, rg))
		return false;
	if (filteredProgram.shaderCount == 0 || filteredVsModule.empty()) {
		try {
			if (filteredVsModule.empty())
				filteredVsModule = ctx.CreateShaderModule(
					readFile(vsPath), "gfxFilteredVS");
		}
		catch (...) {
			return false;
		}
		if (filteredVsModule.empty() || mainFrag_.empty())
			return false;
		smvk::Shader vs{ .module = filteredVsModule, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex };
		smvk::Shader fs{ .module = mainFrag_, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment };
		filteredProgram = { .shaders = { vs, fs }, .shaderCount = 2 };
	}
	// Seed-depth programs (T3). One SPIR-V per phase (the shader build
	// emits a single entry point per file). Missing seed frag
	// (ConfigureSeedFrag not called) disables seeding but leaves the
	// filter path intact.
	if (seedOpaqueProgram.shaderCount == 0 || seedMaskedProgram.shaderCount == 0) {
		smvk::Holder<smvk::ShaderModuleHandle> seedMaskedVsModule;
		try {
			if (seedVsModule.empty())
				seedVsModule = ctx.CreateShaderModule(
					readFile(kGfxSeedVsPath), "gfxSeedVS");
			seedMaskedVsModule = ctx.CreateShaderModule(
				readFile(kGfxSeedMaskedVsPath), "gfxSeedMaskedVS");
		}
		catch (...) {
			return true; // filter works; seed stays off
		}
		if (!seedVsModule.empty()) {
			smvk::Shader seedVs{ .module = seedVsModule, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex };
			seedOpaqueProgram = { .shaders = { seedVs }, .shaderCount = 1 };
		}
		if (!seedMaskedVsModule.empty() && !seedFrag_.empty()) {
			smvk::Shader seedVsMasked{ .module = seedMaskedVsModule, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex };
			smvk::Shader seedFs{ .module = seedFrag_, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment };
			seedMaskedProgram = { .shaders = { seedVsMasked, seedFs }, .shaderCount = 2 };
			seedMaskedVsModule_ = std::move(seedMaskedVsModule);
		}
	}
	return true;
}

void GfxRenderer::AppendPasses(smvk::FrameContext& base) {
	AppFrameContext& fc = static_cast<AppFrameContext&>(base);
	lastSlot_ = UINT32_MAX;
	seededThisFrame_ = false;
	if (!fc.world || !fc.views || fc.mainViewIndex == smvk::RenderViewSet::kInvalidIndex)
		return;
	// NOTE: the seed pass below captures fc.pc (frame addresses for the
	// masked FS) by value, so main must call AppendPasses after fc.pc is
	// built but before the Forward+ prepass (seed writes depth first).
	lastSlot_ = FilterMainView(*fc.ctx, *fc.rg, fc.frameIndex, *fc.world,
		*fc.views, fc.mainViewIndex, fc.eye,
		{ fc.extent.width, fc.extent.height }, config.cullFlags);
	if (lastSlot_ == UINT32_MAX)
		return;
	const uint32_t slot = lastSlot_;
	// Extent drift vs. the armed history (SnapshotVisible stamps the filter
	// extent into the temporal below, in FilterMainView's rotation step).
	{
		smvk::gfx::TemporalView& t = smvk::gfx::Temporal(gfx, slot);
		if ((t.width != 0 || t.height != 0)
			&& (t.width != fc.extent.width || t.height != fc.extent.height))
			smvk::gfx::InvalidateHistory(gfx, slot);
	}
	// HZB sizing (steady-state early-out; rebuilds disarm history).
	smvk::gfx::EnsureHiz(gfx, *fc.ctx, *fc.rg, slot,
		fc.extent.width, fc.extent.height);
	// Seed depth from last frame's visible set (prev* intact: rotation into
	// prev* happens below, after the seed block).
	if (smvk::gfx::Temporal(gfx, slot).historyValid
		&& seedOpaqueProgram.shaderCount != 0
		&& seedMaskedProgram.shaderCount != 0) {
		smvk::gfx::BuildSeedCommands(gfx, slot);
		smvk::gfx::UploadSeed(gfx, *fc.ctx, fc.frameIndex, slot);
		const auto& ranges = smvk::gfx::SeedRanges(gfx, slot);
		const uint32_t opaqueCount = smvk::gfx::SeedOpaqueCommands(gfx, slot);
		if (!ranges.empty()) {
			smvk::RenderPass& seedPass = smvk::gfx::AddSeedPass(
				gfx, *fc.rg, slot, fc.depthView);
			smvk::gfx::AddSeedConsumeHints(seedPass,
				gfx.filterIndexLogical,
				smvk::gfx::VisDrawLogical(gfx, slot),
				smvk::gfx::SeedIndirectLogical(gfx, slot));
			GfxRenderer* self = this;
			smvk::ResourceManager* ctx = fc.ctx;
			const PushConstants depthPC = fc.pc;
			const uint64_t renderViewsAddr =
				fc.views->DeviceAddress(*fc.ctx, fc.frameIndex);
			const uint32_t renderViewIndex = fc.mainViewIndex;
			seedPass.SetRecordingFunc([self, ctx, slot, depthPC, renderViewsAddr, renderViewIndex, opaqueCount](smvk::CommandBuffer& cmd)
			{
				const auto& seedRanges = smvk::gfx::SeedRanges(self->gfx, slot);
				cmd.BindStaticVertexFmt();
				cmd.BindIndexBuffer(smvk::gfx::FilterIndexBuffer(self->gfx));
				// NOTE: no push constants before the first BindRenderPipeline
				// below — PushConstants asserts on a bound pipeline layout
				// (impl.cpp). Each phase pushes after its own bind.
				// Phase 1: opaque (VS-only, back-culled like the prepass).
				if (opaqueCount > 0) {
					cmd.BindRenderPipeline(
						self->seedOpaqueProgram,
						smvk::VertexFormat::StaticVertexFormat,
						smvk::DepthSettings{
							.depthFunc = smvk::CompareOp::GreaterEqual,
							.writeEnable = true,
							.depthTestEnable = true,
						},
						smvk::StencilSettings{},
						smvk::BlendSettings{},
						smvk::MSAASettings{},
						smvk::RasterSettings{ .cullMode = smvk::CullMode::Back }
					);
					cmd.PushConstants(0, sizeof(PushConstants), &depthPC, smvk::ShaderStage::All);
					self->PushSeedConstants(cmd, *ctx, slot, renderViewsAddr, renderViewIndex);
					cmd.DrawIndexedIndirect(smvk::gfx::SeedIndirectBuffer(self->gfx, slot),
						0, opaqueCount, 20);
				}
				// Phase 2: MASK cutout (alpha-tested FS, cull off).
				for (const smvk::gfx::SeedRange& r : seedRanges) {
					if ((r.drawSet & smvk::gfx::DrawSetBit_Masked) == 0 || r.commandCount == 0)
						continue;
					if (r.material == UINT32_MAX)
						continue; // unknown material: no valid materialId push;
						// the full prepass still depths these clusters (T3)
					cmd.BindRenderPipeline(
						self->seedMaskedProgram,
						smvk::VertexFormat::StaticVertexFormat,
						smvk::DepthSettings{
							.depthFunc = smvk::CompareOp::GreaterEqual,
							.writeEnable = true,
							.depthTestEnable = true,
						},
						smvk::StencilSettings{},
						smvk::BlendSettings{},
						smvk::MSAASettings{},
						smvk::RasterSettings{ .cullMode = smvk::CullMode::None }
					);
					PushConstants matPC{};
					matPC.materialId = r.material;
					cmd.PushConstants(
						offsetof(PushConstants, materialId),
						sizeof(matPC.materialId),
						&matPC.materialId,
						smvk::ShaderStage::All
					);
					cmd.PushConstants(
						0,
						offsetof(PushConstants, frameUniforms),
						&depthPC,
						smvk::ShaderStage::All
					);
					self->PushSeedConstants(cmd, *ctx, slot, renderViewsAddr, renderViewIndex);
					self->DrawSeedRange(cmd, slot, (uint32_t)(&r - seedRanges.data()));
				}
			});
			seededThisFrame_ = true;
		}
	}
	// Rotation for next frame (must run after the seed block above).
	smvk::gfx::SnapshotVisible(gfx, slot);
	smvk::gfx::UploadVisible(gfx, *fc.ctx, fc.frameIndex, slot);
	smvk::RenderPass& pass = AddFilterPass(*fc.rg, slot);
	GfxRenderer* self = this;
	smvk::ResourceManager* ctx = fc.ctx;
	pass.SetRecordingFunc([self, ctx, slot](smvk::CommandBuffer& cmd)
	{
		smvk::gfx::PushAndDispatch(self->gfx, cmd, *ctx, slot);
	});
}

void GfxRenderer::AppendHizPasses(smvk::FrameContext& base,
	smvk::LogicalImageViewHandle depthLogical, uint32_t depthBindlessIndex) {
	AppFrameContext& fc = static_cast<AppFrameContext&>(base);
	if (lastSlot_ == UINT32_MAX || lastSlot_ >= smvk::gfx::ViewCount(gfx))
		return;
	const uint32_t slot = lastSlot_;
	const smvk::gfx::TemporalView& t = smvk::gfx::Temporal(gfx, slot);
	if (t.hiz.mipCount == 0)
		return;
	GfxRenderer* self = this;
	smvk::ResourceManager* ctx = fc.ctx;
	for (uint32_t m = 0; m < t.hiz.mipCount; ++m) {
		const bool fromDepth = (m == 0);
		smvk::RenderPass& hizPass = smvk::gfx::AddHizPass(gfx, *fc.rg, slot, m,
			fromDepth ? depthLogical : smvk::LogicalImageViewHandle{});
		hizPass.SetRecordingFunc([self, ctx, slot, m, depthBindlessIndex, fromDepth](smvk::CommandBuffer& cmd)
		{
			smvk::gfx::PushAndDispatchHiz(self->gfx, cmd, *ctx, slot, m,
				fromDepth ? depthBindlessIndex : UINT32_MAX);
		});
	}
}

void GfxRenderer::RegisterWorldMeshes(smvk::World& world) {
	meshMap_.assign(world.meshLocations.size(), UINT32_MAX);
	uint32_t fromCooked = 0, fromUpload = 0, skipped = 0;
	uint64_t skippedTris = 0;
	for (size_t i = 0; i < world.meshLocations.size(); ++i) {
		const smvk::World::MeshLocation& loc = world.meshLocations[i];
		if (!loc.cluster) {
			++skipped;
			continue;
		}
		const smvk::StaticMesh* mesh = loc.cluster->staticMeshes.get(loc.handle);
		if (!mesh || mesh->geometry.surfaces.empty()) {
			++skipped;
			continue;
		}
		const uint32_t baseVertex =
			(uint32_t)std::max<int32_t>(mesh->geometry.surfaces[0].baseVertex, 0);
		uint32_t gfxMesh = UINT32_MAX;
		if (mesh->meshlets && !mesh->meshlets->empty()) {
			gfxMesh = smvk::gfx::RegisterMeshFromCooked(gfx, *mesh->meshlets, baseVertex);
			if (gfxMesh != UINT32_MAX)
				++fromCooked;
		}
		else if (const smvk::StaticMeshUpload* retained =
			loc.cluster->GetRetainedUpload(loc.handle)) {
			gfxMesh = smvk::gfx::RegisterMeshFromUpload(gfx, *retained, baseVertex);
			if (gfxMesh != UINT32_MAX)
				++fromUpload;
		}
		if (gfxMesh == UINT32_MAX) {
			++skipped;
			skippedTris += mesh->geometry.indexCount / 3;
		}
		meshMap_[i] = gfxMesh;
	}
	fprintf(stderr,
		"[GfxFilter] meshes: %u cooked + %u runtime-cooked = %u filtered, %u direct (%llu tris)\n",
		fromCooked, fromUpload, fromCooked + fromUpload, skipped,
		(unsigned long long)skippedTris);
	fprintf(stderr,
		"[GfxFilter] clusters: %u, filter indices: %llu\n",
		smvk::gfx::ClusterCount(gfx),
		(unsigned long long)gfx.filterIndices.size());
}

void GfxRenderer::UploadStaticData(smvk::ResourceManager& ctx, smvk::RenderGraph& rg) {
	smvk::gfx::UploadStaticData(gfx, ctx, rg);
}

uint32_t GfxRenderer::FilterMainView(smvk::ResourceManager& ctx, smvk::RenderGraph& rg,
	uint32_t frameIndex, smvk::World& world, const smvk::RenderViewSet& views,
	uint32_t mainViewIdx, const glm::vec3& eye, const smvk::Extent2D& extent,
	smvk::gfx::CullFlags flags) {
	smvk::gfx::BeginFrame(gfx);
	if (mainViewIdx == smvk::RenderViewSet::kInvalidIndex)
		return UINT32_MAX;
	const smvk::RenderView& view = views.Get(mainViewIdx);
	const uint32_t slot = smvk::gfx::BeginView(gfx, ctx, rg, view, eye,
		flags, extent.width, extent.height);
	if (slot == UINT32_MAX)
		return UINT32_MAX;
	world.forEachNode(
		smvk::IteratorFlagBit_Renderable
		| smvk::IteratorFlagBit_Opaque
		| smvk::IteratorFlagBit_StaticVertexFmt,
		[&](NodeId node) {
			auto meshForNode = world.scene.meshForNode.find(node);
			if (meshForNode == world.scene.meshForNode.end())
				return;
			auto materialForNode = world.scene.materialForNode.find(node);
			if (materialForNode == world.scene.materialForNode.end())
				return;
			const uint32_t worldMesh = meshForNode->second;
			if (worldMesh >= meshMap_.size() || meshMap_[worldMesh] == UINT32_MAX)
				return; // not filterable: legacy direct path still draws it
			const glm::mat4& model = world.scene.globalTransform[node];
			// Mesh-level sphere pre-cull (same test as the shadow passes).
			if (const smvk::StaticMesh* meshDesc = world.GetMesh(worldMesh)) {
				glm::vec3 center;
				float radius = 0.0f;
				smvk::MeshBoundsToWorld(model, meshDesc->geometry.aabbMin,
					meshDesc->geometry.aabbMax, center, radius);
				if (!smvk::RenderViewIntersectsSphere(view, center, radius))
					return;
			}
			// Double-sided materials bind with Cull None (see
			// bindForMaterial): the filter must not backface/cone-cull
			// them or foliage/cutout backs vanish. The draw-set bucket
			// (opaque vs. alpha-tested) selects the seed depth PSO in T3.
			smvk::gfx::DrawFlags drawFlags = 0;
			smvk::gfx::DrawSetMask drawSet = smvk::gfx::DrawSetBit_Opaque;
			if (const smvk::Material* mat =
				world.GetMaterial(materialForNode->second)) {
				if (mat->isDoubleSided())
					drawFlags |= smvk::gfx::DrawFlagBit_TwoSided;
				if (mat->shadingModel == smvk::ShadingModel::Masked)
					drawSet = smvk::gfx::DrawSetBit_Masked;
			}
			smvk::gfx::PushDraw(gfx, slot, meshMap_[worldMesh], model,
				materialForNode->second, drawFlags, drawSet);
		});
	const smvk::gfx::Stats stats = smvk::gfx::FinalizeView(gfx, ctx, frameIndex, slot);
	if ((frameCounter_++ % 300) == 0) {
		fprintf(stderr,
			"[GfxFilter] view: %u draws, %u batches, %u tris processed, %u/%u clusters culled\n",
			stats.draws, stats.commands, stats.trianglesProcessed,
			stats.clustersCulled, stats.clustersProcessed);
	}
	return slot;
}

smvk::RenderPass& GfxRenderer::AddFilterPass(smvk::RenderGraph& rg, uint32_t slot) {
	return smvk::gfx::AddFilterPass(gfx, rg, slot);
}

void GfxRenderer::PushViewConstants(smvk::CommandBuffer& cmd, smvk::ResourceManager& ctx,
	uint32_t slot, uint64_t renderViewsAddr, uint32_t renderViewIndex) {
	smvk::gfx::FilteredVsPush tail{};
	tail.draws = ctx.GetBufferDeviceAddress(smvk::gfx::DrawBuffer(gfx, slot));
	tail.renderViews = renderViewsAddr;
	tail.renderViewIndex = renderViewIndex;
	cmd.PushConstants(smvk::gfx::kFilteredVsPushOffset, sizeof(tail), &tail,
		smvk::ShaderStage::All);
}

void GfxRenderer::PushSeedConstants(smvk::CommandBuffer& cmd, smvk::ResourceManager& ctx,
	uint32_t slot, uint64_t renderViewsAddr, uint32_t renderViewIndex) {
	smvk::gfx::FilteredVsPush tail{};
	tail.draws = ctx.GetBufferDeviceAddress(smvk::gfx::VisDrawBuffer(gfx, slot));
	tail.renderViews = renderViewsAddr;
	tail.renderViewIndex = renderViewIndex;
	cmd.PushConstants(smvk::gfx::kFilteredVsPushOffset, sizeof(tail), &tail,
		smvk::ShaderStage::All);
}

void GfxRenderer::DrawSeedRange(smvk::CommandBuffer& cmd, uint32_t slot, uint32_t rangeIdx) {
	const std::vector<smvk::gfx::SeedRange>& ranges =
		smvk::gfx::SeedRanges(gfx, slot);
	assert(rangeIdx < ranges.size());
	const smvk::gfx::SeedRange& r = ranges[rangeIdx];
	if (r.commandCount == 0)
		return;
	cmd.BindIndexBuffer(smvk::gfx::FilterIndexBuffer(gfx));
	cmd.DrawIndexedIndirect(smvk::gfx::SeedIndirectBuffer(gfx, slot),
		(uint64_t)r.firstCommand * 20, r.commandCount, 20);
}

void GfxRenderer::DrawRange(smvk::CommandBuffer& cmd, smvk::ResourceManager& ctx,
	uint32_t slot, uint32_t rangeIdx) {
	(void)ctx;
	const std::vector<smvk::gfx::MaterialRange>& ranges =
		smvk::gfx::MaterialRanges(gfx, slot);
	assert(rangeIdx < ranges.size());
	const smvk::gfx::MaterialRange& r = ranges[rangeIdx];
	if (r.commandCount == 0)
		return;
	cmd.BindIndexBuffer(smvk::gfx::FilteredBuffer(gfx, slot));
	cmd.DrawIndexedIndirect(smvk::gfx::IndirectBuffer(gfx, slot),
		(uint64_t)r.firstCommand * 20, r.commandCount, 20);
}

} // namespace app
