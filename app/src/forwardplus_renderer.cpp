#include <App/forwardplus_renderer.h>
#include <App/frame_context.h>

#include <Engine/App/world.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Renderer/renderview.h>
#include <Engine/Profile/profile.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <memory>
#include <vector>

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
	// *visible* opaque node — the seed/remainder lists only cover filtered
	// meshes, so skipping here would leave direct-fallback geometry
	// (unregistered meshes, unknown-material masked clusters) out of depth.
	// That breaks the 1x Main path (read-only depth test) and the tile
	// cull min/max, while the 2x MSAA path (own transient depth) hides it.
	// Frustum culling below only removes off-screen nodes, so on-screen
	// fallback geometry is still covered. Newly visible filtered geometry
	// additionally lands in depth pass 2.
	const bool seeded = fc.seedDepthPrimed;
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

	// Snapshots for the recording lambdas (recorded now, executed later at
	// Execute(); nothing here may reference the frame-local fc).
	smvk::World* world = fc.world;
	const PushConstants depthPC = fc.pc;
	const smvk::LogicalImageViewHandle depthView = fc.depthView;
	const bool lodEnabled = fc.lodEnabled;
	const smvk::LodFrameInputs lodInputs = fc.lodInputs;

	// ---- CPU frustum culling for the prepass ----
	// Static nodes: single frustumCullBVH walk over the World's cached
	// static tree (built once in ResolveAndFixup_clusters from frozen
	// static-subtree globals, LOD-union bounds). Dynamic nodes: per-item
	// sphere test below (they never enter the static tree).
	// Both tests are conservative (guarded main-view planes + LOD-union
	// spheres), so a culled node is guaranteed off-screen: skipping it
	// cannot punch holes in depth. When no view/BVH is available the mask
	// stays empty and the prepass falls back to drawing everything.
	smvk::RenderView mainView{};
	bool hasMainView = false;
	if (fc.views && fc.mainViewIndex != smvk::RenderViewSet::kInvalidIndex
		&& fc.mainViewIndex < fc.views->Count()) {
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
	// No color. Solid geometry uses the VS-only fast path; MASK cutout
	// uses the alpha-tested program so holes keep background depth.
	// Transparent (Blend) is intentionally skipped — see above.
	{
		auto& prepass = fp->AddDepthPrepass(rg, depthView,
			{ fc.extent.width, fc.extent.height }, "ForwardPlusDepth",
			seeded ? smvk::AttachmentLoadOp::Load
				: smvk::AttachmentLoadOp::Clear);
		prepass.SetRecordingFunc([this, depthPC, world, lodEnabled, lodInputs, staticMask, mainView, hasMainView](smvk::CommandBuffer& cmd)
		{
			// GPU context + pass scope come from RenderGraph::Execute.
			auto drawPrepassNode = [&](const smvk::World::RenderItem& item, bool maskedPhase) {
				// Masked (cutout) flag replaces the per-node material
				// fetch the phase split used to pay. Material-less meshes
				// carry no flag and stay in the solid phase (drawn, like
				// the old mat == nullptr path).
				if (((item.flags & smvk::IteratorFlagBit_Masked) != 0) != maskedPhase)
					return;
				// Static frustum cull: O(1) mask lookup from the single
				// frustumCullBVH walk at Append time (dynamic nodes are
				// always 1 here and get sphere-tested below).
				if (staticMask && item.node < staticMask->size()
					&& (*staticMask)[item.node] == 0)
					return;
				if (item.node >= world->scene.globalTransform.size())
					return;
				auto& model = world->scene.globalTransform[item.node];
				// Dynamic frustum cull: conservative LOD-union sphere test
				// against the guarded main-view planes (mirrors
				// GfxRenderer::FilterMainView). Keep the instance when
				// EITHER the base or the selected LOD sibling intersects so
				// halfrate LOD flips can never open a 1-frame depth hole.
				if (hasMainView
					&& (item.flags & smvk::IteratorFlagBit_Static) == 0) {
					if (smvk::StaticMesh* baseDesc = world->GetMesh(item.mesh)) {
						glm::vec3 c{ 0.0f };
						float r = 0.0f;
						smvk::MeshBoundsToWorld(model, baseDesc->geometry.aabbMin,
							baseDesc->geometry.aabbMax, c, r);
						if (!smvk::RenderViewIntersectsSphere(mainView, c, r)) {
							bool visible = false;
							if (lodEnabled) {
								const uint32_t lod = world->LodIndexForNode(
									item.node, item.mesh, lodInputs);
								if (smvk::StaticMesh* selDesc =
									world->GetLodMesh(item.mesh, lod);
									selDesc && selDesc != baseDesc) {
									smvk::MeshBoundsToWorld(model,
										selDesc->geometry.aabbMin,
										selDesc->geometry.aabbMax, c, r);
									visible = smvk::RenderViewIntersectsSphere(
										mainView, c, r);
								}
							}
							if (!visible)
								return;
						}
					}
				}
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
			// No Phase 3: transparent excluded by design (see header note).
		});
	}
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
	fp->Ensure(ctx, rg, { fc.extent.width, fc.extent.height }, desc, fc.depthViewHandle);
	if (!fp->Valid())
		return;

	// Snapshots for the recording lambda (recorded now, executed later at
	// Execute(); nothing here may reference the frame-local fc).
	smvk::World* world = fc.world;
	const PushConstants depthPC = fc.pc;
	const float minPixels = occluderMinPixels;
	const float minRadius = occluderMinRadius;
	const glm::vec3 eye = fc.eye;
	const smvk::LodFrameInputs lodInputs = fc.lodInputs;
	const float viewportHeight = fc.lodInputs.viewportHeight > 0.0f
		? fc.lodInputs.viewportHeight : (float)fc.extent.height;
	const float tanHalfFov = std::tan(std::max(fc.lodInputs.fovY, 1e-3f) * 0.5f);

	// CPU frustum culling mirrors the full prepass (conservative static
	// mask + dynamic LOD-union sphere test): skipping a true occluder
	// only loses culling, but the mask walk is shared work anyway.
	smvk::RenderView mainView{};
	bool hasMainView = false;
	if (fc.views && fc.mainViewIndex != smvk::RenderViewSet::kInvalidIndex
		&& fc.mainViewIndex < fc.views->Count()) {
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
		auto& occluder = fp->AddDepthPrepass(rg, fc.hizDepthView,
			{ fc.extent.width, fc.extent.height }, "OccluderDepth",
			smvk::AttachmentLoadOp::Clear);
		occluder.SetRecordingFunc([this, depthPC, world, staticMask, mainView, hasMainView,
			minPixels, minRadius, eye, viewportHeight, tanHalfFov](smvk::CommandBuffer& cmd)
		{
			auto drawOccluderNode = [&](const smvk::World::RenderItem& item) {
				// Solid opaque only: skip MASK cutout (holes) entirely.
				if ((item.flags & smvk::IteratorFlagBit_Masked) != 0)
					return;
				if (staticMask && item.node < staticMask->size()
					&& (*staticMask)[item.node] == 0)
					return;
				if (!world || item.node >= world->scene.globalTransform.size())
					return;
				auto& model = world->scene.globalTransform[item.node];
				smvk::StaticMesh* baseDesc = world->GetMesh(item.mesh);
				if (!baseDesc)
					return;
				// World-space bounds (LOD0 = conservative union: LOD
				// siblings only shrink it, so the size test never admits
				// an item the full prepass would consider tiny).
				glm::vec3 c{ 0.0f };
				float r = 0.0f;
				smvk::MeshBoundsToWorld(model, baseDesc->geometry.aabbMin,
					baseDesc->geometry.aabbMax, c, r);
				if (!(r >= minRadius))
					return; // small geometry: not worth rasterizing
				if (hasMainView) {
					if (!smvk::RenderViewIntersectsSphere(mainView, c, r))
						return;
					// Projected bound-diameter heuristic: occluders below
					// ~minPixels cover too few HZB texels to cull with.
					const float dist = glm::length(c - eye);
					if (dist > 1e-3f && tanHalfFov > 1e-6f) {
						const float diamPx = (2.0f * r / dist)
							* (viewportHeight / (2.0f * tanHalfFov));
						if (!(diamPx >= minPixels))
							return;
					}
				}
				// Crudest LOD: cheapest raster for the silhouette. Safe
				// direction for HZB (conservative): simplifier siblings
				// shrink the surface, so occluder depth lands at or
				// behind the true surface -> fewer culls, never holes.
				// Missing siblings fall back to LOD0 via GetLodMesh.
				smvk::StaticMesh* meshDesc = baseDesc;
				const uint32_t chainLen = world->LodChainLength(item.mesh);
				if (chainLen > 1)
					meshDesc = world->GetLodMesh(item.mesh, chainLen - 1);
				if (!meshDesc)
					return;
				PushConstants objectPC{};
				std::copy_n(glm::value_ptr(model), 16, objectPC.model);
				cmd.PushConstants(offsetof(PushConstants, model), sizeof(objectPC.model), objectPC.model, smvk::ShaderStage::All);
				for (auto& draw_set : meshDesc->geometry.surfaces) {
					cmd.DrawIndexed(draw_set.indexCount, 1, draw_set.firstIndex, draw_set.baseVertex);
				};
			};
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
				, [&](const smvk::World::RenderItem& item) { drawOccluderNode(item); });
		});
	}
}

void ForwardPlusRenderer::AppendTileCull(smvk::FrameContext& base) {	AppFrameContext& fc = static_cast<AppFrameContext&>(base);
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
