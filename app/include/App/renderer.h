#pragma once
// App-side filtered renderer: owns one gfx::State (plain data) plus the
// filtered VS program, and concentrates all GeometryFX wiring that would
// otherwise sprawl across main.cpp (registration, per-frame main-view
// filtering, material-range indirect draws).
//
// The main pass keeps PSO ownership: for each material range the caller
// binds FilteredProgram() with its own depth/blend/raster settings, pushes
// materialId with its own push struct, then calls DrawRange().

#include <Engine/Renderer/gfx_filter.h>
#include <Engine/Rendergraph/renderer.h>
#include <Engine/App/world.h>
#include <Engine/Renderer/renderview.h>
#include <App/push_constants.h>

namespace app {

// Debug-view pushes (share the global [0,256) push range; must mirror the
// debug shaders byte for byte).
struct alignas(16) MeshDebugPush {
	float model[16];
	float viewProj[16];
	float color[4];
};
static_assert(sizeof(MeshDebugPush) == 144, "MeshDebugPush must be 144 bytes");

struct alignas(16) LineDebugPush {
	float viewProj[16];
	float color[4];
};
static_assert(sizeof(LineDebugPush) == 80, "LineDebugPush must be 80 bytes");

// Per-view filter config (persistent resource-side config lives on the
// renderer; per-frame inputs arrive via FrameContext).
struct GfxConfig {
	smvk::gfx::CullFlags cullFlags =
		smvk::gfx::CullFlagBit_Backface | smvk::gfx::CullFlagBit_Frustum
		| smvk::gfx::CullFlagBit_Degenerate | smvk::gfx::CullFlagBit_HzbOcclusion;
};

struct GfxRenderer : public smvk::IFrameRenderer {
	smvk::gfx::State gfx{};
	GfxConfig config{};
	smvk::Holder<smvk::ShaderModuleHandle> filteredVsModule;
	smvk::ShaderProgram filteredProgram{};
	// Seed-depth programs (T3): draw the previous frame's visible clusters
	// straight from the cluster-ordered index heap to prime depth.
	smvk::Holder<smvk::ShaderModuleHandle> seedVsModule;
	smvk::Holder<smvk::ShaderModuleHandle> seedMaskedVsModule_;
	smvk::ShaderProgram seedOpaqueProgram{}; // VS-only (gfxseed mainVS)
	smvk::ShaderProgram seedMaskedProgram{}; // gfxseedmasked mainVS + shadow FS
	// Debug views (self-contained: own VS+FS, no Configure needed).
	// Meshlet colors draw every registered cluster direct (bypasses the
	// GPU filter, so HZB-culled meshlets stay visible); cone axes draw
	// one line per CPU candidate batch from a transient line buffer.
	smvk::Holder<smvk::ShaderModuleHandle> meshDebugVsModule;
	smvk::Holder<smvk::ShaderModuleHandle> meshDebugFragModule;
	smvk::ShaderProgram meshDebugProgram{};
	smvk::Holder<smvk::ShaderModuleHandle> lineVsModule;
	smvk::Holder<smvk::ShaderModuleHandle> lineFragModule;
	smvk::ShaderProgram lineProgram{};

	const char* Name() const override { return "GfxFilter"; }

	// IFrameRenderer (ConfigureFrag must run first: the filtered VS pairs
	// with main's frag module, owned by main; ConfigureSeedFrag provides
	// the alpha-test FS for the masked seed phase).
	bool Ensure(smvk::ResourceManager& ctx, smvk::RenderGraph& rg) override;
	void Destroy(smvk::RenderGraph& rg) override { smvk::gfx::Destroy(gfx, rg); }
	// CPU filter + compute pass for the main view, plus (when history is
	// armed) the seed depth pass from the previous frame's visible set.
	// The consume side (main opaque loop) stays in main until its push
	// data moves into the context.
	void AppendPasses(smvk::FrameContext& fc) override;
	// HZB build passes over the prepass depth (call after the Forward+
	// passes so depth is filled; T3 builds from full depth, T5 refreshes).
	// passName labels the framegraph nodes ("GfxHiz" for the first build,
	// "GfxHizRefresh" for the post-remainder rebuild over updated depth).
	void AppendHizPasses(smvk::FrameContext& fc,
		smvk::LogicalImageViewHandle depthLogical, uint32_t depthBindlessIndex,
		const char* passName = "GfxHiz");
	// Filter compute dispatch (call after AppendHizPasses: the CS reads the
	// HZB built this frame; dispatching earlier would read last frame's or
	// uninitialized memory). CPU expansion stays in AppendPasses.
	void AppendFilterDispatch(smvk::FrameContext& fc);
	[[nodiscard]] float MinContribution() const {
		if (lastSlot_ == UINT32_MAX || lastSlot_ >= gfx.temporals.size())
			return 0.0f;
		return gfx.temporals[lastSlot_].minimumContribution;
	}
	void SetMinContribution(float px2) {
		if (lastSlot_ == UINT32_MAX || lastSlot_ >= gfx.temporals.size())
			return;
		gfx.temporals[lastSlot_].minimumContribution = px2;
	}
	[[nodiscard]] uint32_t LastSlot() const { return lastSlot_; }
	// True when this frame seeded depth (prepass must Load, not Clear).
	[[nodiscard]] bool SeededThisFrame() const { return seededThisFrame_; }
	// Stats of the last filtered view (zeros when inactive). For the debug UI.
	[[nodiscard]] smvk::gfx::Stats LastStats() const {
		if (lastSlot_ == UINT32_MAX || lastSlot_ >= gfx.views.size())
			return smvk::gfx::Stats{};
		return gfx.views[lastSlot_].stats;
	}
	[[nodiscard]] uint32_t LastBatchCount() const {
		if (lastSlot_ == UINT32_MAX || lastSlot_ >= gfx.views.size())
			return 0;
		return gfx.views[lastSlot_].batchCount;
	}
	[[nodiscard]] size_t LastMaterialRangeCount() const {
		if (lastSlot_ == UINT32_MAX || lastSlot_ >= gfx.views.size())
			return 0;
		return gfx.views[lastSlot_].materialRanges.size();
	}

	void ConfigureFrag(smvk::ShaderModuleHandle mainFrag) { mainFrag_ = mainFrag; }
	void ConfigureSeedFrag(smvk::ShaderModuleHandle seedFrag) { seedFrag_ = seedFrag; }

	// Walks world.meshLocations once after scene load/resolve. Meshes with
	// cooked meshlets (pak path) register from those; meshes with retained
	// uploads (clusters with SetRetainUploads(true)) cook at register;
	// anything else maps to UINT32_MAX (direct-draw fallback, still
	// rendered by the legacy path).
	void RegisterWorldMeshes(smvk::World& world);
	void UploadStaticData(smvk::ResourceManager& ctx, smvk::RenderGraph& rg);

	// Per-frame main-view filtering. Iterates Renderable|Opaque|
	// StaticVertexFmt nodes (same set as the main opaque loop), mesh-level
	// sphere pre-cull, then cluster expansion with cone culling inside
	// gfx::FinalizeView. frameIndex is the swapchain frame slot (rings all
	// mutable buffers, cf. RenderViewSet). Returns the gfx view slot.
	uint32_t FilterMainView(smvk::ResourceManager& ctx, smvk::RenderGraph& rg,
		uint32_t frameIndex, smvk::World& world, const smvk::RenderViewSet& views,
		uint32_t mainViewIdx, const glm::vec3& eye, const smvk::Extent2D& extent,
		smvk::gfx::CullFlags flags);

	smvk::RenderPass& AddFilterPass(smvk::RenderGraph& rg, uint32_t slot);

	// Consume side (caller has bound FilteredProgram() with its own PSO).
	// PushViewConstants writes the 24B VS tail push once per pass;
	// DrawRange issues one multi-draw over the material's command range.
	void PushViewConstants(smvk::CommandBuffer& cmd, smvk::ResourceManager& ctx,
		uint32_t slot, uint64_t renderViewsAddr, uint32_t renderViewIndex);
	void DrawRange(smvk::CommandBuffer& cmd, smvk::ResourceManager& ctx,
		uint32_t slot, uint32_t rangeIdx);

	[[nodiscard]] const std::vector<smvk::gfx::MaterialRange>& MaterialRanges(uint32_t slot) const {
		return smvk::gfx::MaterialRanges(gfx, slot);
	}
	// True when the world mesh participates in filtering (false = legacy
	// direct path still draws it; used to avoid double-draw).
	[[nodiscard]] bool IsMeshFiltered(uint32_t worldMesh) const {
		return worldMesh < meshMap_.size() && meshMap_[worldMesh] != UINT32_MAX;
	}
	[[nodiscard]] smvk::ShaderProgram& FilteredProgram() { return filteredProgram; }
	[[nodiscard]] smvk::gfx::State& State() { return gfx; }
	// Consume-side barriers for the Main pass (filtered ranges + meshlet
	// debug view): filtered index list (IndexRead), GPU-written indirect
	// commands, draws ring slice (VS model matrices via BDA), and the
	// cluster-ordered index heap (debug view bind). Without these the
	// rendergraph emits NO compute->vertex barrier between the filter CS
	// and Main's fetches — a same-submit race that flips with any
	// workload change (MSAA mode, light toggles, camera motion).
	void AddMainConsumeHints(smvk::RenderPass& pass, uint32_t slot);
	[[nodiscard]] smvk::ShaderProgram& MeshDebugProgram() { return meshDebugProgram; }
	[[nodiscard]] smvk::ShaderProgram& LineProgram() { return lineProgram; }
	[[nodiscard]] bool DebugProgramsValid() const {
		return meshDebugProgram.shaderCount == 2 && lineProgram.shaderCount == 2;
	}
	// world mesh idx -> gfx mesh idx (UINT32_MAX = not filterable).
	[[nodiscard]] uint32_t GfxMeshForWorldMesh(uint32_t worldMesh) const {
		return worldMesh < meshMap_.size() ? meshMap_[worldMesh] : UINT32_MAX;
	}
	// Stable saturated color per cluster (debug view). mode 0 = hash hue,
	// mode 1 = cone-cutoff heat (red = degenerate/useless cone).
	static void DebugClusterColor(uint32_t cluster, float coneCutoff, int mode, float outRgba[4]);
	// Seed consume side (caller has bound a seed program with its own PSO).
	// PushSeedConstants writes the 24B VS tail push (draws BDA +
	// render-view slab); DrawSeedRange issues one multi-draw over a seed
	// range's commands (index buffer = cluster-ordered filter indices).
	// drawsAddr selects the model source: visDraws snapshot (seed) or
	// current draw constants (remainder).
	void PushSeedConstants(smvk::CommandBuffer& cmd, smvk::ResourceManager& ctx,
		uint32_t slot, uint64_t renderViewsAddr, uint32_t renderViewIndex,
		uint64_t drawsAddr);
	void DrawSeedRange(smvk::CommandBuffer& cmd, uint32_t slot, uint32_t rangeIdx);
	void DrawRemainderRange(smvk::CommandBuffer& cmd, uint32_t slot, uint32_t rangeIdx);
	// Shared two-phase depth-list recorder (opaque VS-only, then masked
	// per-material). remainder=false draws the seed list (visDraws
	// snapshot), true draws the remainder list (current draws).
	void RecordDepthList(smvk::CommandBuffer& cmd, smvk::ResourceManager& ctx,
		uint32_t slot, bool remainder, uint64_t drawsAddr,
		uint64_t renderViewsAddr, uint32_t renderViewIndex,
		const PushConstants& depthPC);
	// Depth pass 2 over the remainder list (call after AppendFilterDispatch
	// when RemainderPending; Load/Store over seeded depth).
	void AppendRemainderPass(smvk::FrameContext& fc);
	[[nodiscard]] bool RemainderPending() const { return remainderPending_; }
	// Debug audit: downloads this view's indirect commands + HZB pyramid
	// and attributes every zero-indexCount batch (GPU kill) against the CPU
	// mirror (HzbOccluded). CPU-agrees = HZB input/bounds fault;
	// CPU-disagrees = triangle-stage kill or shader/mirror divergence
	// (bisect with the backface/frustum/smallprim toggles to split those).
	// Stalls (fence wait); call after WaitForDeviceIdle, debug-only.
	void DumpIndirectAudit(smvk::ResourceManager& ctx, uint32_t slot);

private:
	// world mesh idx -> gfx mesh idx (UINT32_MAX = not filterable).
	std::vector<uint32_t> meshMap_;
	uint64_t frameCounter_ = 0; // throttles the per-frame stats log
	uint32_t lastSlot_ = UINT32_MAX;
	bool seededThisFrame_ = false;
	bool remainderPending_ = false;
	smvk::ShaderModuleHandle mainFrag_{};
	smvk::ShaderModuleHandle seedFrag_{};
};

} // namespace app
