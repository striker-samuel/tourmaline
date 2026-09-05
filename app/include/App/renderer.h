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
#include <Engine/App/world.h>
#include <Engine/Renderer/renderview.h>

namespace app {

struct GfxRenderer {
	smvk::gfx::State gfx{};
	smvk::Holder<smvk::ShaderModuleHandle> filteredVsModule;
	smvk::ShaderProgram filteredProgram{};

	// Loads the filtered VS and pairs it with the main frag module
	// (owned by main). Idempotent.
	bool Ensure(smvk::ResourceManager& ctx, smvk::RenderGraph& rg,
		smvk::ShaderModuleHandle mainFrag,
		const char* vsPath = smvk::gfx::kGfxFilteredVsPath);

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
	// gfx::FinalizeView. Returns the gfx view slot.
	uint32_t FilterMainView(smvk::ResourceManager& ctx, smvk::RenderGraph& rg,
		smvk::World& world, const smvk::RenderViewSet& views, uint32_t mainViewIdx,
		const glm::vec3& eye, const smvk::Extent2D& extent,
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

private:
	// world mesh idx -> gfx mesh idx (UINT32_MAX = not filterable).
	std::vector<uint32_t> meshMap_;
};

} // namespace app
