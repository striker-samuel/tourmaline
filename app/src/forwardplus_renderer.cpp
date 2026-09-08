#include <App/forwardplus_renderer.h>
#include <App/frame_context.h>

#include <Engine/App/world.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Profile/profile.h>

#include <algorithm>
#include <cstring>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace app {

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

void ForwardPlusRenderer::AppendPrepass(smvk::FrameContext& base) {
	AppFrameContext& fc = static_cast<AppFrameContext&>(base);
	if (!fp)
		return;
	smvk::ResourceManager& ctx = *fc.ctx;
	smvk::RenderGraph& rg = *fc.rg;

	SMVK_PROFILE_CATEGORY("ForwardPlusDepth", Optick::Category::Rendering);

	// Resize-safe (mirrors the old frame-top Ensure in main).
	fp->Ensure(ctx, rg, { fc.extent.width, fc.extent.height }, desc, fc.depthViewHandle);
	if (!fp->Valid())
		return;

	// Temporal seed already primed depth (Clear in the seed pass): the
	// direct prepass must Load it, not Clear it. It still draws every
	// opaque node — the seed/remainder lists only cover filtered meshes,
	// so skipping here would leave direct-fallback geometry (unregistered
	// meshes, unknown-material masked clusters) out of depth. That breaks
	// the 1x Main path (read-only depth test) and the tile cull min/max,
	// while the 2x MSAA path (own transient depth) hides it.
	// Newly visible filtered geometry additionally lands in depth pass 2.
	const bool seeded = fc.seedDepthPrimed;

	// Snapshots for the recording lambdas (recorded now, executed later at
	// Execute(); nothing here may reference the frame-local fc).
	smvk::World* world = fc.world;
	const PushConstants depthPC = fc.pc;
	const smvk::LogicalImageViewHandle depthView = fc.depthView;
	const bool lodEnabled = fc.lodEnabled;
	const glm::vec3 lodEye = fc.lodEye;
	const float lodViewportHeight = fc.lodViewportHeight;
	const float lodFovY = fc.lodFovY;
	const float lodPixelBudget = fc.lodPixelBudget;

	using NodeId = ::NodeId;

	// ---- Depth prepass: fills depth for tile min/max cull ----
	// No color. Solid geometry uses the VS-only fast path; MASK cutout
	// uses the alpha-tested program so holes keep background depth.
	{
		auto& prepass = fp->AddDepthPrepass(rg, depthView,
			{ fc.extent.width, fc.extent.height }, "ForwardPlusDepth",
			seeded ? smvk::AttachmentLoadOp::Load
				: smvk::AttachmentLoadOp::Clear);
		prepass.SetRecordingFunc([this, depthPC, world, lodEnabled, lodEye,
			lodViewportHeight, lodFovY, lodPixelBudget](smvk::CommandBuffer& cmd)
		{
			// GPU context + pass scope come from RenderGraph::Execute.
			auto drawPrepassNode = [&](NodeId node, bool maskedPhase) {
				auto meshForNode = world->scene.meshForNode.find(node);
				if (meshForNode == world->scene.meshForNode.end()) return;
				smvk::Material* mat = nullptr;
				if (auto materialForNode = world->scene.materialForNode.find(node);
					materialForNode != world->scene.materialForNode.end())
					mat = world->GetMaterial(materialForNode->second);
				const bool masked = mat &&
					mat->shadingModel == smvk::ShadingModel::Masked;
				if (masked != maskedPhase) return;
				auto& model = world->scene.globalTransform[node];
				// Error-driven LOD (same budget as the main view): the
				// prepass fills the depth the tile cull + Main test
				// against, so it must rasterize the same LOD Main draws.
				smvk::StaticMesh* meshDesc = nullptr;
				if (lodEnabled && node < world->scene.globalTransform.size()) {
					const glm::mat4& m = world->scene.globalTransform[node];
					const glm::vec3 center{ m[3][0], m[3][1], m[3][2] };
					const float distance = glm::length(center - lodEye);
					const uint32_t lod = world->SelectLodIndex(meshForNode->second,
						distance, lodViewportHeight, lodFovY, lodPixelBudget);
					meshDesc = world->GetLodMesh(meshForNode->second, lod);
				}
				else {
					meshDesc = world->GetMesh(meshForNode->second);
				}
				if (!meshDesc) return;
				PushConstants objectPC{};
				std::copy_n(glm::value_ptr(model), 16, objectPC.model);
				if (maskedPhase) {
					auto materialForNode = world->scene.materialForNode.find(node);
					objectPC.materialId = materialForNode->second;
					cmd.PushConstants(offsetof(PushConstants, materialId),
						sizeof(objectPC.materialId), &objectPC.materialId,
						smvk::ShaderStage::All);
				}
				cmd.PushConstants(offsetof(PushConstants, model), sizeof(objectPC.model), objectPC.model, smvk::ShaderStage::All);
				for (auto& draw_set : meshDesc->geometry.surfaces) {
					cmd.DrawIndexed(draw_set.indexCount, 1, draw_set.firstIndex, draw_set.baseVertex);
				};
			};
			// Phase 1: solid geometry, VS-only fast path.
			cmd.BindRenderPipeline(
				fp->depthProgram,
				smvk::VertexFormat::StaticVertexFormat,
				smvk::DepthSettings{
					.depthFunc = smvk::CompareOp::GreaterEqual,
					.writeEnable = true,
					.depthTestEnable = true,
				},
				smvk::StencilSettings{},
				smvk::BlendSettings{},
				smvk::MSAASettings{},
				smvk::RasterSettings{
					.cullMode = smvk::CullMode::Back
				}
			);
			cmd.PushConstants(0, sizeof(PushConstants), &depthPC, smvk::ShaderStage::All);
			world->forEachNode(
				smvk::IteratorFlagBit_Renderable
				| smvk::IteratorFlagBit_Opaque
				| smvk::IteratorFlagBit_StaticVertexFmt
				, [&](NodeId node) { drawPrepassNode(node, false); });
			// Phase 2: MASK cutout, alpha-tested so holes keep
			// background depth. Cull off: foliage is double-sided.
			cmd.BindRenderPipeline(
				prepassMaskedProgram,
				smvk::VertexFormat::StaticVertexFormat,
				smvk::DepthSettings{
					.depthFunc = smvk::CompareOp::GreaterEqual,
					.writeEnable = true,
					.depthTestEnable = true,
				},
				smvk::StencilSettings{},
				smvk::BlendSettings{},
				smvk::MSAASettings{},
				smvk::RasterSettings{
					.cullMode = smvk::CullMode::None
				}
			);
			cmd.PushConstants(0, sizeof(PushConstants), &depthPC, smvk::ShaderStage::All);
			world->forEachNode(
				smvk::IteratorFlagBit_Renderable
				| smvk::IteratorFlagBit_Opaque
				| smvk::IteratorFlagBit_StaticVertexFmt
				, [&](NodeId node) { drawPrepassNode(node, true); });
		});
	}
}

void ForwardPlusRenderer::AppendTileCull(smvk::FrameContext& base) {
	AppFrameContext& fc = static_cast<AppFrameContext&>(base);
	if (!fp)
		return;
	smvk::ResourceManager& ctx = *fc.ctx;
	smvk::RenderGraph& rg = *fc.rg;

	SMVK_PROFILE_CATEGORY("ForwardPlusCull", Optick::Category::Rendering);
	SMVK_PROFILE_TAG("tileCountX", fp->tileCountX);
	SMVK_PROFILE_TAG("tileCountY", fp->tileCountY);
	SMVK_PROFILE_TAG("lightCount", fc.fpLightCount);

	fp->Ensure(ctx, rg, { fc.extent.width, fc.extent.height }, desc, fc.depthViewHandle);
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
		cullPC.depthTextureIndex = fp->depthBindlessIndex;
		cullPC.tileCountX = fp->tileCountX;
		cullPC.tileCountY = fp->tileCountY;
		cullPC.lightCount = fc.fpLightCount;
		cullPC.tileSize = desc.tileSize;
		cullPC.maxLightsPerTile = desc.maxLightsPerTile;
		auto& cullPass = fp->AddCullPass(rg, fc.depthView, fc.fpLights, "ForwardPlusCull");
		cullPass.SetRecordingFunc([this, cullPC, tileCountX = fp->tileCountX, tileCountY = fp->tileCountY](smvk::CommandBuffer& cmd) mutable
		{
			// GPU context + pass scope come from RenderGraph::Execute.
			cmd.BindComputePipeline(fp->cullProgram);
			cmd.PushConstants(0, sizeof(smvk::ForwardPlusCullPush), &cullPC,
				smvk::ShaderStage::All);
			cmd.Dispatch(tileCountX, tileCountY, 1);
		});
	}
}

} // namespace app
