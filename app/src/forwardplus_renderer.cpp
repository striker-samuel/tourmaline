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
	// Dithered transparency (default): Blend materials render with opaque
	// state + screen-door discard, so they join the depth prepass like
	// cutout (alpha-tested program dithers via shadow.frag). Blended mode
	// keeps the legacy behavior (transparent excluded from depth).
	const bool dithered = fc.ditheredTransparency;

	// Snapshots for the recording lambdas (recorded now, executed later at
	// Execute(); nothing here may reference the frame-local fc).
	smvk::World* world = fc.world;
	const PushConstants depthPC = fc.pc;
	const smvk::LogicalImageViewHandle depthView = fc.depthView;
	const bool lodEnabled = fc.lodEnabled;
	const smvk::LodFrameInputs lodInputs = fc.lodInputs;

	using NodeId = ::NodeId;

	// ---- Depth prepass: fills depth for tile min/max cull ----
	// No color. Solid geometry uses the VS-only fast path; MASK cutout
	// uses the alpha-tested program so holes keep background depth.
	{
		auto& prepass = fp->AddDepthPrepass(rg, depthView,
			{ fc.extent.width, fc.extent.height }, "ForwardPlusDepth",
			seeded ? smvk::AttachmentLoadOp::Load
				: smvk::AttachmentLoadOp::Clear);
		prepass.SetRecordingFunc([this, depthPC, world, lodEnabled, lodInputs, dithered](smvk::CommandBuffer& cmd)
		{
			// GPU context + pass scope come from RenderGraph::Execute.
			auto drawPrepassNode = [&](const smvk::World::RenderItem& item, bool maskedPhase) {
				// Masked (cutout) flag replaces the per-node material
				// fetch the phase split used to pay. Material-less meshes
				// carry no flag and stay in the solid phase (drawn, like
				// the old mat == nullptr path).
				if (((item.flags & smvk::IteratorFlagBit_Masked) != 0) != maskedPhase)
					return;
				auto& model = world->scene.globalTransform[item.node];
				// Error-driven LOD (same budget as the main view): the
				// prepass fills the depth the tile cull + Main test
				// against, so it must rasterize the same LOD Main draws.
				// Halfrate cache: World::LodIndexForNode refreshes half the
				// nodes per frame (parity split) — the prepass, main pass
				// and filter all share one decision per node per frame.
				smvk::StaticMesh* meshDesc = nullptr;
				if (lodEnabled) {
					const uint32_t lod = world->LodIndexForNode(item.node,
						item.mesh, lodInputs);
					meshDesc = world->GetLodMesh(item.mesh, lod);
				}
				else {
					meshDesc = world->GetMesh(item.mesh);
				}
				if (!meshDesc) return;
				PushConstants objectPC{};
				std::copy_n(glm::value_ptr(model), 16, objectPC.model);
				if (maskedPhase) {
					objectPC.materialId = item.material;
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
		world->forEachRenderItem(
			smvk::IteratorFlagBit_Renderable
			| smvk::IteratorFlagBit_Opaque
			| smvk::IteratorFlagBit_StaticVertexFmt
			, [&](const smvk::World::RenderItem& item) { drawPrepassNode(item, false); });
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
		world->forEachRenderItem(
			smvk::IteratorFlagBit_Renderable
			| smvk::IteratorFlagBit_Opaque
			| smvk::IteratorFlagBit_StaticVertexFmt
			, [&](const smvk::World::RenderItem& item) { drawPrepassNode(item, true); });
			// Phase 3: dithered transparent – same alpha-tested program
			// (shadow.frag screen-doors Blend materials when the frame
			// requests dithering). Culling follows the material (single-
			// sided glass backfaces must stay culled so the main view,
			// which culls them too, agrees on depth).
			if (dithered) {
				auto drawDithered = [&](bool doubleSidedPhase) {
					world->forEachRenderItem(
						smvk::IteratorFlagBit_Renderable
						| smvk::IteratorFlagBit_Transparent
						| smvk::IteratorFlagBit_StaticVertexFmt
						, [&](const smvk::World::RenderItem& item) {
							if (item.material == smvk::World::kInvalidIndex)
								return;
							if (const smvk::Material* mat = world->GetMaterial(item.material)) {
								if (mat->isDoubleSided() != doubleSidedPhase)
									return;
							}
							else if (doubleSidedPhase) {
								return; // material-less: single-sided phase
							}
							auto& model = world->scene.globalTransform[item.node];
						smvk::StaticMesh* meshDesc = nullptr;
						if (lodEnabled) {
							const uint32_t lod = world->LodIndexForNode(item.node,
								item.mesh, lodInputs);
							meshDesc = world->GetLodMesh(item.mesh, lod);
						}
						else {
							meshDesc = world->GetMesh(item.mesh);
						}
						if (!meshDesc) return;
						PushConstants objectPC{};
						std::copy_n(glm::value_ptr(model), 16, objectPC.model);
						objectPC.materialId = item.material;
						cmd.PushConstants(offsetof(PushConstants, materialId),
							sizeof(objectPC.materialId), &objectPC.materialId,
							smvk::ShaderStage::All);
						cmd.PushConstants(offsetof(PushConstants, model), sizeof(objectPC.model), objectPC.model, smvk::ShaderStage::All);
						for (auto& draw_set : meshDesc->geometry.surfaces) {
							cmd.DrawIndexed(draw_set.indexCount, 1, draw_set.firstIndex, draw_set.baseVertex);
						};
					});
				};
				auto bindDithered = [&](smvk::CullMode cull) {
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
						smvk::RasterSettings{ .cullMode = cull }
					);
					cmd.PushConstants(0, sizeof(PushConstants), &depthPC, smvk::ShaderStage::All);
				};
				bindDithered(smvk::CullMode::Back);
				drawDithered(false);
				bindDithered(smvk::CullMode::None);
				drawDithered(true);
			}
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
