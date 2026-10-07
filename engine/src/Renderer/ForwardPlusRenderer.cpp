#include <Engine/Rendergraph/ForwardPlusRenderer.h>
#include <Engine/Rendergraph/FrameContext.h>
#include <Engine/Renderer/Renderer.h>

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

namespace engine {

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

void ForwardPlusRenderer::AppendPrepass(smvk::FrameContext& base,
	bool fallbackOnly) {
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
	const IRenderer::PushConstants depthPC = fc.pc;
	const smvk::LogicalImageViewHandle depthView = fc.depthView;
	const bool lodEnabled = fc.lodEnabled;
	const smvk::LodFrameInputs lodInputs = fc.lodInputs;
	// Null on full-prepass frames (fallbackOnly stays false then).
	const std::function<bool(NodeId, uint32_t, uint32_t)> lodFilterable =
		fc.gfxLodFilterable;
	// SSSR-style thin G-buffer (see gbuffer.frag.hlsl): on when wanted AND
	// the programs loaded. On temporal-split frames (fallbackOnly) the
	// G-buffer Loads last frame's values — the seed pass draws the
	// filterable majority without a G-buffer FS, and static normals never
	// change (dynamic surfaces read one frame stale, invisible under the
	// temporal SSR blend); on full-prepass frames it Clears (fresh
	// coverage: the prepass draws every visible opaque surface).
	const bool gbufferOn = fc.ssrGbufferWanted
		&& gbufferProgram.shaderCount == 2
		&& gbufferMaskedProgram.shaderCount == 2
		&& !fc.gbufferView.empty();
	const smvk::LogicalImageViewHandle gbufferView = gbufferOn
		? fc.gbufferView : smvk::LogicalImageViewHandle{};
	const smvk::AttachmentLoadOp gbufferLoad = fallbackOnly
		? smvk::AttachmentLoadOp::Load
		: smvk::AttachmentLoadOp::Clear;

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
	// Optional color: the SSSR thin G-buffer (oct normal + roughness +
	// metal, RGBA8 — gbuffer.frag.hlsl). Solid geometry uses the VS-only
	// fast path (or the G-buffer FS pair when on); MASK cutout uses the
	// alpha-tested program so holes keep background depth (the G-buffer
	// masked variant discards holes out of the G-buffer the same way).
	// Transparent (Blend) is intentionally skipped — see above.
	{
		auto& prepass = fp->AddDepthPrepass(rg, depthView,
			{ fc.extent.width, fc.extent.height }, "ForwardPlusDepth",
			seeded ? smvk::AttachmentLoadOp::Load
				: smvk::AttachmentLoadOp::Clear,
			gbufferView, gbufferLoad);
		prepass.SetRecordingFunc([this, depthPC, world, lodEnabled, lodInputs,
			staticMask, mainView, hasMainView, fallbackOnly, lodFilterable,
			gbufferOn](smvk::CommandBuffer& cmd)
		{
			// GPU context + pass scope come from RenderGraph::Execute.
			// Phase 2 masked pipeline: cull follows the material raster
			// (single-sided cutout must not write backface depth the
			// shaded pass never draws; double-sided keeps Cull None).
			// boundMaskedCull starts at a never-bound sentinel; rebinds
			// only on cull changes and the CommandBuffer dedupes identical
			// pipeline states, so the common all-double-sided scene pays
			// one bind. G-buffer on: the masked pair switches to
			// gbufferMaskedProgram (same alpha test + the G-buffer
			// output).
			smvk::CullMode boundMaskedCull = smvk::CullMode::FrontAndBack;
			auto ensureMaskedPipeline = [&](smvk::CullMode cull) {
				if (cull == boundMaskedCull)
					return;
				cmd.BindRenderPipeline(
					gbufferOn ? gbufferMaskedProgram : prepassMaskedProgram,
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
				cmd.PushConstants(0, sizeof(IRenderer::PushConstants), &depthPC, smvk::ShaderStage::All);
				boundMaskedCull = cull;
			};
			auto drawPrepassNode = [&](const smvk::World::RenderItem& item, bool maskedPhase) {
				// Masked (cutout) flag replaces the per-node material
				// fetch the phase split used to pay. Material-less meshes
				// carry no flag and stay in the solid phase (drawn, like
				// the old mat == nullptr path).
				if (((item.flags & smvk::IteratorFlagBit_Masked) != 0) != maskedPhase)
					return;
				// Fallback-only mode (Northlight depth split): the gfx
				// filter's temporal set owns this item's depth — the seed
				// pass covers its prev∩curr clusters and the remainder
				// pass the newly visible curr−prev ones (both drawn with
				// the proven CPU-command machinery, each cluster once).
				// This walk only covers the unfilterable rest: unregistered
				// meshes/LOD levels, unknown-material clusters and shader-LOD
				// vertex-lit instances. MASKED items are NOT exempted: the
				// seed/remainder masked draws run the real alpha test with
				// material cull (see RecordDepthList), and re-walking the
				// full masked set here would double the alpha-tested raster
				// the split exists to avoid.
				// Material-less meshes keep their defensive depth draw
				// (the filter never pushes them; nothing shades them).
				if (fallbackOnly && lodFilterable
					&& item.material != smvk::World::kInvalidIndex) {
					const uint32_t lod = lodEnabled
						? world->LodIndexForNode(item.node, item.mesh, lodInputs)
						: 0u;
					if (lodFilterable(item.node, item.mesh, lod))
						return;
				}
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
				// LOD flips can never open a 1-frame depth hole.
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
				// Decision cache: World::LodIndexForNode refreshes every
				// node per frame by default (halfrate parity opt-out) —
				// the prepass, main pass and filter all share one decision
				// per node per frame.
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
			IRenderer::PushConstants objectPC{};
			std::copy_n(glm::value_ptr(model), 16, objectPC.model);
			if (maskedPhase) {
				smvk::CullMode cull = smvk::CullMode::None;
				if (const smvk::Material* mat = world->GetMaterial(item.material))
					cull = mat->pipeline.raster.cullMode;
				ensureMaskedPipeline(cull);
			}
			// materialId: the masked FS always read it (alpha test); the
			// G-buffer solid FS reads it too (roughness/metal lookup).
			if (maskedPhase || gbufferOn) {
				objectPC.materialId = item.material;
				cmd.PushConstants(offsetof(IRenderer::PushConstants, materialId),
					sizeof(objectPC.materialId), &objectPC.materialId,
					smvk::ShaderStage::All);
			}
			cmd.PushConstants(offsetof(IRenderer::PushConstants, model), sizeof(objectPC.model), objectPC.model, smvk::ShaderStage::All);
			for (auto& draw_set : meshDesc->geometry.surfaces) {
				cmd.DrawIndexed(draw_set.indexCount, 1, draw_set.firstIndex, draw_set.baseVertex);
			};
		};
		// Phase 1: solid geometry. VS-only fast path (legacy) or the
		// G-buffer FS pair (SSSR: the trivial gbuffer.frag reads the
		// interpolated world normal + material factors and writes the
		// RGBA8 thin G-buffer — no textures, no lighting).
		cmd.BindRenderPipeline(
			gbufferOn ? gbufferProgram : fp->depthProgram,
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
		cmd.PushConstants(0, sizeof(IRenderer::PushConstants), &depthPC, smvk::ShaderStage::All);
		// Phase order: MOST EXPENSIVE FIRST. Phase 1 = MASK cutout
		// (alpha-tested FS: baseColor fetch + discard — the expensive
		// per-fragment work): it runs against empty depth, but masked
		// is the FRONT layer (foliage occludes, is rarely occluded) so
		// priming would buy it little anyway. Phase 2 = solid (the
		// trivial G-buffer FS / VS-only): its fragments BEHIND the
		// masked depth now fail GreaterEqual and die to early-Z before
		// the FS + ROP work — the phase that benefits from priming runs
		// last. Final depth/G-buffer content is order-independent
		// (front-most surviving fragment wins under GreaterEqual+write
		// either way).
		world->forEachRenderItem(
			smvk::IteratorFlagBit_Renderable
			| smvk::IteratorFlagBit_Opaque
			| smvk::IteratorFlagBit_StaticVertexFmt
			, [&](const smvk::World::RenderItem& item) { drawPrepassNode(item, true); });
			// Phase 2: solid geometry. The pipeline binds above (the
			// G-buffer FS pair or the VS-only legacy path); early-Z
			// reaps the masked priming (building walls behind foliage
			// skip the FS).
		world->forEachRenderItem(
			smvk::IteratorFlagBit_Renderable
			| smvk::IteratorFlagBit_Opaque
			| smvk::IteratorFlagBit_StaticVertexFmt
			, [&](const smvk::World::RenderItem& item) { drawPrepassNode(item, false); });
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
	const IRenderer::PushConstants depthPC = fc.pc;
	const float minPixels = occluderMinPixels;
	const float minRadius = occluderMinRadius;
	const glm::vec3 eye = fc.eye;
	const bool lodEnabled = fc.lodEnabled;
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
			minPixels, minRadius, eye, lodEnabled, lodInputs, viewportHeight, tanHalfFov](smvk::CommandBuffer& cmd)
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
				// SELECTED LOD: the occluder rasterizes the same surface
				// Main draws, so occluder coverage can never be nearer
				// than the rendered geometry. (The old crudest-LOD pick
				// relied on "simplifier siblings shrink the surface" —
				// meshopt does not guarantee that: error is two-sided, and
				// a coarse bulge in front of the drawn surface over-culls
				// the gap behind it.) Shares the per-node LOD decision
				// cache with every other consumer; missing siblings fall
				// back to LOD0 via GetLodMesh.
				smvk::StaticMesh* meshDesc = baseDesc;
				if (lodEnabled) {
					const uint32_t lod = world->LodIndexForNode(
						item.node, item.mesh, lodInputs);
					if (smvk::StaticMesh* sel = world->GetLodMesh(item.mesh, lod))
						meshDesc = sel;
				}
				if (!meshDesc)
					return;
				IRenderer::PushConstants objectPC{};
				std::copy_n(glm::value_ptr(model), 16, objectPC.model);
				cmd.PushConstants(offsetof(IRenderer::PushConstants, model), sizeof(objectPC.model), objectPC.model, smvk::ShaderStage::All);
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
			cmd.PushConstants(0, sizeof(IRenderer::PushConstants), &depthPC, smvk::ShaderStage::All);
			world->forEachRenderItem(
				smvk::IteratorFlagBit_Renderable
				| smvk::IteratorFlagBit_Opaque
				| smvk::IteratorFlagBit_StaticVertexFmt
				, [&](const smvk::World::RenderItem& item) { drawOccluderNode(item); });
		});
	}
}

void ForwardPlusRenderer::AppendTileCull(smvk::FrameContext& base,
	uint32_t lightCountOverride) {
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
		cullPC.volumeMasks = fp->VolumeTileMasksAddress(ctx);
		cullPC.depthTextureIndex = fp->depthBindlessIndex;
		cullPC.tileCountX = fp->tileCountX;
		cullPC.tileCountY = fp->tileCountY;
		cullPC.lightCount = (lightCountOverride != UINT32_MAX)
			? lightCountOverride : fc.fpLightCount;
		cullPC.tileSize = desc.tileSize;
		cullPC.maxLightsPerTile = desc.maxLightsPerTile;
		auto& cullPass = fp->AddCullPass(rg, fc.depthView, fc.fpLights,
			"ForwardPlusCull", fc.decals);
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

} // namespace engine
