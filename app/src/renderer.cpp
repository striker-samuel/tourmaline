#include <App/renderer.h>

#include <Engine/Renderer/mesh.h>
#include <Engine/platform.h>

#include <algorithm>
#include <cstdio>

namespace app {

bool GfxRenderer::Ensure(smvk::ResourceManager& ctx, smvk::RenderGraph& rg) {
	const char* vsPath = smvk::gfx::kGfxFilteredVsPath;
	if (!smvk::gfx::Ensure(gfx, ctx, rg))
		return false;
	if (filteredProgram.shaderCount != 0 && !filteredVsModule.empty())
		return true;
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
	return true;
}

void GfxRenderer::AppendPasses(smvk::FrameContext& fc) {
	lastSlot_ = UINT32_MAX;
	if (!fc.world || !fc.views || fc.mainViewIndex == smvk::RenderViewSet::kInvalidIndex)
		return;
	lastSlot_ = FilterMainView(*fc.ctx, *fc.rg, fc.frameIndex, *fc.world,
		*fc.views, fc.mainViewIndex, fc.eye,
		{ fc.extent.width, fc.extent.height }, config.cullFlags);
	if (lastSlot_ == UINT32_MAX)
		return;
	smvk::RenderPass& pass = AddFilterPass(*fc.rg, lastSlot_);
	GfxRenderer* self = this;
	smvk::ResourceManager* ctx = fc.ctx;
	pass.SetRecordingFunc([self, ctx, slot = lastSlot_](smvk::CommandBuffer& cmd)
	{
		smvk::gfx::PushAndDispatch(self->gfx, cmd, *ctx, slot);
	});
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
			// them or foliage/cutout backs vanish.
			smvk::gfx::DrawFlags drawFlags = 0;
			if (const smvk::Material* mat =
				world.GetMaterial(materialForNode->second)) {
				if (mat->isDoubleSided())
					drawFlags |= smvk::gfx::DrawFlagBit_TwoSided;
			}
			smvk::gfx::PushDraw(gfx, slot, meshMap_[worldMesh], model,
				materialForNode->second, drawFlags);
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
