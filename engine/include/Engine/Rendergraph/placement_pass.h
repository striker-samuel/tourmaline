// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

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
#include <cstdio>
#include <cstring>
#include <vector>

namespace smvk::placement {

struct PlacementPassDesc {
	uint32_t maxInstancesPerTile = 65536; // per-slot append bound (capacity);
	                                      // may be < the HLSL view bound
	                                      // (65536) — the kernels guard with
	                                      // the count buffer's capacity
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
		tileProgram = ShaderProgram();
		tileModule = {};
	}

	bool Create(ResourceManager& ctx, RenderGraph& rg, const PlacementPassDesc& d,
		const char* tileCsPath = kPlacementTileCsPath) {
		if (d.maxInstancesPerTile == 0 || d.maxTiles == 0) return false;
		if (d.maxInstancesPerTile > 65536) return false; // the HLSL view bound
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
				sizeof(PlacementCountGpu), BufferType_Storage, "placement tile count");
			if (ib.empty() || cb.empty()) {
				// Graceful by default (the RHI returns empty handles and
				// never decides policy); strict turns it into a loud abort
				// at the call site (see AGENTS.md — the error contract).
				smvk::OnResourceExhaustion("placement tile buffers");
				return false;
			}
			LogicalBufferHandle il = rg.AddExternalBuffer(ib, "placement tile instances");
			LogicalBufferHandle cl = rg.AddExternalBuffer(cb, "placement tile count");
			if (il.empty() || cl.empty()) {
				rg.FreeBuffer(il);
				rg.FreeBuffer(cl);
				smvk::OnResourceExhaustion("placement tile logicals");
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
		// Prime every append counter BEFORE first use: count/overflow zeroed
		// AND capacity published (DEVICE_LOCAL pages recycle garbage on
		// churn — the UploadDataToBuffer zero-on-creation convention; the
		// kernels' capacity guard reads it back).
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

	// Primes a slot's append counter for the next dispatch: count/overflow
	// zeroed AND capacity published (the kernels' guard reads it). Call
	// before recording the eval pass for that slot (uploads flush
	// immediately; the pass RW hint orders the dispatch after prior uses).
	void ClearTile(ResourceManager& ctx, uint32_t slot) const {
		assert(slot < tileSlots);
		PlacementCountGpu state{};
		state.capacity = desc.maxInstancesPerTile;
		ctx.UploadDataToBuffer(countBuffers[slot], &state, sizeof(state),
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
	// Dispatch-setup contract (Phase 0): candidateCount sane, output BDAs
	// valid, prototype count within the kernels' 8 slots — asserted so a
	// mis-built push faults at dispatch setup, not as device garbage.
	// Non-const: CommandBuffer::BindComputePipeline takes ShaderProgram&.
	void PushAndDispatch(CommandBuffer& cmd, const PlacementTilePush& push) {
		assert(ShadersValid());
		if (push.candidateCount == 0) return;
		assert(push.outInstances != 0 && "placement: null instances BDA");
		assert(push.outCount != 0 && "placement: null count BDA");
		assert(push.protoCount >= 1 && push.protoCount <= 8
			&& "placement: protoCount outside the kernel's 8 slots");
		cmd.BindComputePipeline(tileProgram);
		cmd.PushConstants(0, sizeof(push), &push, ShaderStage::All);
		cmd.Dispatch(PlacementDispatchGroups(push.candidateCount), 1, 1);
	}

	// Debug readback: count + instances for one slot (staging copy + fence
	// wait; stalls — parity checks, transient draws, bake only).
	// Returns the CLAMPED instance count (min of the raw count, the slot's
	// capacity and maxInstances), or UINT32_MAX on failure. overflowOut,
	// when non-null, receives the guard's dropped-accepted count — a
	// successful tile reports 0; > 0 means the capacity was too small and
	// the caller must treat the tile as an error/diagnostic, not a
	// truncated success.
	uint32_t DownloadTile(ResourceManager& ctx, uint32_t slot,
		std::vector<RenderInstance>& out, uint32_t maxInstances,
		uint32_t* overflowOut = nullptr) const {
		assert(slot < tileSlots);
		out.clear();
		PlacementCountGpu state;
		if (!ctx.DownloadDataFromBuffer(countBuffers[slot], &state, sizeof(state)))
			return UINT32_MAX;
		if (overflowOut) *overflowOut = state.overflow;
		if (state.overflow > 0)
			std::fprintf(stderr, "[Placement] tile slot %u OVERFLOW: %u accepted "
				"candidates dropped (capacity %u) — raise "
				"PlacementPassDesc::maxInstancesPerTile\n",
				slot, state.overflow, state.capacity);
		uint32_t count = state.count;
		if (count > state.capacity) count = state.capacity; // raw -> clamped
		if (count > maxInstances) count = maxInstances;
		if (count == 0) return 0;
		out.resize(count);
		if (!ctx.DownloadDataFromBuffer(instanceBuffers[slot], out.data(),
			(size_t)count * sizeof(RenderInstance)))
			return UINT32_MAX;
		return count;
	}

	// ---- CPU LayerBinding -> GPU push ----------------------------------
	// PR02: the density comes from the UPLOADED PROGRAM (instrBDA) —
	// there is no const-extraction and no 0.5 fallback. The caller
	// validates + uploads the program through PlacementProgramGpu and
	// passes its view; an invalid-for-GPU program refuses the fill
	// explicitly (the caller runs the CPU oracle instead — never a
	// silent approximation).
	static bool BuildTilePush(const PlacementSystem::LayerBinding& layer,
		TileID tile, const PlacementProgramView& program,
		const PlacementEcoView& eco,
		uint64_t instancesBDA, uint64_t countBDA,
		PlacementTilePush& out) {
		if (layer.domainKind != DomainKind::Grid || !layer.grid) return false;
		if (!program.Valid()) return false;
		// The eco gate: a program that READS the painted layer refuses
		// when the LAYER carries no mask or the view is unresolved
		// (never a silent zero density).
		if (ProgramUsesEcotopeMask(layer.program)
			&& (!layer.ecotope || !eco.Valid())) return false;
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
		out.densityScale = layer.config.densityScale;
		// noiseAmpRetired/noiseFreqRetired stay 0 (the program subsumes).
		out.minScale = layer.config.minScale;
		out.maxScale = layer.config.maxScale;
		// Weighted prototype shares, clamped to the kernel's 8 slots.
		const size_t n = layer.prototypes.size() > 8 ? 8 : layer.prototypes.size();
		out.protoCount = (uint32_t)n;
		float* w[8] = { &out.weightsA.x, &out.weightsA.y, &out.weightsA.z, &out.weightsA.w,
			&out.weightsB.x, &out.weightsB.y, &out.weightsB.z, &out.weightsB.w };
		for (size_t i = 0; i < n; ++i) *w[i] = layer.prototypes.at(i).weight;
		if (n == 0) { out.protoCount = 1; out.weightsA.x = 1.0f; }
		// The program tail (the graph SSA the kernel evaluates).
		out.instrBDA = program.instrBDA;
		out.instrCount = program.instrCount;
		out.acceptIndex = program.acceptIndex;
		// The ecotope-mask tail (PR04: the painted layer).
		out.ecoBDA = eco.bda;
		out.ecoW = eco.w;
		out.ecoH = eco.h;
		out.ecoChannels = eco.channels;
		out.ecoOX = eco.ox; out.ecoOZ = eco.oz;
		out.ecoSX = eco.sx; out.ecoSZ = eco.sz;
		return true;
	}
}; // struct PlacementPass

// ---------------------------------------------------------------------------
// The uploaded field program (PR02): converts FieldInstr -> GpuFieldInstr
// (the stable device layout), validates, uploads ONCE per (programId,
// revision) — the caller holds one PlacementProgramGpu per layer and
// re-Ensures after a graph edit/reload, never per frame. The kernels
// evaluate the actual SSA; invalid programs refuse at validation (the
// caller falls back to the CPU oracle with a diagnostic — never a
// silent density substitution).
// ---------------------------------------------------------------------------
struct PlacementProgramGpu {
	uint64_t programId = 0;
	uint64_t revision = 0;
	Holder<BufferHandle> instrs;     // GpuFieldInstr[instrCount]
	LogicalBufferHandle instrsL{};
	uint32_t instrCount = 0;
	uint32_t acceptIndex = 0;

	[[nodiscard]] bool Valid() const {
		return !instrs.empty() && !instrsL.empty() && instrCount > 0;
	}
	void Destroy(RenderGraph& rg) {
		if (!instrsL.empty()) rg.FreeBuffer(instrsL);
		instrsL = {};
		instrs = {};
		instrCount = acceptIndex = 0;
	}
	// Validates + converts + uploads. Returns false (with a stderr
	// diagnostic) when the program is not GPU-evaluable — the caller
	// must NOT dispatch; run the CPU oracle instead. The eco gate: a
	// program using EcotopeMask refuses when the layer carries no mask
	// (or an out-of-range paint channel) — never a silent zero density.
	bool Upload(ResourceManager& ctx, RenderGraph& rg,
		const PlacementSystem::LayerBinding& layer, uint64_t rev) {
		const uint32_t ecoChannels = layer.ecotope ? layer.ecotope->channels : 0;
		const char* err = nullptr;
		if (!ValidateProgramForGpu(layer.program, ecoChannels, err)) {
			std::fprintf(stderr, "[Placement] program %llu NOT GPU-evaluable: %s"
				" — the layer runs on the CPU oracle\n",
				(unsigned long long)layer.programId, err ? err : "?");
			Destroy(rg);
			programId = revision = 0;
			return false;
		}
		Destroy(rg);
		const size_t n = layer.program.instrs.size();
		std::vector<GpuFieldInstr> gpu(n);
		for (size_t i = 0; i < n; ++i) {
			const FieldInstr& in = layer.program.instrs[i];
			gpu[i].op = static_cast<uint32_t>(in.op);
			gpu[i].a = in.a;
			gpu[i].b = in.b;
			gpu[i].reserved = 0;
			for (int k = 0; k < 4; ++k) gpu[i].params[k] = in.params[k];
		}
		instrs = ctx.CreateBuffer(sizeof(GpuFieldInstr) * n,
			BufferType_Storage, "placement field program");
		if (instrs.empty()) {
			smvk::OnResourceExhaustion("placement program instrs buffer");
			return false;
		}
		instrsL = rg.AddExternalBuffer(instrs, "placement field program");
		if (instrsL.empty()) {
			smvk::OnResourceExhaustion("placement program instrs logical");
			return false;
		}
		ctx.UploadDataToBuffer(instrs, gpu.data(), sizeof(GpuFieldInstr) * n,
			PipelineStageBit_Compute, AccessFlags::ShaderRead, 0, 0);
		programId = layer.programId;
		revision = rev;
		instrCount = (uint32_t)n;
		acceptIndex = layer.program.acceptIndex;
		return true;
	}
	// Re-uploads only when the (programId, revision) key changed.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg,
		const PlacementSystem::LayerBinding& layer, uint64_t rev) {
		if (Valid() && programId == layer.programId && revision == rev)
			return true;
		return Upload(ctx, rg, layer, rev);
	}
	PlacementProgramView View(ResourceManager& ctx) const {
		PlacementProgramView v;
		v.instrBDA = Valid() ? ctx.GetBufferDeviceAddress(instrs) : 0;
		v.instrCount = instrCount;
		v.acceptIndex = acceptIndex;
		return v;
	}
};

// ---------------------------------------------------------------------------
// PR04: the uploaded ecotope weight mask — the painted layer the
// EcotopeMask FieldOp reads. Uploaded ONCE per mask edit (the caller
// re-Ensures after a paint change), never per frame. Invalid programs
// (maskless layers using EcotopeMask) refuse at validation.
// ---------------------------------------------------------------------------
inline constexpr uint32_t kPlacementMaxEcoTexels = 1048576; // 4M floats

struct PlacementEcoMaskGpu {
	Holder<BufferHandle> weights;    // float[channels*h*w]
	LogicalBufferHandle weightsL{};
	uint32_t w = 0, h = 0, channels = 0;

	[[nodiscard]] bool Valid() const {
		return !weights.empty() && !weightsL.empty() && channels >= 1;
	}
	void Destroy(RenderGraph& rg) {
		if (!weightsL.empty()) rg.FreeBuffer(weightsL);
		weightsL = {};
		weights = {};
		w = h = channels = 0;
	}
	bool Upload(ResourceManager& ctx, RenderGraph& rg,
		const EcotopeMaskData& eco) {
		Destroy(rg);
		if (!eco.valid()) return false;
		if ((uint64_t)eco.channels * eco.height * eco.width
			> kPlacementMaxEcoTexels) return false;
		weights = ctx.CreateBuffer(sizeof(float) * eco.weights.size(),
			BufferType_Storage, "placement ecotope mask");
		if (weights.empty()) {
			smvk::OnResourceExhaustion("placement ecotope mask buffer");
			return false;
		}
		weightsL = rg.AddExternalBuffer(weights, "placement ecotope mask");
		if (weightsL.empty()) {
			smvk::OnResourceExhaustion("placement ecotope mask logical");
			return false;
		}
		ctx.UploadDataToBuffer(weights, eco.weights.data(),
			sizeof(float) * eco.weights.size(),
			PipelineStageBit_Compute, AccessFlags::ShaderRead, 0, 0);
		w = eco.width; h = eco.height; channels = eco.channels;
		return true;
	}
	PlacementEcoView View(ResourceManager& ctx) const {
		PlacementEcoView v;
		v.bda = Valid() ? ctx.GetBufferDeviceAddress(weights) : 0;
		v.w = w; v.h = h; v.channels = channels;
		return v;
	}
// The CPU-side view of the SAME data (origin/spacing from the source
// mask — the caller fills them; the buffer carries only the grid).
	static PlacementEcoView ViewOf(const EcotopeMaskData& eco) {
		PlacementEcoView v;
		v.w = eco.width; v.h = eco.height; v.channels = eco.channels;
		v.ox = eco.origin.x; v.oz = eco.origin.y;
		v.sx = eco.spacing.x; v.sz = eco.spacing.y;
		return v;
	}
};

// ---------------------------------------------------------------------------
// PR05: GPU tile-slot residency — the TileKey -> PlacementPass slot map
// with generation tokens. The caller claims a slot when dispatching a
// tile's scatter chain, and publishes/retires with the rendergraph's
// in-flight conventions (never reassign a slot a prior frame's cull/
// draw still reads). A delayed publish against a stale generation (the
// slot was re-claimed under a newer activation) is refused — a rapid
// editor repaint can never read half-tenant contents.
// ---------------------------------------------------------------------------
struct PlacementGpuTileSlot {
	TileKey key{};             // empty programId = FREE
	uint64_t generation = 0;  // the activation token (bumped per claim)
	uint64_t lastUse = 0;     // LRU stamp
	bool generating = false;  // claimed, dispatched, not yet published
};

struct PlacementGpuTiles {
	std::vector<PlacementGpuTileSlot> slots;
	uint64_t tick_ = 0;

	void Init(uint32_t slotCount) {
		slots.assign(slotCount, PlacementGpuTileSlot{});
		tick_ = 0;
	}
	[[nodiscard]] bool Valid() const { return !slots.empty(); }

	// The slot serving key, or -1 (bumps LRU). FREE slots never match:
	// a real key carries programId != 0.
	int32_t Find(const TileKey& key) {
		if (key.programId == 0) return -1;
		for (size_t i = 0; i < slots.size(); ++i)
			if (slots[i].key == key) {
				slots[i].lastUse = ++tick_;
				return int32_t(i);
			}
		return -1;
	}
	// Claims a slot for key: a FREE one first, else the LRU
	// non-generating (published) one — evicting means REASSIGNMENT (the
	// caller guarantees the prior tenant's frames retired). -1 when
	// every slot is mid-generation.
	int32_t Claim(const TileKey& key) {
		int32_t victim = -1;
		uint64_t best = ~0ull;
		for (size_t i = 0; i < slots.size(); ++i) {
			PlacementGpuTileSlot& s = slots[i];
			if (s.key.programId == 0) {
				assign(s, key);
				return int32_t(i);
			}
			if (!s.generating && s.lastUse < best) {
				best = s.lastUse;
				victim = int32_t(i);
			}
		}
		if (victim < 0) return -1;
		assign(slots[size_t(victim)], key);
		return victim;
	}
	// Publishes a claimed slot. Refuses when the slot was re-claimed
	// under a newer activation (stale generation) or the key moved.
	bool Finish(int32_t slot, const TileKey& key, uint64_t generation) {
		if (slot < 0 || slot >= int32_t(slots.size())) return false;
		PlacementGpuTileSlot& s = slots[size_t(slot)];
		if (!s.generating || s.generation != generation || !(s.key == key))
			return false;
		s.generating = false; // published: the tenant's buffers are readable
		s.lastUse = ++tick_;
		return true;
	}
	// Retires a published slot back to FREE (the caller guarantees the
	// in-flight cull/draw users retired).
	void Release(int32_t slot) {
		if (slot < 0 || slot >= int32_t(slots.size())) return;
		slots[size_t(slot)] = PlacementGpuTileSlot{};
	}
	// The LIVE (published) keys, for diagnostics/LRU walks.
	template<typename Func>
	void ForEachPublished(Func&& func) const {
		for (const PlacementGpuTileSlot& s : slots)
			if (s.key.programId != 0 && !s.generating)
				func(s);
	}

private:
	void assign(PlacementGpuTileSlot& s, const TileKey& key) {
		s.key = key;
		s.generation = ++tick_;
		s.generating = true;
		s.lastUse = tick_;
	}
};

// ---------------------------------------------------------------------------
// PR03: the per-(tile slot, view) visible-path scratch. The persistent
// 40-byte instances NEVER move per camera frame — only the visible-
// index list does. One set per (slot, view); overlapping in-flight
// frames need their own sets (the rendergraph's logical/external
// buffer + in-flight retirement conventions apply).
//
// Frame cadence (per slot-view):
//   ClearFrame  — zero the mask + the bin slab (counts/appendNext;
//                 offsets/total are rewritten by prefix anyway)
//   cull -> prefix -> scatter -> drawbuild  (four compute passes; the
//                 rendergraph's buffer-hint edges order them)
//   SeedIndirect — once per prototype-table change (the immutable halves)
// ---------------------------------------------------------------------------
struct PlacementVisibleSet {
	Holder<BufferHandle> mask;      // bitpack verdict: maxInstances/32 words
	Holder<BufferHandle> bins;      // PlacementBinsGpu (128B)
	Holder<BufferHandle> visible;   // uint32[maxInstances]
	Holder<BufferHandle> indirect;  // PlacementIndirectCmd[8] (BufferType_Indirect)
	LogicalBufferHandle maskL{}, binsL{}, visibleL{}, indirectL{};
	uint32_t maxInstances = 0;

	[[nodiscard]] bool Valid() const {
		return !mask.empty() && !bins.empty() && !visible.empty()
			&& !indirect.empty() && !maskL.empty() && !binsL.empty()
			&& !visibleL.empty() && !indirectL.empty();
	}
	void Destroy(RenderGraph& rg) {
		for (LogicalBufferHandle l : { maskL, binsL, visibleL, indirectL })
			if (!l.empty()) rg.FreeBuffer(l);
		maskL = {}; binsL = {}; visibleL = {}; indirectL = {};
		mask = {}; bins = {}; visible = {}; indirect = {};
		maxInstances = 0;
	}
	bool Create(ResourceManager& ctx, RenderGraph& rg, uint32_t maxInst) {
		Destroy(rg);
		if (maxInst == 0 || maxInst > kPlacementMaxTileInstances) return false;
		mask = ctx.CreateBuffer(sizeof(uint32_t) * ((maxInst + 31) / 32),
			BufferType_Storage, "placement cull mask");
		bins = ctx.CreateBuffer(sizeof(PlacementBinsGpu),
			BufferType_Storage, "placement cull bins");
		visible = ctx.CreateBuffer(sizeof(uint32_t) * maxInst,
			BufferType_Storage, "placement visible");
		indirect = ctx.CreateBuffer(kPlacementIndirectStride * kPlacementMaxProtos,
			BufferType_Indirect, "placement indirect");
		if (mask.empty() || bins.empty() || visible.empty() || indirect.empty()) {
			smvk::OnResourceExhaustion("placement visible-set buffers");
			return false;
		}
		maskL = rg.AddExternalBuffer(mask, "placement cull mask");
		binsL = rg.AddExternalBuffer(bins, "placement cull bins");
		visibleL = rg.AddExternalBuffer(visible, "placement visible");
		indirectL = rg.AddExternalBuffer(indirect, "placement indirect");
		if (maskL.empty() || binsL.empty() || visibleL.empty() || indirectL.empty()) {
			smvk::OnResourceExhaustion("placement visible-set logicals");
			return false;
		}
		maxInstances = maxInst;
		// Creation-zero the atomics (the zero-on-creation convention).
		ClearFrame(ctx);
		return true;
	}
	// Per-frame zero of the mask + the bin slab's atomics.
	void ClearFrame(ResourceManager& ctx) const {
		assert(Valid());
		const std::vector<uint32_t> zeros((maxInstances + 31) / 32, 0);
		ctx.UploadDataToBuffer(mask, zeros.data(),
			sizeof(uint32_t) * zeros.size(),
			PipelineStageBit_Transfer, AccessFlags::TransferWrite, 0, 0);
		PlacementBinsGpu zeroBins{};
		ctx.UploadDataToBuffer(bins, &zeroBins, sizeof(zeroBins),
			PipelineStageBit_Transfer, AccessFlags::TransferWrite, 0, 0);
	}
	// Seeds the immutable command halves ONCE per prototype-table change
	// (the GPU rewrites instanceCount/firstInstance per frame). Pointer +
	// count: seeds is cmdCount PlacementIndirectCmd values (cmdCount <=
	// kPlacementMaxProtos; the dynamic halves are overwritten anyway).
	bool SeedIndirect(ResourceManager& ctx,
		const PlacementIndirectCmd* seeds, uint32_t cmdCount) const {
		assert(Valid());
		if (!seeds || cmdCount == 0 || cmdCount > kPlacementMaxProtos) return false;
		std::vector<PlacementIndirectCmd> slab(kPlacementMaxProtos,
			PlacementIndirectCmd{});
		for (uint32_t i = 0; i < cmdCount; ++i) {
			slab[i].indexCount = seeds[i].indexCount;
			slab[i].instanceCount = 0; // drawbuild's per-frame output
			slab[i].firstIndex = seeds[i].firstIndex;
			slab[i].vertexOffset = seeds[i].vertexOffset;
			slab[i].firstInstance = 0; // drawbuild's per-frame output
		}
		ctx.UploadDataToBuffer(indirect, slab.data(),
			kPlacementIndirectStride * kPlacementMaxProtos,
			PipelineStageBit_Transfer, AccessFlags::TransferWrite, 0, 0);
		return true;
	}
	uint64_t MaskAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(mask);
	}
	uint64_t BinsAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(bins);
	}
	uint64_t VisibleAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(visible);
	}
	uint64_t IndirectAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(indirect);
	}
};

// The chain's shader program set (cull + prefix + scatter + drawbuild).
struct PlacementCullPrograms {
	Holder<ShaderModuleHandle> cullModule, prefixModule, scatterModule, buildModule;
	ShaderProgram cullProgram{}, prefixProgram{}, scatterProgram{}, buildProgram{};

	[[nodiscard]] bool Valid() const {
		return cullProgram.shaderCount != 0 && prefixProgram.shaderCount != 0
			&& scatterProgram.shaderCount != 0 && buildProgram.shaderCount != 0
			&& !cullModule.empty() && !prefixModule.empty()
			&& !scatterModule.empty() && !buildModule.empty();
	}
	bool Ensure(ResourceManager& ctx,
		const char* cullCsPath = kPlacementCullCsPath,
		const char* prefixCsPath = kPlacementPrefixCsPath,
		const char* scatterCsPath = kPlacementScatterVisCsPath,
		const char* buildCsPath = kPlacementDrawBuildCsPath) {
		if (Valid()) return true;
		try {
			if (cullModule.empty())
				cullModule = ctx.CreateShaderModule(
					readFile(cullCsPath ? cullCsPath : kPlacementCullCsPath),
					"placementCullCS");
			if (prefixModule.empty())
				prefixModule = ctx.CreateShaderModule(
					readFile(prefixCsPath ? prefixCsPath : kPlacementPrefixCsPath),
					"placementPrefixCS");
			if (scatterModule.empty())
				scatterModule = ctx.CreateShaderModule(
					readFile(scatterCsPath ? scatterCsPath : kPlacementScatterVisCsPath),
					"placementScatterVisCS");
			if (buildModule.empty())
				buildModule = ctx.CreateShaderModule(
					readFile(buildCsPath ? buildCsPath : kPlacementDrawBuildCsPath),
					"placementDrawBuildCS");
		} catch (...) {
			return false;
		}
		if (cullModule.empty() || prefixModule.empty()
			|| scatterModule.empty() || buildModule.empty())
			return false;
		Shader c{ .module = cullModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		cullProgram = { .shaders = { c }, .shaderCount = 1 };
		Shader p{ .module = prefixModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		prefixProgram = { .shaders = { p }, .shaderCount = 1 };
		Shader s{ .module = scatterModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		scatterProgram = { .shaders = { s }, .shaderCount = 1 };
		Shader b{ .module = buildModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		buildProgram = { .shaders = { b }, .shaderCount = 1 };
		return true;
	}
	void Destroy() {
		cullProgram = ShaderProgram();
		prefixProgram = ShaderProgram();
		scatterProgram = ShaderProgram();
		buildProgram = ShaderProgram();
		cullModule = {}; prefixModule = {}; scatterModule = {}; buildModule = {};
	}

	// ---- Pass shells (buffer-hint edges order the chain) ----
	// cull: instances+count read; mask+bins RW (+ alive read when used).
	static RenderPass& AddCullPass(RenderGraph& rg, const PlacementPass& pass,
		uint32_t slot, const PlacementVisibleSet& vis,
		LogicalBufferHandle aliveLogical,
		LogicalBufferHandle renderViewsLogical) {
		RenderPass& rp = rg.AddComputePass("PlacementCull");
		BufferSyncRequest read{};
		read.barrierMasks.accessMask = AccessFlags::ShaderRead;
		read.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(pass.instanceLogicals[slot], read);
		rp.AddBufferHint(pass.countLogicals[slot], read);
		if (!aliveLogical.empty()) rp.AddBufferHint(aliveLogical, read);
		if (!renderViewsLogical.empty()) rp.AddBufferHint(renderViewsLogical, read);
		BufferSyncRequest rw{};
		rw.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		rw.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(vis.maskL, rw);
		rp.AddBufferHint(vis.binsL, rw);
		return rp;
	}
	// prefix: bins RW (reads counts, writes offsets/total).
	static RenderPass& AddPrefixPass(RenderGraph& rg, const PlacementVisibleSet& vis) {
		RenderPass& rp = rg.AddComputePass("PlacementPrefix");
		BufferSyncRequest rw{};
		rw.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		rw.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(vis.binsL, rw);
		return rp;
	}
	// scatter: instances+count+bins read; bins RW (appendNext) + visible RW.
	static RenderPass& AddScatterVisPass(RenderGraph& rg, const PlacementPass& pass,
		uint32_t slot, const PlacementVisibleSet& vis) {
		RenderPass& rp = rg.AddComputePass("PlacementScatterVisible");
		BufferSyncRequest read{};
		read.barrierMasks.accessMask = AccessFlags::ShaderRead;
		read.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(pass.instanceLogicals[slot], read);
		rp.AddBufferHint(pass.countLogicals[slot], read);
		rp.AddBufferHint(vis.maskL, read);
		BufferSyncRequest rw{};
		rw.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		rw.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(vis.binsL, rw);
		rp.AddBufferHint(vis.visibleL, rw);
		return rp;
	}
	// drawbuild: bins read; indirect RW (the dynamic halves).
	static RenderPass& AddDrawBuildPass(RenderGraph& rg, const PlacementVisibleSet& vis) {
		RenderPass& rp = rg.AddComputePass("PlacementDrawBuild");
		BufferSyncRequest read{};
		read.barrierMasks.accessMask = AccessFlags::ShaderRead;
		read.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(vis.binsL, read);
		BufferSyncRequest rw{};
		rw.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		rw.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(vis.indirectL, rw);
		return rp;
	}

	// ---- Recording helpers (dispatch widths: the CPU-known capacity
	// upper bound — the kernels self-bound from the count buffer). ----
	static void PushAndDispatchCull(CommandBuffer& cmd, ShaderProgram& program,
		const PlacementCullPush& push, uint32_t capacity) {
		if (capacity == 0) return;
		assert(push.instances != 0 && push.count != 0
			&& push.mask != 0 && push.bins != 0);
		assert(push.renderViews != 0);
		assert(push.protoCount >= 1 && push.protoCount <= kPlacementMaxProtos);
		cmd.BindComputePipeline(program);
		cmd.PushConstants(0, sizeof(push), &push, ShaderStage::All);
		cmd.Dispatch(PlacementDispatchGroups(capacity), 1, 1);
	}
	static void PushAndDispatchPrefix(CommandBuffer& cmd, ShaderProgram& program,
		const PlacementCullPush& push) {
		cmd.BindComputePipeline(program);
		cmd.PushConstants(0, sizeof(push), &push, ShaderStage::All);
		cmd.Dispatch(1, 1, 1);
	}
	static void PushAndDispatchScatterVis(CommandBuffer& cmd, ShaderProgram& program,
		const PlacementScatterVisPush& push, uint32_t capacity) {
		if (capacity == 0) return;
		assert(push.instances != 0 && push.count != 0 && push.mask != 0
			&& push.bins != 0 && push.visible != 0);
		cmd.BindComputePipeline(program);
		cmd.PushConstants(0, sizeof(push), &push, ShaderStage::All);
		cmd.Dispatch(PlacementDispatchGroups(capacity), 1, 1);
	}
	static void PushAndDispatchDrawBuild(CommandBuffer& cmd, ShaderProgram& program,
		const PlacementDrawBuildPush& push) {
		assert(push.bins != 0 && push.indirect != 0);
		cmd.BindComputePipeline(program);
		cmd.PushConstants(0, sizeof(push), &push, ShaderStage::All);
		cmd.Dispatch(1, 1, 1);
	}
};

// Pure-CPU fill (headless-testable): the per-proto conservative spheres
// come from the layer's cooked-mesh bounds (boundsMin/Max -> center +
// radius — never a made-up guard factor); the species distances from
// the prototypes' maxDrawDistance. BDAs are raw values the caller
// resolved (the PlacementPass slot + the PlacementVisibleSet + the
// app's render-view slab).
inline void FillCullPush(const PlacementSystem::LayerBinding& layer,
	uint64_t instancesBDA, uint64_t countBDA, uint64_t maskBDA,
	uint64_t binsBDA, uint64_t aliveBDA, uint64_t renderViewsBDA,
	uint32_t renderViewIndex, const glm::vec3& camPos, uint32_t cullFlags,
	PlacementCullPush& out) {
	out = PlacementCullPush{};
	out.instances = instancesBDA;
	out.count = countBDA;
	out.mask = maskBDA;
	out.bins = binsBDA;
	out.alive = aliveBDA;   // 0 = skip the alive test (CPU tiles)
	out.renderViews = renderViewsBDA;
	out.renderViewIndex = renderViewIndex;
	out.cullFlags = cullFlags;
	const size_t n = layer.prototypes.size() > kPlacementMaxProtos
		? kPlacementMaxProtos : layer.prototypes.size();
	out.protoCount = (uint32_t)(n == 0 ? 1 : n);
	for (size_t i = 0; i < n; ++i) {
		const Prototype& p = layer.prototypes.at(i);
		const glm::vec3 center = (p.boundsMin + p.boundsMax) * 0.5f;
		const glm::vec3 halfDiag = (p.boundsMax - p.boundsMin) * 0.5f;
		out.protoBounds[i] = glm::vec4(center, glm::length(halfDiag));
		out.protoDist[i] = p.maxDrawDistance;
	}
	if (n == 0)
		out.protoBounds[0] = glm::vec4(0.0f, 0.0f, 0.0f, 0.5f);
	out.camPos = camPos;
	out.maxInstanceScale = layer.config.maxScale;
}

// ---- Mesh / heightfield domain GPU data -------------------------------
// Padded uploads (float4 positions/normals) + BDAs + params fill. The
// HLSL views (placement_domains.hlsli) interpret raw bytes; the float4
// padding keeps CPU/GPU array strides unambiguous by construction.

inline constexpr uint32_t kPlacementMaxMeshVerts = 65536;
inline constexpr uint32_t kPlacementMaxMeshTris = 131072;
inline constexpr uint32_t kPlacementMaxHfTexels = 1048576;

struct ScatterBDAs {
	uint64_t pos = 0, nrm = 0, uv = 0, idx = 0, prob = 0, alias = 0;
	uint64_t heights = 0;
};

struct PlacementMeshGpu {
	Holder<BufferHandle> pos, nrm, uv, idx, prob, alias;
	LogicalBufferHandle posL{}, nrmL{}, uvL{}, idxL{}, probL{}, aliasL{};
	uint32_t vertCount = 0;
	uint32_t triCount = 0;
	bool hasNormals = false;
	bool hasUVs = false;

	[[nodiscard]] bool Valid() const {
		return !pos.empty() && !idx.empty() && !prob.empty() && !alias.empty()
			&& !posL.empty() && !idxL.empty() && !probL.empty() && !aliasL.empty()
			&& triCount > 0;
	}
	void Destroy(RenderGraph& rg) {
		for (LogicalBufferHandle l : { posL, nrmL, uvL, idxL, probL, aliasL })
			if (!l.empty()) rg.FreeBuffer(l);
		posL = {}; nrmL = {}; uvL = {}; idxL = {}; probL = {}; aliasL = {};
		pos = {}; nrm = {}; uv = {}; idx = {}; prob = {}; alias = {};
		vertCount = triCount = 0;
		hasNormals = hasUVs = false;
	}

	// Uploads (converting vec3 -> float4). Builds the alias table when
	// missing. Returns false when the mesh exceeds GPU bounds or is empty.
	bool Upload(ResourceManager& ctx, RenderGraph& rg, PlacementMeshData& mesh) {
		Destroy(rg);
		const size_t tris = mesh.triangleCount();
		if (tris == 0 || tris > kPlacementMaxMeshTris) return false;
		if (mesh.positions.empty()) return false;
		uint32_t maxIdx = 0;
		for (uint32_t i : mesh.indices) maxIdx = maxIdx > i ? maxIdx : i;
		const uint32_t verts = maxIdx + 1;
		if (verts == 0 || verts > kPlacementMaxMeshVerts) return false;
		if (!mesh.hasAlias()) BuildMeshAliasTable(mesh);
		hasNormals = mesh.normals.size() > maxIdx;
		hasUVs = mesh.uvs.size() > maxIdx;

		std::vector<glm::vec4> pos4(verts, glm::vec4(0.0f));
		for (uint32_t v = 0; v < verts && v < mesh.positions.size(); ++v)
			pos4[v] = glm::vec4(mesh.positions[v], 0.0f);
		std::vector<glm::vec4> nrm4;
		if (hasNormals) {
			nrm4.assign(verts, glm::vec4(0, 1, 0, 0));
			for (uint32_t v = 0; v < verts; ++v) nrm4[v] = glm::vec4(mesh.normals[v], 0.0f);
		}
		std::vector<glm::vec2> uv2;
		if (hasUVs) {
			uv2.assign(verts, glm::vec2(0.0f));
			for (uint32_t v = 0; v < verts; ++v) uv2[v] = mesh.uvs[v];
		}
		auto make = [&](size_t bytes, const char* name) {
			Holder<BufferHandle> b = ctx.CreateBuffer(bytes, BufferType_Storage, name);
			return b;
		};
		pos = make(sizeof(glm::vec4) * verts, "placement mesh pos");
		nrm = hasNormals ? make(sizeof(glm::vec4) * verts, "placement mesh nrm") : Holder<BufferHandle>{};
		uv = hasUVs ? make(sizeof(glm::vec2) * verts, "placement mesh uv") : Holder<BufferHandle>{};
		idx = make(sizeof(uint32_t) * tris * 3, "placement mesh idx");
		prob = make(sizeof(float) * tris, "placement mesh prob");
		alias = make(sizeof(uint32_t) * tris, "placement mesh alias");
		if (pos.empty() || idx.empty() || prob.empty() || alias.empty()
			|| (hasNormals && nrm.empty()) || (hasUVs && uv.empty())) {
			smvk::OnResourceExhaustion("placement mesh domain buffers");
			return false;
		}
		posL = rg.AddExternalBuffer(pos, "placement mesh pos");
		nrmL = hasNormals ? rg.AddExternalBuffer(nrm, "placement mesh nrm") : LogicalBufferHandle{};
		uvL = hasUVs ? rg.AddExternalBuffer(uv, "placement mesh uv") : LogicalBufferHandle{};
		idxL = rg.AddExternalBuffer(idx, "placement mesh idx");
		probL = rg.AddExternalBuffer(prob, "placement mesh prob");
		aliasL = rg.AddExternalBuffer(alias, "placement mesh alias");
		if (posL.empty() || idxL.empty() || probL.empty() || aliasL.empty()) {
			smvk::OnResourceExhaustion("placement mesh domain logicals");
			return false;
		}
		ctx.UploadDataToBuffer(pos, pos4.data(), sizeof(glm::vec4) * verts,
			PipelineStageBit_Compute, AccessFlags::ShaderRead, 0, 0);
		if (hasNormals) ctx.UploadDataToBuffer(nrm, nrm4.data(), sizeof(glm::vec4) * verts,
			PipelineStageBit_Compute, AccessFlags::ShaderRead, 0, 0);
		if (hasUVs) ctx.UploadDataToBuffer(uv, uv2.data(), sizeof(glm::vec2) * verts,
			PipelineStageBit_Compute, AccessFlags::ShaderRead, 0, 0);
		ctx.UploadDataToBuffer(idx, mesh.indices.data(), sizeof(uint32_t) * tris * 3,
			PipelineStageBit_Compute, AccessFlags::ShaderRead, 0, 0);
		ctx.UploadDataToBuffer(prob, mesh.aliasProb.data(), sizeof(float) * tris,
			PipelineStageBit_Compute, AccessFlags::ShaderRead, 0, 0);
		ctx.UploadDataToBuffer(alias, mesh.aliasIndex.data(), sizeof(uint32_t) * tris,
			PipelineStageBit_Compute, AccessFlags::ShaderRead, 0, 0);
		vertCount = verts;
		triCount = (uint32_t)tris;
		return true;
	}

	ScatterBDAs Addresses(ResourceManager& ctx) const {
		ScatterBDAs b;
		b.pos = ctx.GetBufferDeviceAddress(pos);
		b.nrm = hasNormals ? ctx.GetBufferDeviceAddress(nrm) : 0;
		b.uv = hasUVs ? ctx.GetBufferDeviceAddress(uv) : 0;
		b.idx = ctx.GetBufferDeviceAddress(idx);
		b.prob = ctx.GetBufferDeviceAddress(prob);
		b.alias = ctx.GetBufferDeviceAddress(alias);
		return b;
	}
	// Read-only logicals for scatter-pass hints (null entries skipped).
	std::vector<LogicalBufferHandle> ReadLogicals() const {
		std::vector<LogicalBufferHandle> out;
		for (LogicalBufferHandle l : { posL, nrmL, uvL, idxL, probL, aliasL })
			if (!l.empty()) out.push_back(l);
		return out;
	}
};

struct PlacementHeightfieldGpu {
	Holder<BufferHandle> heights;
	LogicalBufferHandle heightsL{};
	uint32_t w = 0, h = 0;

	[[nodiscard]] bool Valid() const {
		return !heights.empty() && !heightsL.empty() && w >= 2 && h >= 2;
	}
	void Destroy(RenderGraph& rg) {
		if (!heightsL.empty()) rg.FreeBuffer(heightsL);
		heightsL = {};
		heights = {};
		w = h = 0;
	}
	bool Upload(ResourceManager& ctx, RenderGraph& rg, const HeightfieldData& hf) {
		Destroy(rg);
		if (!hf.valid()) return false;
		if ((uint64_t)hf.width * hf.height > kPlacementMaxHfTexels) return false;
		heights = ctx.CreateBuffer(sizeof(float) * hf.heights.size(),
			BufferType_Storage, "placement heightfield");
		if (heights.empty()) {
			smvk::OnResourceExhaustion("placement heightfield buffer");
			return false;
		}
		heightsL = rg.AddExternalBuffer(heights, "placement heightfield");
		if (heightsL.empty()) {
			smvk::OnResourceExhaustion("placement heightfield logical");
			return false;
		}
		ctx.UploadDataToBuffer(heights, hf.heights.data(),
			sizeof(float) * hf.heights.size(),
			PipelineStageBit_Compute, AccessFlags::ShaderRead, 0, 0);
		w = hf.width; h = hf.height;
		return true;
	}
	uint64_t Address(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(heights);
	}
};

// The device-side PlacementDomainParams home for the scatter kernels
// (mesh + heightfield): one small buffer, uploaded per (layer, domain
// binding) change — NOT per frame. The scatter push addresses it by BDA
// (the 64B push budget carries nothing inline).
struct PlacementParamsGpu {
	Holder<BufferHandle> params;
	LogicalBufferHandle logical{};

	[[nodiscard]] bool Valid() const { return !params.empty() && !logical.empty(); }
	void Destroy(RenderGraph& rg) {
		if (!logical.empty()) rg.FreeBuffer(logical);
		logical = {};
		params = {};
	}
	bool Upload(ResourceManager& ctx, RenderGraph& rg, const PlacementDomainParams& p) {
		Destroy(rg);
		params = ctx.CreateBuffer(sizeof(PlacementDomainParams),
			BufferType_Storage, "placement scatter params");
		if (params.empty()) {
			smvk::OnResourceExhaustion("placement scatter params buffer");
			return false;
		}
		logical = rg.AddExternalBuffer(params, "placement scatter params");
		if (logical.empty()) {
			smvk::OnResourceExhaustion("placement scatter params logical");
			return false;
		}
		ctx.UploadDataToBuffer(params, &p, sizeof(p),
			PipelineStageBit_Compute, AccessFlags::ShaderRead, 0, 0);
		return true;
	}
	uint64_t Address(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(params);
	}
};

// Pure-CPU params fill (headless-testable; BDAs wired by the caller from
// the structs above + PlacementPass slot addresses). PR02: the program
// view carries the field SSA; an invalid program refuses (explicit CPU
// fallback, never a silent approximation). Grid layers use their own
// kernel/push (BuildTilePush) and refuse here, as before.
inline bool FillScatterParams(const PlacementSystem::LayerBinding& layer,
	const PlacementProgramView& program, const PlacementEcoView& eco,
	const ScatterBDAs& bdas,
	PlacementDomainParams& out) {
	if (!program.Valid()) return false;
	// The eco gate: a program that READS the painted layer refuses
	// when the LAYER carries no mask or the view is unresolved
	// (never a silent zero density).
	if (ProgramUsesEcotopeMask(layer.program)
		&& (!layer.ecotope || !eco.Valid())) return false;
	out = PlacementDomainParams{};
	out.densityScale = layer.config.densityScale;
	out.instrBDA = program.instrBDA;
	out.instrCount = program.instrCount;
	out.acceptIndex = program.acceptIndex;
	// The ecotope-mask tail (PR04: the painted layer).
	out.ecoBDA = eco.bda;
	out.ecoW = eco.w;
	out.ecoH = eco.h;
	out.ecoChannels = eco.channels;
	out.ecoOX = eco.ox; out.ecoOZ = eco.oz;
	out.ecoSX = eco.sx; out.ecoSZ = eco.sz;
	out.minScale = layer.config.minScale;
	out.maxScale = layer.config.maxScale;
	const size_t n = layer.prototypes.size() > 8 ? 8 : layer.prototypes.size();
	out.protoCount = (uint32_t)(n == 0 ? 1 : n);
	for (size_t i = 0; i < n; ++i) out.weights[i] = layer.prototypes.at(i).weight;
	if (n == 0) out.weights[0] = 1.0f;
	if (layer.domainKind == DomainKind::MeshSurface && layer.mesh) {
		out.domainKind = kPlacementScatterDomain_Mesh;
		out.triCount = (uint32_t)layer.mesh->triangleCount();
		uint32_t maxIdx = 0;
		for (uint32_t i : layer.mesh->indices) maxIdx = maxIdx > i ? maxIdx : i;
		out.vertCount = maxIdx + 1;
		if (!layer.mesh->normals.empty()) out.flags |= kPlacementFlag_HasNormals;
		if (!layer.mesh->uvs.empty()) out.flags |= kPlacementFlag_HasUVs;
		out.posBDA = bdas.pos; out.nrmBDA = bdas.nrm; out.uvBDA = bdas.uv;
		out.idxBDA = bdas.idx; out.probBDA = bdas.prob; out.aliasBDA = bdas.alias;
		return out.triCount > 0;
	}
	if (layer.domainKind == DomainKind::Heightfield && layer.heightfield
		&& layer.heightfield->field) {
		const HeightfieldData* hf = layer.heightfield->field;
		out.domainKind = kPlacementScatterDomain_Heightfield;
		out.heightsBDA = bdas.heights;
		out.hfW = hf->width; out.hfH = hf->height;
		out.hfOX = hf->origin.x; out.hfOZ = hf->origin.z;
		out.hfSX = hf->spacing.x; out.hfSZ = hf->spacing.y;
		out.extMinX = layer.heightfield->extentMin.x;
		out.extMinZ = layer.heightfield->extentMin.y;
		out.extMaxX = layer.heightfield->extentMax.x;
		out.extMaxZ = layer.heightfield->extentMax.y;
		return hf->valid();
	}
	return false;
}

inline bool BuildScatterPush(const PlacementSystem::LayerBinding& layer,
	TileID tile, uint64_t instancesBDA, uint64_t countBDA, uint64_t paramsBDA,
	PlacementScatterPush& out) {
	out = PlacementScatterPush{};
	out.outInstances = instancesBDA;
	out.outCount = countBDA;
	out.params = paramsBDA;
	out.seedLo = (uint32_t)(layer.program.graphSeed & 0xFFFFFFFFull);
	out.seedHi = (uint32_t)(layer.program.graphSeed >> 32);
	out.nodeId = layer.program.nodeId;
	out.candidateCount = layer.config.candidatesPerTile;
	out.tileX = tile.x; out.tileY = tile.y; out.tileZ = tile.z;
	return true;
}

// Scatter program management: mesh + heightfield tile kernels. Lives
// outside PlacementPass (which owns the tile slots) because domain data
// has its own lifetime (asset-cooked, shared across slots).
struct PlacementScatterPrograms {
	Holder<ShaderModuleHandle> meshModule, hfModule;
	ShaderProgram meshProgram{}, hfProgram{};

	[[nodiscard]] bool Valid() const {
		return meshProgram.shaderCount != 0 && hfProgram.shaderCount != 0
			&& !meshModule.empty() && !hfModule.empty();
	}
	bool Ensure(ResourceManager& ctx,
		const char* meshCsPath = kPlacementMeshTileCsPath,
		const char* hfCsPath = kPlacementHfTileCsPath) {
		if (Valid()) return true;
		try {
			if (meshModule.empty())
				meshModule = ctx.CreateShaderModule(
					readFile(meshCsPath ? meshCsPath : kPlacementMeshTileCsPath),
					"placementMeshTileCS");
			if (hfModule.empty())
				hfModule = ctx.CreateShaderModule(
					readFile(hfCsPath ? hfCsPath : kPlacementHfTileCsPath),
					"placementHfTileCS");
		} catch (...) {
			return false;
		}
		if (meshModule.empty() || hfModule.empty()) return false;
		Shader mcs{ .module = meshModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		meshProgram = { .shaders = { mcs }, .shaderCount = 1 };
		Shader hcs{ .module = hfModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		hfProgram = { .shaders = { hcs }, .shaderCount = 1 };
		return true;
	}
	void Destroy() {
		meshProgram = ShaderProgram();
		hfProgram = ShaderProgram();
		meshModule = {};
		hfModule = {};
	}
	// Scatter eval pass shell over a PlacementPass tile slot: slot buffers
	// RW + domain/params buffers read-only (rendergraph emits the edges).
	static RenderPass& AddScatterPass(RenderGraph& rg, const PlacementPass& pass,
		uint32_t slot, const std::vector<LogicalBufferHandle>& domainReads,
		LogicalBufferHandle paramsLogical, const char* name = "PlacementScatter") {
		assert(pass.Valid() && slot < pass.tileSlots);
		RenderPass& rp = rg.AddComputePass(name);
		BufferSyncRequest rw{};
		rw.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		rw.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(pass.instanceLogicals[slot], rw);
		rp.AddBufferHint(pass.countLogicals[slot], rw);
		BufferSyncRequest read{};
		read.barrierMasks.accessMask = AccessFlags::ShaderRead;
		read.barrierMasks.stageMask = PipelineStageBit_Compute;
		for (LogicalBufferHandle l : domainReads)
			if (!l.empty()) rp.AddBufferHint(l, read);
		if (!paramsLogical.empty()) rp.AddBufferHint(paramsLogical, read);
		return rp;
	}
	// Recording helper (mirrors PlacementPass::PushAndDispatch). Same
	// dispatch-setup contract: asserts the output/params BDAs and the
	// candidate count before any dispatch is recorded.
	static void PushAndDispatchScatter(CommandBuffer& cmd, ShaderProgram& program,
		const PlacementScatterPush& push) {
		if (push.candidateCount == 0) return;
		assert(push.outInstances != 0 && "placement scatter: null instances BDA");
		assert(push.outCount != 0 && "placement scatter: null count BDA");
		assert(push.params != 0 && "placement scatter: null params BDA");
		cmd.BindComputePipeline(program);
		cmd.PushConstants(0, sizeof(push), &push, ShaderStage::All);
		cmd.Dispatch(PlacementDispatchGroups(push.candidateCount), 1, 1);
	}
};

// ---- GPU min-distance constraint --------------------------------------
// Scratch + programs + pass shells for the build/elect/kill trio. One
// shared scratch set (tile-gen dispatches are sequential per queue).
// Consume contract: instances + flag2[].y (alive) masks; compaction to a
// dense list happens at consume time (readback filter or cull kernel).
struct PlacementConstraint {
	uint32_t maxInstances = kPlacementConMaxInstances;
	uint32_t maxCells = kPlacementConMaxCells;

	Holder<BufferHandle> headBuffer, nextBuffer, flag2Buffer, paramsBuffer;
	LogicalBufferHandle headLogical{}, nextLogical{}, flag2Logical{}, paramsLogical{};

	Holder<ShaderModuleHandle> buildModule, electModule, killModule;
	ShaderProgram buildProgram{}, electProgram{}, killProgram{};

	[[nodiscard]] bool Valid() const {
		return !headBuffer.empty() && !nextBuffer.empty() && !flag2Buffer.empty()
			&& !paramsBuffer.empty() && !headLogical.empty() && !nextLogical.empty()
			&& !flag2Logical.empty() && !paramsLogical.empty();
	}
	[[nodiscard]] bool ShadersValid() const {
		return buildProgram.shaderCount != 0 && electProgram.shaderCount != 0
			&& killProgram.shaderCount != 0 && !buildModule.empty()
			&& !electModule.empty() && !killModule.empty();
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* buildCsPath = kPlacementConBuildCsPath,
		const char* electCsPath = kPlacementConElectCsPath,
		const char* killCsPath = kPlacementConKillCsPath) {
		if (ShadersValid()) return true;
		try {
			if (buildModule.empty())
				buildModule = ctx.CreateShaderModule(
					readFile(buildCsPath ? buildCsPath : kPlacementConBuildCsPath),
					"placementConBuildCS");
			if (electModule.empty())
				electModule = ctx.CreateShaderModule(
					readFile(electCsPath ? electCsPath : kPlacementConElectCsPath),
					"placementConElectCS");
			if (killModule.empty())
				killModule = ctx.CreateShaderModule(
					readFile(killCsPath ? killCsPath : kPlacementConKillCsPath),
					"placementConKillCS");
		} catch (...) {
			return false;
		}
		if (buildModule.empty() || electModule.empty() || killModule.empty()) return false;
		Shader b{ .module = buildModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		buildProgram = { .shaders = { b }, .shaderCount = 1 };
		Shader e{ .module = electModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		electProgram = { .shaders = { e }, .shaderCount = 1 };
		Shader k{ .module = killModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		killProgram = { .shaders = { k }, .shaderCount = 1 };
		return true;
	}

	bool Create(ResourceManager& ctx, RenderGraph& rg,
		uint32_t maxInst = kPlacementConMaxInstances,
		uint32_t cells = kPlacementConMaxCells) {
		if (maxInst == 0 || cells == 0) return false;
		Destroy(rg);
		Holder<BufferHandle> head = ctx.CreateBuffer(sizeof(uint32_t) * cells,
			BufferType_Storage, "placement con head");
		Holder<BufferHandle> next = ctx.CreateBuffer(sizeof(uint32_t) * maxInst,
			BufferType_Storage, "placement con next");
		Holder<BufferHandle> flag2 = ctx.CreateBuffer(sizeof(uint32_t) * 2 * maxInst,
			BufferType_Storage, "placement con flags");
		Holder<BufferHandle> prm = ctx.CreateBuffer(sizeof(PlacementConParams),
			BufferType_Storage, "placement con params");
		if (head.empty() || next.empty() || flag2.empty() || prm.empty()) {
			smvk::OnResourceExhaustion("placement constraint buffers");
			return false;
		}
		LogicalBufferHandle headL = rg.AddExternalBuffer(head, "placement con head");
		LogicalBufferHandle nextL = rg.AddExternalBuffer(next, "placement con next");
		LogicalBufferHandle flag2L = rg.AddExternalBuffer(flag2, "placement con flags");
		LogicalBufferHandle prmL = rg.AddExternalBuffer(prm, "placement con params");
		if (headL.empty() || nextL.empty() || flag2L.empty() || prmL.empty()) {
			rg.FreeBuffer(headL); rg.FreeBuffer(nextL);
			rg.FreeBuffer(flag2L); rg.FreeBuffer(prmL);
			smvk::OnResourceExhaustion("placement constraint logicals");
			return false;
		}
		maxInstances = maxInst;
		maxCells = cells;
		headBuffer = std::move(head);
		nextBuffer = std::move(next);
		flag2Buffer = std::move(flag2);
		paramsBuffer = std::move(prm);
		headLogical = headL; nextLogical = nextL;
		flag2Logical = flag2L; paramsLogical = prmL;
		Clear(ctx);
		return EnsureShaders(ctx);
	}

	void Destroy(RenderGraph& rg) {
		for (LogicalBufferHandle l : { headLogical, nextLogical, flag2Logical, paramsLogical })
			if (!l.empty()) rg.FreeBuffer(l);
		headLogical = {}; nextLogical = {}; flag2Logical = {}; paramsLogical = {};
		headBuffer = {}; nextBuffer = {}; flag2Buffer = {}; paramsBuffer = {};
		// (ShaderProgram() - the paren value-init form: GCC rejects the
		// brace temporary; see the NOTE at struct ShaderProgram in smvk.h.)
		buildProgram = ShaderProgram(); electProgram = ShaderProgram(); killProgram = ShaderProgram();
		buildModule = {}; electModule = {}; killModule = {};
	}

	// Primes scratch: head=EMPTY, next=0, flag2=0 (~2MB uploads; tile-gen
	// cadence only, never per-frame).
	void Clear(ResourceManager& ctx) const {
		assert(Valid());
		std::vector<uint32_t> empty(maxCells, 0xFFFFFFFFu);
		ctx.UploadDataToBuffer(headBuffer, empty.data(), sizeof(uint32_t) * maxCells,
			PipelineStageBit_Transfer, AccessFlags::TransferWrite, 0, 0);
		std::vector<uint32_t> zeros(maxInstances * 2, 0);
		ctx.UploadDataToBuffer(nextBuffer, zeros.data(), sizeof(uint32_t) * maxInstances,
			PipelineStageBit_Transfer, AccessFlags::TransferWrite, 0, 0);
		ctx.UploadDataToBuffer(flag2Buffer, zeros.data(), sizeof(uint32_t) * maxInstances * 2,
			PipelineStageBit_Transfer, AccessFlags::TransferWrite, 0, 0);
	}

	void UploadParams(ResourceManager& ctx, const PlacementConParams& p) const {
		assert(Valid());
		ctx.UploadDataToBuffer(paramsBuffer, &p, sizeof(p),
			PipelineStageBit_Compute, AccessFlags::ShaderRead, 0, 0);
	}

	uint64_t HeadAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(headBuffer);
	}
	uint64_t NextAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(nextBuffer);
	}
	uint64_t Flag2Address(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(flag2Buffer);
	}
	uint64_t ParamsAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(paramsBuffer);
	}

	// Pass shells. Barrier edges (rendergraph-emitted):
	//   build: instances+countBuf+params read; head/next RW.
	//   elect: instances+head+next+countBuf+params read; flag2 RW.
	//   kill:  instances+head+next+flag2+countBuf+params read... flag2 is
	//          read (.x snapshot) AND written (.y) -> RW hint. The .x
	//          snapshot is stable because elect completed (pass edge) and
	//          kill never writes .x.
	static RenderPass& AddBuildPass(RenderGraph& rg, const PlacementConstraint& con,
		LogicalBufferHandle instancesLogical, LogicalBufferHandle countLogical,
		const char* name = "PlacementConBuild") {
		RenderPass& rp = rg.AddComputePass(name);
		BufferSyncRequest read{};
		read.barrierMasks.accessMask = AccessFlags::ShaderRead;
		read.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(instancesLogical, read);
		rp.AddBufferHint(countLogical, read);
		rp.AddBufferHint(con.paramsLogical, read);
		BufferSyncRequest rw{};
		rw.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		rw.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(con.headLogical, rw);
		rp.AddBufferHint(con.nextLogical, rw);
		return rp;
	}
	static RenderPass& AddElectPass(RenderGraph& rg, const PlacementConstraint& con,
		LogicalBufferHandle instancesLogical, LogicalBufferHandle countLogical,
		const char* name = "PlacementConElect") {
		RenderPass& rp = rg.AddComputePass(name);
		BufferSyncRequest read{};
		read.barrierMasks.accessMask = AccessFlags::ShaderRead;
		read.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(instancesLogical, read);
		rp.AddBufferHint(countLogical, read);
		rp.AddBufferHint(con.paramsLogical, read);
		rp.AddBufferHint(con.headLogical, read);
		rp.AddBufferHint(con.nextLogical, read);
		BufferSyncRequest rw{};
		rw.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		rw.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(con.flag2Logical, rw);
		return rp;
	}
	static RenderPass& AddKillPass(RenderGraph& rg, const PlacementConstraint& con,
		LogicalBufferHandle instancesLogical, LogicalBufferHandle countLogical,
		const char* name = "PlacementConKill") {
		RenderPass& rp = rg.AddComputePass(name);
		BufferSyncRequest read{};
		read.barrierMasks.accessMask = AccessFlags::ShaderRead;
		read.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(instancesLogical, read);
		rp.AddBufferHint(countLogical, read);
		rp.AddBufferHint(con.paramsLogical, read);
		rp.AddBufferHint(con.headLogical, read);
		rp.AddBufferHint(con.nextLogical, read);
		BufferSyncRequest rw{};
		rw.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		rw.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(con.flag2Logical, rw);
		return rp;
	}

	static void PushAndDispatchCon(CommandBuffer& cmd, ShaderProgram& program,
		const PlacementConPush& push, uint32_t count) {
		if (count == 0) return;
		cmd.BindComputePipeline(program);
		cmd.PushConstants(0, sizeof(push), &push, ShaderStage::All);
		cmd.Dispatch((count + 63) / 64, 1, 1);
	}

	// GPU eligibility: bounded grid-layer volumes with dims fitting the
	// scratch grid. Anything else (mesh overshoot, tiny spacings) falls
	// back to the CPU constraint — same result class, caller decides.
	static bool VolumeFitsGpu(const ConstraintVolume& vol) {
		return vol.dimX > 0 && vol.dimY > 0 && vol.dimZ > 0 && vol.spacing > 0.0f
			&& vol.dimX <= 64 && vol.dimY <= 64 && vol.dimZ <= 64
			&& (uint64_t)vol.dimX * vol.dimY * vol.dimZ <= kPlacementConMaxCells;
	}
	// Pure-CPU con params fill (headless-testable). The tile rides along:
	// the kernels RECOMPUTE StableIDs from (seed, node, tile, customB) —
	// a zeroed tile hashes the WRONG IDs and elects wrong cell winners
	// (caught on device by PlacementGpuTests' elect-mask parity).
	static bool FillConParams(const PlacementSystem::LayerBinding& layer,
		TileID tile, const ConstraintVolume& vol, PlacementConParams& out) {
		if (!VolumeFitsGpu(vol)) return false;
		out = PlacementConParams{};
		out.origin = vol.origin;
		out.spacing = vol.spacing;
		out.dimX = vol.dimX; out.dimY = vol.dimY; out.dimZ = vol.dimZ;
		out.seedLo = (uint32_t)(layer.program.graphSeed & 0xFFFFFFFFull);
		out.seedHi = (uint32_t)(layer.program.graphSeed >> 32);
		out.nodeId = layer.program.nodeId;
		out.tileX = tile.x; out.tileY = tile.y; out.tileZ = tile.z;
		return true;
	}
	static bool BuildConPush(uint64_t instancesBDA, uint64_t headBDA, uint64_t nextBDA,
		uint64_t flag2BDA, uint64_t paramsBDA, uint64_t countBDA, PlacementConPush& out) {
		out = PlacementConPush{};
		out.instances = instancesBDA;
		out.head = headBDA;
		out.next = nextBDA;
		out.flag2 = flag2BDA;
		out.params = paramsBDA;
		out.countBuf = countBDA;
		return true;
	}
};

} // namespace smvk::placement
