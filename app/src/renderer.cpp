#include <App/renderer.h>
#include <App/frame_context.h>
#include <App/push_constants.h>

#include <Engine/Renderer/mesh.h>
#include <Engine/debug_log.h>
#include <Engine/platform.h>
#include <Engine/Profile/profile.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace app {

static constexpr const char* kGfxSeedVsPath = "content/shaders/gfxseed.vert.hlsl.spv";
static constexpr const char* kGfxSeedMaskedVsPath = "content/shaders/gfxseedmasked.vert.hlsl.spv";
static constexpr const char* kFilteredMotionVsPath = "content/shaders/gfxfilteredmotion.vert.hlsl.spv";
// Byte offset of the FrameUniforms BDA in the filtered-motion push (free
// prevModel scratch at 192..256: 192..216 is the 24B filtered VS tail,
// 216..224 carries frameUniforms; the motion FS never reads prevModel).
static constexpr uint32_t kFilteredMotionFrameUniformsOffset = 216;
static constexpr const char* kMeshDebugVsPath = "content/shaders/meshdebug.vert.hlsl.spv";
static constexpr const char* kMeshDebugFragPath = "content/shaders/meshdebug.frag.hlsl.spv";
static constexpr const char* kLineVsPath = "content/shaders/debugline.vert.hlsl.spv";
static constexpr const char* kLineFragPath = "content/shaders/debugline.frag.hlsl.spv";

void GfxRenderer::DebugClusterColor(uint32_t cluster, float coneCutoff, int mode, float outRgba[4]) {
	if (mode == 1) {
		// Cone heat: cutoff ~1 (degenerate/wide, never culls) -> red,
		// tight useful cones -> green/cyan.
		float t = std::clamp((coneCutoff + 1.0f) * 0.5f, 0.0f, 1.0f);
		outRgba[0] = t;
		outRgba[1] = 1.0f - std::abs(t - 0.5f) * 2.0f * 0.5f;
		outRgba[2] = 1.0f - t;
		outRgba[3] = 1.0f;
		return;
	}
	// Stable hash hue per cluster (golden-ratio scramble), full saturation.
	uint32_t h = cluster * 2654435761u;
	h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
	const float hue = (h % 1024) / 1024.0f;
	const float s = 0.85f, v = 0.95f;
	const float c = v * s;
	const float hh = hue * 6.0f;
	const float x = c * (1.0f - std::fabs(std::fmod(hh, 2.0f) - 1.0f));
	float r = 0.0f, g = 0.0f, b = 0.0f;
	if (hh < 1.0f) { r = c; g = x; }
	else if (hh < 2.0f) { r = x; g = c; }
	else if (hh < 3.0f) { g = c; b = x; }
	else if (hh < 4.0f) { g = x; b = c; }
	else if (hh < 5.0f) { r = x; b = c; }
	else { r = c; b = x; }
	const float m = v - c;
	outRgba[0] = r + m; outRgba[1] = g + m; outRgba[2] = b + m; outRgba[3] = 1.0f;
}

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
	// Debug programs (self-contained; failure only disables debug views).
	if (meshDebugProgram.shaderCount == 0 || lineProgram.shaderCount == 0) {
		try {
			if (meshDebugVsModule.empty())
				meshDebugVsModule = ctx.CreateShaderModule(
					readFile(kMeshDebugVsPath), "meshDebugVS");
			if (meshDebugFragModule.empty())
				meshDebugFragModule = ctx.CreateShaderModule(
					readFile(kMeshDebugFragPath), "meshDebugFS");
			if (lineVsModule.empty())
				lineVsModule = ctx.CreateShaderModule(
					readFile(kLineVsPath), "debugLineVS");
			if (lineFragModule.empty())
				lineFragModule = ctx.CreateShaderModule(
					readFile(kLineFragPath), "debugLineFS");
		}
		catch (...) {
			return true; // filter works; debug views stay off
		}
		if (!meshDebugVsModule.empty() && !meshDebugFragModule.empty()) {
			smvk::Shader vs{ .module = meshDebugVsModule, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex };
			smvk::Shader fs{ .module = meshDebugFragModule, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment };
			meshDebugProgram = { .shaders = { vs, fs }, .shaderCount = 2 };
		}
		if (!lineVsModule.empty() && !lineFragModule.empty()) {
			smvk::Shader vs{ .module = lineVsModule, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex };
			smvk::Shader fs{ .module = lineFragModule, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment };
			lineProgram = { .shaders = { vs, fs }, .shaderCount = 2 };
		}
	}
	return true;
}

void GfxRenderer::RecordDepthList(smvk::CommandBuffer& cmd, smvk::ResourceManager& ctx,
	uint32_t slot, bool remainder, uint64_t drawsAddr,
	uint64_t renderViewsAddr, uint32_t renderViewIndex,
	const PushConstants& depthPC) {
	const std::vector<smvk::gfx::SeedRange>& ranges = remainder
		? smvk::gfx::RemainderRanges(gfx, slot)
		: smvk::gfx::SeedRanges(gfx, slot);
	const uint32_t opaqueCount = remainder
		? smvk::gfx::RemainderOpaqueCommands(gfx, slot)
		: smvk::gfx::SeedOpaqueCommands(gfx, slot);
	const smvk::BufferHandle indirectBuf = remainder
		? smvk::gfx::RemainderIndirectBuffer(gfx, slot)
		: smvk::gfx::SeedIndirectBuffer(gfx, slot);
	cmd.BindStaticVertexFmt();
	// NOTE: no push constants before the first BindRenderPipeline below —
	// PushConstants asserts on a bound pipeline layout (impl.cpp).
	// No index binding anywhere here: mesh, cluster-ordered and filtered
	// index regions share the static index monoblock with absolute
	// firstIndex, so the offset-0 binding from BindStaticVertexFmt (run by
	// every BindRenderPipeline) is correct for every draw below.
	// Phase 1: opaque (VS-only, back-culled like the prepass).
	if (opaqueCount > 0) {
		cmd.BindRenderPipeline(
			seedOpaqueProgram,
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
		PushSeedConstants(cmd, ctx, slot, renderViewsAddr, renderViewIndex, drawsAddr);
		cmd.DrawIndexedIndirect(indirectBuf, 0, opaqueCount, 20);
	}
	// Phase 2: MASK cutout (alpha-tested FS, cull off).
	for (size_t ri = 0; ri < ranges.size(); ++ri) {
		const smvk::gfx::SeedRange& r = ranges[ri];
		if ((r.drawSet & smvk::gfx::DrawSetBit_Masked) == 0 || r.commandCount == 0)
			continue;
		if (r.material == UINT32_MAX)
			continue; // unknown material: no valid materialId push;
			// the full prepass still depths these clusters (T3)
		cmd.BindRenderPipeline(
			seedMaskedProgram,
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
		PushSeedConstants(cmd, ctx, slot, renderViewsAddr, renderViewIndex, drawsAddr);
		if (remainder)
			DrawRemainderRange(cmd, slot, (uint32_t)ri);
		else
			DrawSeedRange(cmd, slot, (uint32_t)ri);
	}
}

void GfxRenderer::AppendPasses(smvk::FrameContext& base) {
	AppFrameContext& fc = static_cast<AppFrameContext&>(base);
	lastSlot_ = UINT32_MAX;
	seededThisFrame_ = false;
	remainderPending_ = false;
	if (!fc.world || !fc.views || fc.mainViewIndex == smvk::RenderViewSet::kInvalidIndex)
		return;

	SMVK_PROFILE_RENDER("GfxAppendPasses");
	// NOTE: the seed pass below captures fc.pc (frame addresses for the
	// masked FS) by value, so main must call AppendPasses after fc.pc is
	// built but before the Forward+ prepass (seed writes depth first).
	// Debug culling freeze: batch expansion (+ the filter CS downstream)
	// runs against the override view/eye when set. Everything that writes
	// the live depth buffer (seed, remainder) deliberately stays on the
	// live main view.
	uint32_t cullView = fc.mainViewIndex;
	glm::vec3 cullEye = fc.eye;
	if (cullOverride_ && fc.views && cullView_ < fc.views->Count())
	{
		cullView = cullView_;
		cullEye = cullEye_;
	}
	lastSlot_ = FilterMainView(*fc.ctx, *fc.rg, fc.frameIndex, *fc.world,
		*fc.views, cullView, cullEye,
		{ fc.extent.width, fc.extent.height }, config.cullFlags,
		fc.lodEnabled ? &fc.lodInputs : nullptr);
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
	{
		SMVK_PROFILE_SCOPE("GfxEnsureHiz");
		smvk::gfx::EnsureHiz(gfx, *fc.ctx, *fc.rg, slot,
			fc.extent.width, fc.extent.height);
	}
	// Seed depth from last frame's visible set (prev* intact: rotation into
	// prev* happens below, after the seed block).
	if (smvk::gfx::Temporal(gfx, slot).historyValid
		&& seedOpaqueProgram.shaderCount != 0
		&& seedMaskedProgram.shaderCount != 0) {
		smvk::gfx::BuildSeedCommands(gfx, slot);
		smvk::gfx::UploadSeed(gfx, *fc.ctx, fc.frameIndex, slot);
		const auto& ranges = smvk::gfx::SeedRanges(gfx, slot);
		if (!ranges.empty()) {
			smvk::RenderPass& seedPass = smvk::gfx::AddSeedPass(
				gfx, *fc.rg, slot, fc.depthView);
			smvk::gfx::AddSeedConsumeHints(seedPass,
				smvk::gfx::IndexHeapLogical(gfx),
				smvk::gfx::VisDrawLogical(gfx, slot),
				smvk::gfx::SeedIndirectLogical(gfx, slot));
			GfxRenderer* self = this;
			smvk::ResourceManager* ctx = fc.ctx;
			const PushConstants depthPC = fc.pc;
			const uint64_t renderViewsAddr =
				fc.views->DeviceAddress(*fc.ctx, fc.frameIndex);
			const uint32_t renderViewIndex = fc.mainViewIndex;
			const uint64_t drawsAddr =
				ctx->GetBufferDeviceAddress(smvk::gfx::VisDrawBuffer(gfx, slot));
			seedPass.SetRecordingFunc([self, ctx, slot, depthPC, renderViewsAddr, renderViewIndex, drawsAddr](smvk::CommandBuffer& cmd)
			{
				// GPU context + pass scope come from RenderGraph::Execute.
				self->RecordDepthList(cmd, *ctx, slot, false, drawsAddr,
					renderViewsAddr, renderViewIndex, depthPC);
			});
			seededThisFrame_ = true;
		}
	}
	// Remainder difference (curr minus prev) for depth pass 2 (T5). Must
	// run before rotation overwrites prev* below. Only meaningful when the
	// seed primed depth (otherwise the full prepass covered everything).
	if (seededThisFrame_) {
		smvk::gfx::BuildRemainderCommands(gfx, slot);
		smvk::gfx::UploadRemainder(gfx, *fc.ctx, fc.frameIndex, slot);
		remainderPending_ = !smvk::gfx::RemainderRanges(gfx, slot).empty();
	}
	// Publish this frame's seed source, then rotate for next frame. Order
	// matters: UploadVisible must push the OLD snapshot (prev = last
	// frame) into the current ring slice BEFORE SnapshotVisible
	// overwrites prev with the current frame. The seed pass above reads
	// VisDrawBuffer(current slice), so uploading after the snapshot would
	// hand it the current draws under the previous frame's firstInstance
	// indices — wrong model matrices whenever the draw list churns
	// (camera moves, frustum changes), i.e. corrupt seed depth, broken
	// 1x Main (read-only depth) and wrong tile-cull min/max. Static
	// scenes hide it (prev == curr, same order).
	smvk::gfx::UploadVisible(gfx, *fc.ctx, fc.frameIndex, slot);
	smvk::gfx::SnapshotVisible(gfx, slot);
}

void GfxRenderer::AppendRemainderPass(smvk::FrameContext& base) {
	AppFrameContext& fc = static_cast<AppFrameContext&>(base);
	if (lastSlot_ == UINT32_MAX || !remainderPending_)
		return;
	const uint32_t slot = lastSlot_;
	smvk::RenderPass& pass = smvk::gfx::AddRemainderPass(gfx, *fc.rg, slot, fc.depthView);
	smvk::gfx::AddSeedConsumeHints(pass,
		smvk::gfx::IndexHeapLogical(gfx),
		smvk::gfx::DrawLogical(gfx, slot),
		smvk::gfx::RemainderIndirectLogical(gfx, slot));
	GfxRenderer* self = this;
	smvk::ResourceManager* ctx = fc.ctx;
	const PushConstants depthPC = fc.pc;
	const uint64_t renderViewsAddr =
		fc.views->DeviceAddress(*fc.ctx, fc.frameIndex);
	const uint32_t renderViewIndex = fc.mainViewIndex;
	const uint64_t drawsAddr =
		ctx->GetBufferDeviceAddress(smvk::gfx::DrawBuffer(gfx, slot));
pass.SetRecordingFunc([self, ctx, slot, depthPC, renderViewsAddr, renderViewIndex, drawsAddr](smvk::CommandBuffer& cmd)
	{
		// GPU context + pass scope come from RenderGraph::Execute.
		self->RecordDepthList(cmd, *ctx, slot, true, drawsAddr,
			renderViewsAddr, renderViewIndex, depthPC);
	});
	// Counts ride on the [GfxFilter] hist: line (FilterMainView).
}

void GfxRenderer::AppendFilterDispatch(smvk::FrameContext& fc) {
	if (lastSlot_ == UINT32_MAX)
		return;
	const uint32_t slot = lastSlot_;

	SMVK_PROFILE_RENDER("GfxAppendFilterDispatch");
	SMVK_PROFILE_TAG("slot", slot);

	smvk::RenderPass& pass = AddFilterPass(*fc.rg, slot);
	GfxRenderer* self = this;
	smvk::ResourceManager* ctx = fc.ctx;
	pass.SetRecordingFunc([self, ctx, slot](smvk::CommandBuffer& cmd)
	{
		// GPU context + pass scope come from RenderGraph::Execute.
		smvk::gfx::PushAndDispatch(self->gfx, cmd, *ctx, slot);
	});
}

void GfxRenderer::AppendHizPasses(smvk::FrameContext& base,
	smvk::LogicalImageViewHandle depthLogical, uint32_t depthBindlessIndex,
	const char* passName) {
	AppFrameContext& fc = static_cast<AppFrameContext&>(base);
	if (lastSlot_ == UINT32_MAX || lastSlot_ >= smvk::gfx::ViewCount(gfx))
		return;
	const uint32_t slot = lastSlot_;
	const smvk::gfx::TemporalView& t = smvk::gfx::Temporal(gfx, slot);
	// Current flight's pyramid (same rule as every other ringed buffer;
	// slices can disagree briefly across a resize before all rebuild).
	if (t.hiz[fc.frameIndex].mipCount == 0)
		return;

	SMVK_PROFILE_RENDER("GfxAppendHizPasses");
	SMVK_PROFILE_TAG("mipCount", t.hiz[fc.frameIndex].mipCount);

	GfxRenderer* self = this;
	smvk::ResourceManager* ctx = fc.ctx;
	for (uint32_t m = 0; m < t.hiz[fc.frameIndex].mipCount; ++m) {
		const bool fromDepth = (m == 0);
		smvk::RenderPass& hizPass = smvk::gfx::AddHizPass(gfx, *fc.rg, slot, m,
			fromDepth ? depthLogical : smvk::LogicalImageViewHandle{},
			passName ? passName : "GfxHiz");
		hizPass.SetRecordingFunc([self, ctx, slot, m, depthBindlessIndex, fromDepth](smvk::CommandBuffer& cmd)
		{
			// Per-mip GPU scope (the pass-level scope uses the shared
			// "GfxHiz" name). Static literals: Optick stores the name
			// pointer, so run-time names must have static storage.
			static const char* const kHizMipGpuNames[16] = {
				"GfxHizMip0", "GfxHizMip1", "GfxHizMip2", "GfxHizMip3",
				"GfxHizMip4", "GfxHizMip5", "GfxHizMip6", "GfxHizMip7",
				"GfxHizMip8", "GfxHizMip9", "GfxHizMip10", "GfxHizMip11",
				"GfxHizMip12", "GfxHizMip13", "GfxHizMip14", "GfxHizMip15",
			};
			::smvk::profiler::GpuPassScope hizMipScope(
				kHizMipGpuNames[m < 16 ? m : 15], __FILE__, __LINE__);
			SMVK_PROFILE_TAG("mip", m);
			smvk::gfx::PushAndDispatchHiz(self->gfx, cmd, *ctx, slot, m,
				fromDepth ? depthBindlessIndex : UINT32_MAX);
		});
	}
}

void GfxRenderer::RegisterWorldMeshes(smvk::World& world) {
	meshMap_.assign(world.meshLocations.size(), UINT32_MAX);
	lodMeshMap_.clear();
	lodMeshMap_.resize(world.meshLocations.size());
	uint32_t fromCooked = 0, fromUpload = 0, skipped = 0;
	uint64_t skippedTris = 0;
	// Dedupe: distinct base meshes can share one LOD sibling URI, and the
	// same base mesh is never registered twice. Key = owning cluster +
	// pool index (stable for the register pass; all clusters are alive).
	struct LocKey {
		const void* cluster = nullptr;
		uint32_t index = 0;
		bool operator==(const LocKey& o) const {
			return cluster == o.cluster && index == o.index;
		}
	};
	struct LocHash {
		size_t operator()(const LocKey& k) const noexcept {
			return std::hash<const void*>{}(k.cluster) * 31u + k.index;
		}
	};
	std::unordered_map<LocKey, uint32_t, LocHash> done;
	auto registerOne = [&](const smvk::World::MeshLocation& loc) -> uint32_t {
		if (!loc.cluster || !loc.handle.valid())
			return UINT32_MAX;
		const LocKey key{ loc.cluster, loc.handle.index() };
		if (auto it = done.find(key); it != done.end())
			return it->second;
		const smvk::StaticMesh* mesh = loc.cluster->staticMeshes.get(loc.handle);
		if (!mesh || mesh->geometry.surfaces.empty())
			return UINT32_MAX;
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
		if (gfxMesh == UINT32_MAX)
			skippedTris += mesh->geometry.indexCount / 3;
		done.emplace(key, gfxMesh);
		return gfxMesh;
	};
	// Every LOD sibling gets its own gfx mesh so FilterMainView can push
	// the selected LOD into the HZB-culled indirect ranges (previously
	// only LOD0 was registered and LOD>0 drew direct, unculled).
	for (size_t i = 0; i < world.meshLocations.size(); ++i) {
		const uint32_t chainLen = world.LodChainLength((uint32_t)i);
		if (chainLen <= 1) {
			const uint32_t g = registerOne(world.meshLocations[i]);
			if (g == UINT32_MAX)
				++skipped;
			meshMap_[i] = g;
			lodMeshMap_[i] = { g };
			continue;
		}
		std::vector<uint32_t> perLod;
		perLod.reserve(chainLen);
		for (uint32_t l = 0; l < chainLen; ++l) {
			auto hit = world.ResolveLodMesh((uint32_t)i, l);
			const uint32_t g = hit ? registerOne(*hit) : UINT32_MAX;
			if (g == UINT32_MAX)
				++skipped;
			perLod.push_back(g);
		}
		meshMap_[i] = perLod.empty() ? UINT32_MAX : perLod[0];
		lodMeshMap_[i] = std::move(perLod);
	}
	SMVK_DEBUG_LOG(
		"[GfxFilter] meshes: %u cooked + %u runtime-cooked = %u filtered, %u direct (%llu tris)\n",
		fromCooked, fromUpload, fromCooked + fromUpload, skipped,
		(unsigned long long)skippedTris);
	SMVK_DEBUG_LOG(
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
	smvk::gfx::CullFlags flags, const smvk::LodFrameInputs* lod) {

	SMVK_PROFILE_RENDER("GfxFilterMainView");
	SMVK_PROFILE_TAG("extentWidth", extent.width);
	SMVK_PROFILE_TAG("extentHeight", extent.height);

	smvk::gfx::BeginFrame(gfx);
	if (mainViewIdx == smvk::RenderViewSet::kInvalidIndex)
		return UINT32_MAX;
	const smvk::RenderView& view = views.Get(mainViewIdx);
	const uint32_t slot = smvk::gfx::BeginView(gfx, ctx, rg, view, eye,
		flags, extent.width, extent.height);
	if (slot == UINT32_MAX)
		return UINT32_MAX;

	SMVK_PROFILE_SCOPE("GfxFilterPushDraws");
	world.forEachRenderItem(
		smvk::IteratorFlagBit_Renderable
		| smvk::IteratorFlagBit_Opaque
		| smvk::IteratorFlagBit_StaticVertexFmt,
		[&](const smvk::World::RenderItem& item) {
			if (item.material == smvk::World::kInvalidIndex)
				return;
			const uint32_t worldMesh = item.mesh;
			if (worldMesh >= meshMap_.size())
				return; // unknown mesh: legacy direct path still draws it
			const glm::mat4& model = world.scene.globalTransform[item.node];
			// LOD-aware push (World::LodIndexForNode shares the halfrate
			// cache with every direct-draw consumer, so both sides agree
			// per node per frame): the selected sibling's gfx mesh goes
			// into the HZB-culled indirect ranges. Only a sibling whose
			// registration failed falls through to the direct fallback,
			// so each instance still rasterizes exactly once.
			uint32_t lodIdx = 0;
			if (lod && world.LodChainLength(worldMesh) > 1)
				lodIdx = world.LodIndexForNode(item.node, worldMesh, *lod);
			uint32_t gfxMesh = UINT32_MAX;
			if (worldMesh < lodMeshMap_.size()
				&& lodIdx < lodMeshMap_[worldMesh].size())
				gfxMesh = lodMeshMap_[worldMesh][lodIdx];
			else if (worldMesh < meshMap_.size())
				gfxMesh = meshMap_[worldMesh];
			if (gfxMesh == UINT32_MAX)
				return; // that LOD not filterable: direct path draws it
			// Mesh-level sphere pre-cull on CONSERVATIVE bounds: LOD
			// siblings may have tighter/smaller AABBs than LOD0, so
			// culling on the selected sibling alone drops instances the
			// direct path skips (it sees GfxMeshForLod != UINT32_MAX) and
			// neither path draws -> 1-frame holes while the camera moves
			// and the halfrate cache flips siblings. Keep the instance
			// when EITHER the base or the selected sibling intersects;
			// per-cluster GPU culling still rejects truly hidden meshlets.
			{
				smvk::StaticMesh* baseDesc = world.GetMesh(worldMesh);
				smvk::StaticMesh* selDesc = world.GetLodMesh(worldMesh, lodIdx);
				bool baseHit = true, selHit = true;
				glm::vec3 baseC{ 0.0f }; float baseR = 0.0f;
				glm::vec3 selC{ 0.0f }; float selR = 0.0f;
				if (baseDesc) {
					smvk::MeshBoundsToWorld(model, baseDesc->geometry.aabbMin,
						baseDesc->geometry.aabbMax, baseC, baseR);
					baseHit = smvk::RenderViewIntersectsSphere(view, baseC, baseR);
				}
				if (selDesc && selDesc != baseDesc) {
					smvk::MeshBoundsToWorld(model, selDesc->geometry.aabbMin,
						selDesc->geometry.aabbMax, selC, selR);
					selHit = smvk::RenderViewIntersectsSphere(view, selC, selR);
				}
				else if (selDesc) {
					selC = baseC; selR = baseR; selHit = baseHit;
				}
				if (!baseHit && !selHit)
					return;
				if (baseHit != selHit) {
					static uint64_t s_lodBoundsDisagree = 0;
					if ((++s_lodBoundsDisagree % 512) == 1) {
						SMVK_DEBUG_LOG("[GfxFilter] LOD bounds disagree mesh=%u lod=%u baseHit=%d selHit=%d\n",
							worldMesh, lodIdx, baseHit ? 1 : 0, selHit ? 1 : 0);
					}
				}
			}
			// Double-sided materials bind with Cull None (see
			// bindForMaterial): the filter must not backface/cone-cull
			// them or foliage/cutout backs vanish. The draw-set bucket
			// (opaque vs. alpha-tested) selects the seed depth PSO in T3.
			smvk::gfx::DrawFlags drawFlags = 0;
			smvk::gfx::DrawSetMask drawSet = smvk::gfx::DrawSetBit_Opaque;
			if (const smvk::Material* mat = world.GetMaterial(item.material)) {
				if (mat->isDoubleSided())
					drawFlags |= smvk::gfx::DrawFlagBit_TwoSided;
				if (mat->shadingModel == smvk::ShadingModel::Masked)
					drawSet = smvk::gfx::DrawSetBit_Masked;
			}
			smvk::gfx::PushDraw(gfx, slot, gfxMesh, model,
				item.material, drawFlags, drawSet);
		});
	const smvk::gfx::Stats stats = smvk::gfx::FinalizeView(gfx, ctx, frameIndex, slot);
	// NOTE: unconditional (not SMVK_DEBUG_LOG): this is the only per-run
	// visibility into filter load (draws/batches vs kMaxDraws/kMaxBatches).
	if ((frameCounter_++ % 300) == 0) {
		std::fprintf(stderr,
			"[GfxFilter] view: %u draws, %u batches, %u tris processed, %u/%u clusters culled\n",
			stats.draws, stats.commands, stats.trianglesProcessed,
			stats.clustersCulled, stats.clustersProcessed);
		// Temporal dashboard: prev* holds LAST frame's snapshot here (this
		// frame's rotation runs later in AppendPasses), so the seed /
		// remainder / HZB figures describe what the temporal loop did.
		const smvk::gfx::TemporalView& ht = smvk::gfx::Temporal(gfx, slot);
		const smvk::gfx::HizResources& hh = ht.hiz[frameIndex];
		SMVK_DEBUG_LOG(
			"[GfxFilter] view: %u draws, %u batches, %u tris processed, %u/%u clusters culled\n",
			stats.draws, stats.commands, stats.trianglesProcessed,
			stats.clustersCulled, stats.clustersProcessed);
		SMVK_DEBUG_LOG(
			"[GfxFilter] hist: valid=%d prev=%u seed=%u/%u rem=%u/%u hiz=%umips %ux%u minContrib=%.2f\n",
			ht.historyValid ? 1 : 0, ht.prevBatchCount,
			(unsigned)ht.seedCommands.size(), (unsigned)ht.seedRanges.size(),
			(unsigned)ht.remainderCommands.size(), (unsigned)ht.remainderRanges.size(),
			hh.mipCount, hh.width, hh.height, ht.minimumContribution);
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

bool GfxRenderer::EnsureFilteredMotion(smvk::ResourceManager& ctx,
	smvk::ShaderModuleHandle motionFrag) {
	if (FilteredMotionValid())
		return true;
	if (motionFrag.empty())
		return false;
	try {
		if (filteredMotionVsModule.empty())
			filteredMotionVsModule = ctx.CreateShaderModule(
				readFile(kFilteredMotionVsPath), "gfxFilteredMotionVS");
	}
	catch (...) {
		return false;
	}
	if (filteredMotionVsModule.empty())
		return false;
	smvk::Shader vs{ .module = filteredMotionVsModule, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex };
	smvk::Shader fs{ .module = motionFrag, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment };
	filteredMotionProgram = { .shaders = { vs, fs }, .shaderCount = 2 };
	return true;
}

void GfxRenderer::PushMotionViewConstants(smvk::CommandBuffer& cmd, smvk::ResourceManager& ctx,
	uint32_t slot, uint64_t renderViewsAddr, uint32_t renderViewIndex,
	uint64_t frameUniformsAddr) {
	PushViewConstants(cmd, ctx, slot, renderViewsAddr, renderViewIndex);
	cmd.PushConstants(kFilteredMotionFrameUniformsOffset, sizeof(frameUniformsAddr),
		&frameUniformsAddr, smvk::ShaderStage::All);
}

void GfxRenderer::PushSeedConstants(smvk::CommandBuffer& cmd, smvk::ResourceManager& ctx,
	uint32_t slot, uint64_t renderViewsAddr, uint32_t renderViewIndex,
	uint64_t drawsAddr) {
	(void)ctx;
	(void)slot;
	smvk::gfx::FilteredVsPush tail{};
	tail.draws = drawsAddr;
	tail.renderViews = renderViewsAddr;
	tail.renderViewIndex = renderViewIndex;
	cmd.PushConstants(smvk::gfx::kFilteredVsPushOffset, sizeof(tail), &tail,
		smvk::ShaderStage::All);
}

void GfxRenderer::DrawRemainderRange(smvk::CommandBuffer& cmd, uint32_t slot, uint32_t rangeIdx) {
	const std::vector<smvk::gfx::SeedRange>& ranges =
		smvk::gfx::RemainderRanges(gfx, slot);
	assert(rangeIdx < ranges.size());
	const smvk::gfx::SeedRange& r = ranges[rangeIdx];
	if (r.commandCount == 0)
		return;
	// No index bind: seed commands address the static index monoblock
	// absolutely (offset-0 binding from BindStaticVertexFmt is correct).
	cmd.DrawIndexedIndirect(smvk::gfx::RemainderIndirectBuffer(gfx, slot),
		(uint64_t)r.firstCommand * 20, r.commandCount, 20);
}

void GfxRenderer::DrawSeedRange(smvk::CommandBuffer& cmd, uint32_t slot, uint32_t rangeIdx) {
	const std::vector<smvk::gfx::SeedRange>& ranges =
		smvk::gfx::SeedRanges(gfx, slot);
	assert(rangeIdx < ranges.size());
	const smvk::gfx::SeedRange& r = ranges[rangeIdx];
	if (r.commandCount == 0)
		return;
	// No index bind: see DrawRemainderRange.
	cmd.DrawIndexedIndirect(smvk::gfx::SeedIndirectBuffer(gfx, slot),
		(uint64_t)r.firstCommand * 20, r.commandCount, 20);
}

void GfxRenderer::DumpIndirectAudit(smvk::ResourceManager& ctx, uint32_t slot) {
	using namespace smvk::gfx;
	if (slot >= gfx.views.size() || slot >= gfx.temporals.size()) {
		SMVK_DEBUG_LOG( "[GfxAudit] bad slot %u.\n", slot);
		return;
	}
	const View& v = gfx.views[slot];
	const TemporalView& t = gfx.temporals[slot];
	// Audit the current flight's HZB slice (same rule as every other ringed
	// buffer: the GPU wrote this frame's slice).
	const smvk::gfx::HizResources& hz = t.hiz[v.frameIndex];
	if (v.batchCount == 0 || v.batches.empty()) {
		SMVK_DEBUG_LOG( "[GfxAudit] slot %u: no batches.\n", slot);
		return;
	}
	// GPU indirect commands (one per batch; zero indexCount = GPU kill).
	std::vector<SeedCommand> cmds(v.batchCount);
	if (!ctx.DownloadDataFromBuffer(IndirectBuffer(gfx, slot),
			cmds.data(), cmds.size() * sizeof(SeedCommand))) {
		SMVK_DEBUG_LOG( "[GfxAudit] slot %u: indirect download failed.\n", slot);
		return;
	}
	// GPU HZB pyramid for the CPU mirror.
	std::vector<float> hzb;
	bool haveHzb = false;
	if (hz.mipCount > 0 && hz.width > 0 && hz.height > 0 &&
		!hz.buffer.empty()) {
		hzb.assign(HzbTotalFloats(hz.width, hz.height), 0.0f);
		haveHzb = ctx.DownloadDataFromBuffer(hz.buffer,
			hzb.data(), hzb.size() * sizeof(float));
	}
	// Material per batch (ranges span batches in order).
	std::vector<uint32_t> batchMat(v.batches.size(), UINT32_MAX);
	for (const MaterialRange& r : v.materialRanges) {
		for (uint32_t k = 0; k < r.commandCount; ++k) {
			if (r.firstCommand + k < (uint32_t)batchMat.size())
				batchMat[r.firstCommand + k] = r.material;
		}
	}
	SMVK_DEBUG_LOG(
		"[GfxAudit] slot %u: batches=%u flags=0x%x hiz=%ux%u/%u haveHzb=%d\n",
		slot, v.batchCount, v.cullFlags,
		hz.width, hz.height, hz.mipCount, haveHzb ? 1 : 0);
	uint32_t zeroTotal = 0, cpuOcc = 0, cpuKeep = 0;
	uint32_t printedOcc = 0, printedKeep = 0;
	// Per-class budgets: occlusion-agreeing kills used to hide behind the
	// single 48-line cap (the first kills are usually off-screen frustum
	// kills), making occlusion look permanently silent. Occluded and keep
	// kills each get their own budget so both classes stay visible.
	static constexpr uint32_t kAuditPrintPerClass = 24;
	static constexpr uint32_t kAuditMissCap = 16;
	// gfx mesh -> world mesh (first match) for traceability; -1 = none.
	// Lets a "mesh X still renders" report map to the scene graph.
	std::vector<int> gfxToWorld(gfx.meshes.size(), -1);
	for (uint32_t w = 0; w < (uint32_t)lodMeshMap_.size(); ++w) {
		for (uint32_t g : lodMeshMap_[w]) {
			if (g < gfxToWorld.size() && gfxToWorld[g] < 0)
				gfxToWorld[g] = (int)w;
		}
	}
	for (uint32_t w = 0; w < (uint32_t)meshMap_.size(); ++w) {
		const uint32_t g = meshMap_[w];
		if (g < gfxToWorld.size() && gfxToWorld[g] < 0)
			gfxToWorld[g] = (int)w;
	}
	struct MeshAudit {
		uint32_t batches = 0, tris = 0, gpuKill = 0;
		uint32_t occKill = 0, missed = 0, aliveTris = 0;
	};
	std::vector<MeshAudit> meshStats(gfx.meshes.size());
	struct MissDetail {
		uint32_t b, cl, draw, mesh, mat, tris, alive, level;
		float cx, cy, rx, ry, dn, stored;
		const char* screenTag;
	};
	std::vector<MissDetail> misses;
	misses.reserve(kAuditMissCap);
	for (uint32_t i = 0; i < v.batchCount && i < (uint32_t)v.batches.size(); ++i) {
		const Batch& b = v.batches[i];
		uint32_t mat = batchMat[i];
		uint32_t tris = 0, mesh = UINT32_MAX;
		float cx = 0.0f, cy = 0.0f, rx = 0.0f, ry = 0.0f, dn = 0.0f;
		float stored = -1.0f;
		uint32_t level = 0;
		bool mirrorOk = false, behind = false, verdict = false;
		if (b.draw < v.drawConstants.size() && b.cluster < gfx.clusters.size()) {
			const Draw& dc = v.drawConstants[b.draw];
			const Cluster& c = gfx.clusters[b.cluster];
			tris = c.triCount;
			mesh = dc.mesh;
			const float bs = std::max({
				glm::length(glm::vec3(dc.model[0])),
				glm::length(glm::vec3(dc.model[1])),
				glm::length(glm::vec3(dc.model[2])) });
			const glm::vec3 wc = glm::vec3(dc.model * glm::vec4(c.boundCenter, 1.0f));
			const float wr = c.boundRadius * bs;
			ClusterFootprint f = ProjectClusterFootprint(
				wc, wr, v.eye, v.viewProj, v.width, v.height);
			// Match the GPU jitter guard (GFX_CULL_GUARD_PX): the filter
			// widens the tested rect by a pixel each side, so the mirror
			// must too or every guarded keep reads as a CPU/GPU "miss".
			f.radiusX += 1.0f;
			f.radiusY += 1.0f;
			cx = f.centerX; cy = f.centerY; rx = f.radiusX; ry = f.radiusY;
			dn = f.nearestDepth;
			behind = f.behindCamera;
			if (!f.behindCamera && haveHzb)
				level = HzbMipForRect(rx * 2.0f, ry * 2.0f, hz.mipCount);
			mirrorOk = true;
			verdict = haveHzb && HzbOccluded(hzb.data(),
				hz.width, hz.height, hz.mipCount, f, &stored);
		}
		const bool killed = (cmds[i].indexCount == 0);
		if (killed) ++zeroTotal;
		if (verdict) ++cpuOcc; else ++cpuKeep;
		const int world = (mesh < gfxToWorld.size()) ? gfxToWorld[mesh] : -1;
		// Screen tag: separates occlusion hiding visible geometry from
		// pre-empting the frustum stages (same outcome there). Mirrors the
		// off-target condition in HzbOccluded.
		const char* screenTag = "unresolved";
		if (mirrorOk) {
			const float w = (float)v.width, h = (float)v.height;
			const bool offscreen = behind
				|| cx + rx < 0.0f || cx - rx > w || cy + ry < 0.0f || cy - ry > h;
			screenTag = behind ? "behindcam" : (offscreen ? "offscreen" : "screen");
		}
		// Per-mesh summary over kills AND survivors: answers "mesh X still
		// renders — what did the filter think of its batches?" missed =
		// CPU says occluded but the GPU kept the batch.
		if (mesh < meshStats.size()) {
			MeshAudit& ms = meshStats[mesh];
			++ms.batches;
			ms.tris += tris;
			if (killed) {
				++ms.gpuKill;
				if (verdict) ++ms.occKill;
			}
			else {
				ms.aliveTris += cmds[i].indexCount / 3;
				if (verdict) {
					++ms.missed;
					if (mirrorOk && misses.size() < kAuditMissCap) {
						MissDetail d{};
						d.b = i; d.cl = b.cluster; d.draw = b.draw;
						d.mesh = mesh; d.mat = mat; d.tris = tris;
						d.alive = cmds[i].indexCount / 3; d.level = level;
						d.cx = cx; d.cy = cy; d.rx = rx; d.ry = ry;
						d.dn = dn; d.stored = stored; d.screenTag = screenTag;
						misses.push_back(d);
					}
				}
			}
		}
		if (!killed)
			continue;
		uint32_t& budget = verdict ? printedOcc : printedKeep;
		if (budget < kAuditPrintPerClass) {
			++budget;
			if (verdict) {
				SMVK_DEBUG_LOG(
					"[GfxAudit] kill b=%u cl=%u draw=%u mesh=%u world=%d mat=%u tris=%u | "
					"c=(%.1f,%.1f) r=(%.1f,%.1f) mip=%u dNear=%.5f stored=%.5f "
					"margin=%+.5f %s cpu=OCCLUDED%s\n",
					i, b.cluster, b.draw, mesh, world, mat, tris, cx, cy, rx, ry,
					level, dn, stored, stored - dn, screenTag,
					mirrorOk ? "" : " (unresolved)");
			}
			else {
				SMVK_DEBUG_LOG(
					"[GfxAudit] kill b=%u cl=%u draw=%u mesh=%u world=%d mat=%u tris=%u | "
					"c=(%.1f,%.1f) r=(%.1f,%.1f) mip=%u dNear=%.5f stored=%.5f "
					"%s cpu=keep%s\n",
					i, b.cluster, b.draw, mesh, world, mat, tris, cx, cy, rx, ry,
					level, dn, stored, screenTag,
					mirrorOk ? "" : " (unresolved)");
			}
		}
	}
	// Misses first: CPU says occluded but the GPU kept the batch — the
	// exact "mesh behind still renders" cases, with margins.
	for (const MissDetail& d : misses) {
		SMVK_DEBUG_LOG(
			"[GfxAudit] miss b=%u cl=%u draw=%u mesh=%u mat=%u tris=%u aliveTris=%u | "
			"c=(%.1f,%.1f) r=(%.1f,%.1f) mip=%u dNear=%.5f stored=%.5f "
			"margin=%+.5f %s cpu=OCCLUDED-MISS\n",
			d.b, d.cl, d.draw, d.mesh, d.mat, d.tris, d.alive,
			d.cx, d.cy, d.rx, d.ry, d.level, d.dn, d.stored,
			d.stored - d.dn, d.screenTag);
	}
	if (misses.size() == kAuditMissCap)
		SMVK_DEBUG_LOG( "[GfxAudit] miss list capped at %u (more unprinted)\n",
			kAuditMissCap);
	uint32_t missedTotal = 0;
	for (uint32_t m = 0; m < (uint32_t)meshStats.size(); ++m) {
		const MeshAudit& ms = meshStats[m];
		if (ms.batches == 0)
			continue;
		missedTotal += ms.missed;
		SMVK_DEBUG_LOG(
			"[GfxAudit] mesh=%u world=%d batches=%u tris=%u gpuKill=%u occKill=%u missed=%u aliveTris=%u\n",
			m, gfxToWorld[m], ms.batches, ms.tris, ms.gpuKill, ms.occKill,
			ms.missed, ms.aliveTris);
	}
	SMVK_DEBUG_LOG(
		"[GfxAudit] zero=%u/%u cpuOccluded=%u cpuKeep=%u missed=%u%s (keep = triangle kill or shader divergence; "
		"bisect backface/frustum/smallprim to split)\n",
		zeroTotal, v.batchCount, cpuOcc, cpuKeep, missedTotal,
		(v.batchCount > 0 && zeroTotal == v.batchCount)
			? " WARNING: every batch killed (degenerate view or stale slice?)"
			: "");
}

void GfxRenderer::AddMainConsumeHints(smvk::RenderPass& pass, uint32_t slot) {
	using namespace smvk::gfx;
	if (slot >= gfx.views.size())
		return;
	// Filtered index region (in the static index monoblock, bound at
	// offset 0 by BindStaticVertexFmt) + the GPU-written indirect commands.
	// Engine helper covers IndexRead@Vertex and IndirectCommandRead@DrawIndirect.
	AddConsumeHints(pass, IndexHeapLogical(gfx), IndirectLogical(gfx, slot));
	// gfxfiltered.vert resolves model matrices from the draws ring slice
	// via BDA — compute/transfer-written, vertex-read.
	smvk::BufferSyncRequest drawsRead{};
	drawsRead.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
	drawsRead.barrierMasks.stageMask = smvk::PipelineStageBit_Vertex;
	pass.AddBufferHint(DrawLogical(gfx, slot), drawsRead);
	// The meshlet debug view draws cluster regions of the same monoblock
	// (the seed/remainder passes hint it too; Main needs the transition
	// whenever the debug branch runs).
	smvk::BufferSyncRequest indexRead{};
	indexRead.barrierMasks.accessMask = smvk::AccessFlags::IndexRead;
	indexRead.barrierMasks.stageMask = smvk::PipelineStageBit_Vertex;
	pass.AddBufferHint(IndexHeapLogical(gfx), indexRead);
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
	// No index bind: indirect firstIndex is absolute into the static index
	// monoblock (offset-0 binding from BindStaticVertexFmt is correct).
	cmd.DrawIndexedIndirect(smvk::gfx::IndirectBuffer(gfx, slot),
		(uint64_t)r.firstCommand * 20, r.commandCount, 20);
}

void GfxRenderer::DumpHzb(smvk::ResourceManager& ctx, uint32_t slot) {
	smvk::gfx::DumpHzb(gfx, ctx, slot);
}

} // namespace app
