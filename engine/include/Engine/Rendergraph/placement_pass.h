#pragma once
// Rendergraph PlacementPass: GPU tile-evaluation dispatch wiring.
//
// Owns per-tile-slot GPU buffers (instance array + atomic count) and the
// tile compute program, and shells the eval compute pass. The recording
// caller binds the program, pushes PlacementTilePush and dispatches —
// see PushAndDispatch. Barriers come from the pass buffer hints.
//
// Lifecycle (mirrors Particles):
//   PlacementPass pass;
//   pass.Ensure(ctx, rg, desc);          // resize-safe, once at load
//   pass.ClearTile(ctx, slot);           // zero the append counter
//   pass.BuildTilePush(layer, tile, ...) // CPU LayerBinding -> GPU push
//   auto& eval = pass.AddEvalPass(rg, slot);
//   eval.SetRecordingFunc([&](CommandBuffer& cmd){
//       pass.PushAndDispatch(cmd, push); });
//   // ...Execute... DownloadTile() for readback/parity/bake.
//
// Tile slots are persistent across frames (generation is on-demand, not
// per-frame); the rendergraph serializes eval passes via the RW hints.
// Slot <-> TileID assignment lives in PlacementSystem::TileCache (CPU);
// this struct only owns GPU memory + dispatch.

#include <Engine/Memory/pool.h>
#include <Engine/Placement/placement_gpu.h>
#include <Engine/Placement/placement_tile.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace smvk::placement {

struct PlacementPassDesc {
	uint32_t maxInstancesPerTile = 65536; // must match HLSL array bound
	uint32_t maxTiles = 8;                // concurrent resident GPU tile slots
	const char* name = "placement";
};

struct PlacementPass {
	PlacementPassDesc desc{};
	uint32_t tileSlots = 0; // live slot count (== desc.maxTiles after Create)

	std::vector<Holder<BufferHandle>> instanceBuffers;
	std::vector<Holder<BufferHandle>> countBuffers;
	std::vector<LogicalBufferHandle> instanceLogicals;
	std::vector<LogicalBufferHandle> countLogicals;

	Holder<ShaderModuleHandle> tileModule;
	ShaderProgram tileProgram{};

	[[nodiscard]] bool Valid() const {
		if (tileSlots == 0 || instanceBuffers.size() != tileSlots
			|| countBuffers.size() != tileSlots)
			return false;
		for (uint32_t i = 0; i < tileSlots; ++i) {
			if (instanceBuffers[i].empty() || countBuffers[i].empty()
				|| instanceLogicals[i].empty() || countLogicals[i].empty())
				return false;
		}
		return true;
	}
	[[nodiscard]] bool ShadersValid() const {
		return tileProgram.shaderCount != 0 && !tileModule.empty();
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* tileCsPath = kPlacementTileCsPath) {
		if (ShadersValid()) return true;
		try {
			if (tileModule.empty())
				tileModule = ctx.CreateShaderModule(
					readFile(tileCsPath ? tileCsPath : kPlacementTileCsPath),
					"placementTileCS");
		} catch (...) {
			return false;
		}
		if (tileModule.empty()) return false;
		Shader cs{ .module = tileModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		tileProgram = { .shaders = { cs }, .shaderCount = 1 };
		return true;
	}

	void DestroyShaders() {
		tileProgram = ShaderProgram{};
		tileModule = {};
	}

	bool Create(ResourceManager& ctx, RenderGraph& rg, const PlacementPassDesc& d,
		const char* tileCsPath = kPlacementTileCsPath) {
		if (d.maxInstancesPerTile == 0 || d.maxTiles == 0) return false;
		Destroy(rg);
		std::vector<Holder<BufferHandle>> inst;
		std::vector<Holder<BufferHandle>> cnt;
		std::vector<LogicalBufferHandle> instL;
		std::vector<LogicalBufferHandle> cntL;
		inst.reserve(d.maxTiles); cnt.reserve(d.maxTiles);
		instL.reserve(d.maxTiles); cntL.reserve(d.maxTiles);
		for (uint32_t s = 0; s < d.maxTiles; ++s) {
			Holder<BufferHandle> ib = ctx.CreateBuffer(
				sizeof(RenderInstance) * d.maxInstancesPerTile,
				BufferType_Storage, "placement tile instances");
			Holder<BufferHandle> cb = ctx.CreateBuffer(
				sizeof(uint32_t), BufferType_Storage, "placement tile count");
			if (ib.empty() || cb.empty()) return false;
			LogicalBufferHandle il = rg.AddExternalBuffer(ib, "placement tile instances");
			LogicalBufferHandle cl = rg.AddExternalBuffer(cb, "placement tile count");
			if (il.empty() || cl.empty()) {
				rg.FreeBuffer(il);
				rg.FreeBuffer(cl);
				return false;
			}
			inst.push_back(std::move(ib));
			cnt.push_back(std::move(cb));
			instL.push_back(il);
			cntL.push_back(cl);
		}
		desc = d;
		tileSlots = d.maxTiles;
		instanceBuffers = std::move(inst);
		countBuffers = std::move(cnt);
		instanceLogicals = std::move(instL);
		countLogicals = std::move(cntL);
		// Prime all append counters to zero (device memory is not
		// guaranteed zero; the kernel appends from the counter).
		for (uint32_t s = 0; s < tileSlots; ++s) ClearTile(ctx, s);
		return EnsureShaders(ctx, tileCsPath);
	}

	void Destroy(RenderGraph& rg) {
		for (uint32_t i = 0; i < tileSlots; ++i) {
			if (!instanceLogicals[i].empty()) rg.FreeBuffer(instanceLogicals[i]);
			if (!countLogicals[i].empty()) rg.FreeBuffer(countLogicals[i]);
		}
		instanceBuffers.clear();
		countBuffers.clear();
		instanceLogicals.clear();
		countLogicals.clear();
		tileSlots = 0;
	}

	bool Ensure(ResourceManager& ctx, RenderGraph& rg, const PlacementPassDesc& d,
		const char* tileCsPath = kPlacementTileCsPath) {
		const bool shapeOk = Valid()
			&& desc.maxInstancesPerTile == d.maxInstancesPerTile
			&& desc.maxTiles == d.maxTiles
			&& tileSlots == d.maxTiles;
		if (shapeOk) {
			desc = d;
			return EnsureShaders(ctx, tileCsPath);
		}
		return Create(ctx, rg, d, tileCsPath);
	}

	// Zeroes a slot's append counter. Call before recording the eval pass
	// for that slot (uploads flush immediately; the pass RW hint orders
	// the dispatch after prior uses).
	void ClearTile(ResourceManager& ctx, uint32_t slot) const {
		assert(slot < tileSlots);
		const uint32_t zero = 0;
		ctx.UploadDataToBuffer(countBuffers[slot], &zero, sizeof(zero),
			PipelineStageBit_Transfer, AccessFlags::TransferWrite, 0, 0);
	}

	uint64_t TileInstancesAddress(ResourceManager& ctx, uint32_t slot) const {
		assert(slot < tileSlots);
		return ctx.GetBufferDeviceAddress(instanceBuffers[slot]);
	}
	uint64_t TileCountAddress(ResourceManager& ctx, uint32_t slot) const {
		assert(slot < tileSlots);
		return ctx.GetBufferDeviceAddress(countBuffers[slot]);
	}

	// Eval compute pass shell for one tile slot. Caller binds tileProgram,
	// pushes PlacementTilePush (see BuildTilePush) and dispatches
	// PlacementDispatchGroups(candidateCount). Barriers: both slot buffers
	// are compute RW (append counter + instance writes).
	RenderPass& AddEvalPass(RenderGraph& rg, uint32_t slot,
		const char* name = "PlacementEval") const {
		assert(Valid() && slot < tileSlots);
		RenderPass& pass = rg.AddComputePass(name);
		BufferSyncRequest rw{};
		rw.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		rw.barrierMasks.stageMask = PipelineStageBit_Compute;
		pass.AddBufferHint(instanceLogicals[slot], rw);
		pass.AddBufferHint(countLogicals[slot], rw);
		return pass;
	}

	// Recording helper: bind + push + dispatch. No-op when empty.
	// Non-const: CommandBuffer::BindComputePipeline takes ShaderProgram&.
	void PushAndDispatch(CommandBuffer& cmd, const PlacementTilePush& push) {
		assert(ShadersValid());
		if (push.candidateCount == 0) return;
		cmd.BindComputePipeline(tileProgram);
		cmd.PushConstants(0, sizeof(push), &push, ShaderStage::All);
		cmd.Dispatch(PlacementDispatchGroups(push.candidateCount), 1, 1);
	}

	// Debug readback: count + instances for one slot (staging copy + fence
	// wait; stalls — parity checks, transient draws, bake only).
	// Returns instance count, or UINT32_MAX on failure.
	uint32_t DownloadTile(ResourceManager& ctx, uint32_t slot,
		std::vector<RenderInstance>& out, uint32_t maxInstances) const {
		assert(slot < tileSlots);
		out.clear();
		uint32_t count = 0;
		if (!ctx.DownloadDataFromBuffer(countBuffers[slot], &count, sizeof(count)))
			return UINT32_MAX;
		count = count > maxInstances ? maxInstances : count;
		if (count == 0) return 0;
		out.resize(count);
		if (!ctx.DownloadDataFromBuffer(instanceBuffers[slot], out.data(),
			(size_t)count * sizeof(RenderInstance)))
			return UINT32_MAX;
		return count;
	}

	// ---- CPU LayerBinding -> GPU push ----------------------------------
	// Extracts a constant density when the program is a single Const
	// (the kernel MVP's supported shape); otherwise falls back to 0.5
	// and the caller is expected to upgrade the kernel, not the data.
	static bool TryExtractConstDensity(const PlacementProgram& prog, float& out) {
		if (prog.instrs.size() == 1 && prog.acceptIndex == 0
			&& prog.instrs[0].op == FieldOp::Const) {
			out = prog.instrs[0].params[0];
			return true;
		}
		return false;
	}

	// Fills a push from a grid-domain layer. Returns false when the layer
	// has no grid binding (mesh/heightfield need their kernels).
	static bool BuildTilePush(const PlacementSystem::LayerBinding& layer,
		TileID tile, uint64_t instancesBDA, uint64_t countBDA,
		PlacementTilePush& out) {
		if (layer.domainKind != DomainKind::Grid || !layer.grid) return false;
		out = PlacementTilePush{};
		out.outInstances = instancesBDA;
		out.outCount = countBDA;
		out.seedLo = (uint32_t)(layer.program.graphSeed & 0xFFFFFFFFull);
		out.seedHi = (uint32_t)(layer.program.graphSeed >> 32);
		out.nodeId = layer.program.nodeId;
		out.candidateCount = layer.config.candidatesPerTile;
		out.tileX = tile.x; out.tileY = tile.y; out.tileZ = tile.z;
		out.gridOrigin = layer.grid->origin;
		out.cellsX = layer.grid->cellsX;
		out.gridExtent = layer.grid->extent;
		out.cellsZ = layer.grid->cellsZ;
		float density = 0.5f;
		TryExtractConstDensity(layer.program, density);
		out.density = density;
		out.noiseAmp = 0.0f;
		out.minScale = layer.config.minScale;
		out.maxScale = layer.config.maxScale;
		// Weighted prototype shares, clamped to the kernel's 8 slots.
		const size_t n = layer.prototypes.size() > 8 ? 8 : layer.prototypes.size();
		out.protoCount = (uint32_t)n;
		float* w[8] = { &out.weightsA.x, &out.weightsA.y, &out.weightsA.z, &out.weightsA.w,
			&out.weightsB.x, &out.weightsB.y, &out.weightsB.z, &out.weightsB.w };
		for (size_t i = 0; i < n; ++i) *w[i] = layer.prototypes.at(i).weight;
		if (n == 0) { out.protoCount = 1; out.weightsA.x = 1.0f; }
		return true;
	}
};

} // namespace smvk::placement
