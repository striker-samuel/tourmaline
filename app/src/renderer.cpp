#include <App/renderer.h>

#include <Engine/Renderer/mesh.h>
#include <Engine/platform.h>

#include <algorithm>

namespace app {

bool GfxRenderer::Ensure(smvk::ResourceManager& ctx, smvk::RenderGraph& rg,
	smvk::ShaderModuleHandle mainFrag, const char* vsPath) {
	if (!smvk::gfx::Ensure(gfx, ctx, rg))
		return false;
	if (filteredProgram.shaderCount != 0 && !filteredVsModule.empty())
		return true;
	try {
		if (filteredVsModule.empty())
			filteredVsModule = ctx.CreateShaderModule(
				readFile(vsPath ? vsPath : smvk::gfx::kGfxFilteredVsPath),
				"gfxFilteredVS");
	}
	catch (...) {
		return false;
	}
	if (filteredVsModule.empty() || mainFrag.empty())
		return false;
	smvk::Shader vs{ .module = filteredVsModule, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex };
	smvk::Shader fs{ .module = mainFrag, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment };
	filteredProgram = { .shaders = { vs, fs }, .shaderCount = 2 };
	return true;
}

void GfxRenderer::RegisterWorldMeshes(smvk::World& world) {
	meshMap_.assign(world.meshLocations.size(), UINT32_MAX);
	for (size_t i = 0; i < world.meshLocations.size(); ++i) {
		const smvk::World::MeshLocation& loc = world.meshLocations[i];
		if (!loc.cluster)
			continue;
		const smvk::StaticMesh* mesh = loc.cluster->staticMeshes.get(loc.handle);
		if (!mesh || mesh->geometry.surfaces.empty())
			continue;
		const uint32_t baseVertex =
			(uint32_t)std::max<int32_t>(mesh->geometry.surfaces[0].baseVertex, 0);
		uint32_t gfxMesh = UINT32_MAX;
		if (mesh->meshlets && !mesh->meshlets->empty()) {
			gfxMesh = smvk::gfx::RegisterMeshFromCooked(gfx, *mesh->meshlets, baseVertex);
		}
		else if (const smvk::StaticMeshUpload* retained =
			loc.cluster->GetRetainedUpload(loc.handle)) {
			gfxMesh = smvk::gfx::RegisterMeshFromUpload(gfx, *retained, baseVertex);
		}
		meshMap_[i] = gfxMesh;
	}
}

void GfxRenderer::UploadStaticData(smvk::ResourceManager& ctx, smvk::RenderGraph& rg) {
	smvk::gfx::UploadStaticData(gfx, ctx, rg);
}

uint32_t GfxRenderer::FilterMainView(smvk::ResourceManager& ctx, smvk::RenderGraph& rg,
	smvk::World& world, const smvk::RenderViewSet& views, uint32_t mainViewIdx,
	const glm::vec3& eye, const smvk::Extent2D& extent, smvk::gfx::CullFlags flags) {
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
			smvk::gfx::PushDraw(gfx, slot, meshMap_[worldMesh], model,
				materialForNode->second);
		});
	smvk::gfx::FinalizeView(gfx, ctx, slot);
	return slot;
}

smvk::RenderPass& GfxRenderer::AddFilterPass(smvk::RenderGraph& rg, uint32_t slot) {
	return smvk::gfx::AddFilterPass(gfx, rg, slot);
}

void GfxRenderer::PushViewConstants(smvk::CommandBuffer& cmd, smvk::ResourceManager& ctx,
	uint32_t slot, uint64_t renderViewsAddr, uint32_t renderViewIndex) {
	smvk::gfx::FilteredVsPush tail{};
	tail.draws = ctx.GetBufferDeviceAddress(gfx.drawBuffer);
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
