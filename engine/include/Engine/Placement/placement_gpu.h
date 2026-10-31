// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// GPU tile-evaluation backend (HZD-style runtime path).
// C++ mirror of engine/shaders/placement_tile.cs.hlsl push constants.
// The kernel evaluates: candidate → StableID → grid sample → density field
// (base + value-noise) → quantized accept → weighted prototype → transform,
// compacted via atomic append into a persistent per-tile instance buffer.
//
// Mesh-surface / heightfield domains + GPU min-distance arrive next;
// the CPU EvaluateTile() in placement_tile.h is the oracle for all paths.

#include <cstddef>
#include <cstdint>

#include <glm/glm.hpp>

#include <Engine/Placement/placement_point.h>

namespace tour::placement {

// Output buffer views (HLSL: vk::BufferPointer, see placement_tile.cs.hlsl).
struct PlacementInstanceBuffer {
	RenderInstance items[1]; // variable-length
};

// Must match HLSL PlacementCountBuffer (16 bytes) — the append-counter
// triple. count is the RAW atomic result (grows past capacity on
// overflow); consumers clamp with min(count, capacity) before any
// items[] access. overflow counts ACCEPTED candidates the guard dropped
// (> 0 = a capacity diagnostic, never a silent truncation); capacity is
// the items[] append bound PlacementPass::ClearTile primes before every
// dispatch (zero-on-creation: DEVICE_LOCAL pages recycle garbage).
struct PlacementCountGpu {
	uint32_t count = 0;
	uint32_t overflow = 0;
	uint32_t capacity = 0;
	uint32_t pad = 0;
};
static_assert(sizeof(PlacementCountGpu) == 16,
	"PlacementCountGpu size changed — update placement_common.hlsli");

// ---------------------------------------------------------------------------
// The GPU field-program upload (PR02: the kernels evaluate the ACTUAL
// graph SSA, never a density+noise shortcut).
//
// FieldInstr stays the CPU/compiler IR; this is the EXPLICIT device
// representation with stable 16-byte-aligned rows (never assume raw
// FieldInstr and an HLSL buffer view are interchangeable). Converted +
// uploaded once per (programId, revision) — PlacementProgramGpu caches
// it; never per frame.
// ---------------------------------------------------------------------------
inline constexpr uint32_t kPlacementMaxFieldInstrs = 64;

// Must match HLSL PlacementFieldInstr (placement_field.hlsli, 32 bytes:
// one uint4 row + one float4 row, 16-aligned).
struct alignas(16) GpuFieldInstr {
	uint32_t op = 0;        // FieldOp numeric value
	uint32_t a = 0;         // SSA operand index (validated: < i when used)
	uint32_t b = 0;
	uint32_t reserved = 0;
	float params[4] = { 0, 0, 0, 0 };
};
static_assert(sizeof(GpuFieldInstr) == 32, "GpuFieldInstr layout changed — update placement_field.hlsli");

// The descriptor the kernels carry (split: the BDA lives in the per-
// kernel push/params; count/accept ride with it).
struct PlacementProgramView {
	uint64_t instrBDA = 0;
	uint32_t instrCount = 0;
	uint32_t acceptIndex = 0;
	bool Valid() const { return instrBDA != 0 && instrCount > 0; }
};

// Validates a program for GPU evaluation. Checks: instruction cap,
// supported opcodes (op <= EcotopeMask — every FieldOp the HLSL
// evaluator implements), SSA operands strictly before their use (a/b
// < i for the ops that read them), acceptIndex in range, finite
// constants, and — when the program uses EcotopeMask — that the layer
// actually carries that mask channel (ecoChannels > params[0]; a
// maskless layer or an out-of-range paint channel refuses LOUDLY, not
// a silent zero density). On failure returns false and (when errorOut
// is non-null) a human-readable diagnostic naming the offending
// instruction. This is the gate against SILENT approximation: an
// invalid program NEVER reaches the kernels — the caller refuses GPU
// generation explicitly (CPU fallback or a loud error), never a
// density-0.5 substitution.
bool ValidateProgramForGpu(const PlacementProgram& prog,
	uint32_t ecoChannels, const char*& errorOut);

// The eco-free convenience form (programs without EcotopeMask ops).
inline bool ValidateProgramForGpu(const PlacementProgram& prog,
	const char*& errorOut) {
	return ValidateProgramForGpu(prog, 0, errorOut);
}

inline bool ValidateProgramForGpu(const PlacementProgram& prog,
	uint32_t ecoChannels, const char*& errorOut) {
	errorOut = nullptr;
	if (prog.instrs.empty()) { errorOut = "empty program"; return false; }
	if (prog.instrs.size() > kPlacementMaxFieldInstrs) {
		errorOut = "instruction count exceeds the GPU cap "
			"(kPlacementMaxFieldInstrs)";
		return false;
	}
	if (prog.acceptIndex >= prog.instrs.size()) {
		errorOut = "acceptIndex out of range";
		return false;
	}
	// Ops that READ the SSA operands (the input ops ignore a/b).
	auto usesA = [](uint32_t op) {
		return op >= 6 && op <= 14; // Add..Invert
	};
	auto usesB = [](uint32_t op) {
		return op >= 6 && op <= 9; // Add/Mul/Min/Max
	};
	for (size_t i = 0; i < prog.instrs.size(); ++i) {
		const FieldInstr& in = prog.instrs[i];
		const uint32_t op = static_cast<uint32_t>(in.op);
		if (op > 17) { // > FieldOp::EcotopeMask
			errorOut = "unsupported opcode (no HLSL evaluator form)";
			return false;
		}
		if (usesA(op) && in.a >= i) {
			errorOut = "SSA operand 'a' must reference an EARLIER value";
			return false;
		}
		if (usesB(op) && in.b >= i) {
			errorOut = "SSA operand 'b' must reference an EARLIER value";
			return false;
		}
		if (op == 17) { // EcotopeMask: params[0] = the paint channel
			const float chf = in.params[0];
			if (!(chf >= 0.0f) || chf != std::floor(chf)
				|| (uint32_t)chf >= ecoChannels) {
				errorOut = "EcotopeMask channel out of range for the "
					"layer's mask (or the layer carries no mask)";
				return false;
			}
		}
		for (int k = 0; k < 4; ++k)
			if (!std::isfinite(in.params[k])) {
				errorOut = "non-finite constant parameter";
				return false;
			}
	}
	return true;
}

// True when the program is GPU-evaluable (ValidateProgramForGpu with a
// discarded diagnostic — the quiet query form for call sites that log
// once and fall back to the CPU oracle).
inline bool ProgramIsGpuEvaluable(const PlacementProgram& prog) {
	const char* err = nullptr;
	return ValidateProgramForGpu(prog, err);
}

// Must match HLSL PlacementTilePush (168 bytes, 16-byte rows after the
// two 8-byte BDAs; compiled with -fvk-use-dx-layout).
//
// PR02: the density field itself moved INTO the uploaded program (the
// instrBDA tail) — the kernels evaluate the actual graph SSA and this
// push carries only the per-LAYER scalars (densityScale, prototype
// weights, scale range). The old density+noiseAmp shortcut slots are
// retired: density became densityScale; noiseAmp/noiseFreq must be 0
// (the program's Noise2 instructions are the noise source now).
struct PlacementTilePush {
	uint64_t outInstances = 0; // BDA of PlacementInstanceBuffer
	uint64_t outCount = 0;     // BDA of PlacementCountBuffer (count/overflow/capacity)
	uint32_t seedLo = 0;
	uint32_t seedHi = 0;
	uint32_t nodeId = 0;
	uint32_t candidateCount = 0;
	int32_t tileX = 0;
	int32_t tileY = 0;
	int32_t tileZ = 0;
	uint32_t pad0 = 0;
	glm::vec3 gridOrigin{ 0.0f };
	uint32_t cellsX = 8;
	glm::vec2 gridExtent{ 64.0f, 64.0f };
	uint32_t cellsZ = 8;
	uint32_t pad1 = 0;
	float densityScale = 1.0f;   // was: density (the const-extracted shortcut)
	float noiseAmpRetired = 0.0f;   // RETIRED: must stay 0 (program subsumes)
	float noiseFreqRetired = 0.0f;   // RETIRED: must stay 0
	uint32_t pad2 = 0;
	float minScale = 0.8f;
	float maxScale = 1.2f;
	uint32_t protoCount = 1; // <= 8
	uint32_t pad3 = 0;
	glm::vec4 weightsA{ 1, 0, 0, 0 }; // prototypes 0..3
	glm::vec4 weightsB{ 0, 0, 0, 0 }; // prototypes 4..7
	// ---- the program tail (PR02: the graph SSA) ----
	uint64_t instrBDA = 0;     // GpuFieldInstr[] upload
	uint32_t instrCount = 0;    // <= kPlacementMaxFieldInstrs
	uint32_t acceptIndex = 0;  // which value is the density
	uint32_t pad4 = 0;
	uint32_t pad5 = 0;
	// ---- the ecotope-mask tail (PR04: the painted layer) ----
	uint64_t ecoBDA = 0;       // float[channels*h*w] upload (0 = none)
	uint32_t ecoW = 0;
	uint32_t ecoH = 0;
	uint32_t ecoChannels = 0;
	uint32_t padEco = 0;
	float ecoOX = 0.0f, ecoOZ = 0.0f; // world XZ of texel (0,0)
	float ecoSX = 1.0f, ecoSZ = 1.0f; // meters per texel
};
static_assert(sizeof(PlacementTilePush) == 208, "PlacementTilePush size changed — update placement_tile.cs.hlsl");

inline constexpr uint32_t kPlacementTileThreads = 64;
inline uint32_t PlacementDispatchGroups(uint32_t candidateCount) {
	return (candidateCount + kPlacementTileThreads - 1u) / kPlacementTileThreads;
}

inline constexpr const char* kPlacementTileCsPath = "content/shaders/placement_tile.cs.hlsl.spv";
inline constexpr const char* kPlacementMeshTileCsPath = "content/shaders/placement_mesh_tile.cs.hlsl.spv";
inline constexpr const char* kPlacementHfTileCsPath = "content/shaders/placement_hf_tile.cs.hlsl.spv";
inline constexpr const char* kPlacementConBuildCsPath = "content/shaders/placement_con_build.cs.hlsl.spv";
inline constexpr const char* kPlacementConElectCsPath = "content/shaders/placement_con_elect.cs.hlsl.spv";
inline constexpr const char* kPlacementConKillCsPath = "content/shaders/placement_con_kill.cs.hlsl.spv";

inline constexpr uint32_t kPlacementConMaxInstances = 65536;
inline constexpr uint32_t kPlacementConMaxCells = 262144; // 64^3

// Must match placement_constraint.hlsli PlacementConParams (64 bytes).
struct PlacementConParams {
	glm::vec3 origin{ 0.0f };
	float spacing = 1.0f;
	uint32_t dimX = 0, dimY = 0, dimZ = 0;
	uint32_t pad0 = 0;
	uint32_t seedLo = 0, seedHi = 0, nodeId = 0;
	uint32_t pad1 = 0;
	int32_t tileX = 0, tileY = 0, tileZ = 0;
	uint32_t pad2 = 0;
};
static_assert(sizeof(PlacementConParams) == 64, "PlacementConParams moved — update placement_constraint.hlsli");

// Must match the constraint kernels' push (64 bytes).
struct PlacementConPush {
	uint64_t instances = 0;
	uint64_t head = 0;
	uint64_t next = 0;
	uint64_t flag2 = 0;
	uint64_t params = 0;
	uint64_t countBuf = 0;
	uint32_t pad[4] = {};
};
static_assert(sizeof(PlacementConPush) == 64, "PlacementConPush moved — update constraint kernels");

// ---- Mesh / heightfield scatter path ----------------------------------
// The grid kernel carries everything inline in its 144B push (engine
// precedent: FilterPush is 200B). Mesh/heightfield descriptors don't fit
// that budget, so they live in a device-side params struct addressed by
// a compact 64B push. Domain data layouts are padded for unambiguous
// CPU/GPU agreement: positions/normals as float4 arrays (never float3,
// whose HLSL array stride is layout-dependent), uvs float2, the rest
// scalars.

inline constexpr uint32_t kPlacementScatterDomain_Mesh = 1;
inline constexpr uint32_t kPlacementScatterDomain_Heightfield = 2;

inline constexpr uint32_t kPlacementFlag_HasNormals = 1u << 0;
inline constexpr uint32_t kPlacementFlag_HasUVs = 1u << 1;

// Must match placement_domains.hlsli PlacementDomainParams (192 bytes).
//
// PR02: the density moved into the uploaded program (instrBDA tail) —
// the params carry the per-LAYER scalars only. density became
// densityScale; noiseAmp/noiseFreq are RETIRED slots (must be 0 — the
// program's Noise2 instructions are the noise source).
struct PlacementDomainParams {
	float densityScale = 1.0f;    // was: density (the const-extracted shortcut)
	float noiseAmpRetired = 0.0f; // RETIRED: must stay 0
	float noiseFreqRetired = 0.0f; // RETIRED: must stay 0
	float minScale = 0.8f;
	float maxScale = 1.0f;
	uint32_t protoCount = 1;
	uint32_t padA = 0;
	float weights[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };
	uint32_t domainKind = 0;
	uint32_t triCount = 0;
	uint32_t vertCount = 0;
	uint32_t flags = 0;
	uint64_t posBDA = 0;   // float4[vertCount]
	uint64_t nrmBDA = 0;   // float4[vertCount], 0 when absent
	uint64_t uvBDA = 0;    // float2[vertCount], 0 when absent
	uint64_t idxBDA = 0;   // uint32[triCount*3]
	uint64_t probBDA = 0;  // float[triCount] alias keep-probability
	uint64_t aliasBDA = 0; // uint32[triCount] alias fallback
	uint64_t heightsBDA = 0; // float[hfW*hfH], heightfield only
	uint32_t hfW = 0;
	uint32_t hfH = 0;
	float hfOX = 0.0f, hfOZ = 0.0f; // world origin of texel (0,0)
	float hfSX = 1.0f, hfSZ = 1.0f; // meters per texel
	float extMinX = 0.0f, extMinZ = 0.0f; // scatter rect (world XZ)
	float extMaxX = 64.0f, extMaxZ = 64.0f;
	// ---- PR02 program tail (the graph SSA the kernels evaluate) ----
	uint64_t instrBDA = 0;    // GpuFieldInstr[] upload
	uint32_t instrCount = 0;  // <= kPlacementMaxFieldInstrs
	uint32_t acceptIndex = 0; // which value is the density
	// ---- PR04 ecotope-mask tail (the painted layer) ----
	uint64_t ecoBDA = 0;      // float[channels*h*w] upload (0 = none)
	uint32_t ecoW = 0;
	uint32_t ecoH = 0;
	uint32_t ecoChannels = 0;
	uint32_t padEco = 0;
	float ecoOX = 0.0f, ecoOZ = 0.0f; // world XZ of texel (0,0)
	float ecoSX = 1.0f, ecoSZ = 1.0f; // meters per texel
};
static_assert(sizeof(PlacementDomainParams) == 232, "PlacementDomainParams moved — update placement_domains.hlsli");

// True when the program carries EcotopeMask instructions (the layer
// MUST then carry a mask — the validation gate enforces the channel).
inline bool ProgramUsesEcotopeMask(const PlacementProgram& prog) {
	for (const FieldInstr& in : prog.instrs)
		if (in.op == FieldOp::EcotopeMask) return true;
	return false;
}

// The resolved ecotope-mask descriptor (the params/push tails' source).
struct PlacementEcoView {
	uint64_t bda = 0;
	uint32_t w = 0, h = 0, channels = 0;
	float ox = 0.0f, oz = 0.0f, sx = 1.0f, sz = 1.0f;
	bool Valid() const { return bda != 0 && channels > 0 && w >= 2 && h >= 2; }
};

// Must match the scatter kernels' push (64 bytes).
struct PlacementScatterPush {
	uint64_t outInstances = 0;
	uint64_t outCount = 0;
	uint64_t params = 0; // BDA of PlacementDomainParams
	uint64_t pad0 = 0;
	uint32_t seedLo = 0;
	uint32_t seedHi = 0;
	uint32_t nodeId = 0;
	uint32_t candidateCount = 0;
	int32_t tileX = 0, tileY = 0, tileZ = 0;
	uint32_t flags = 0;
};
static_assert(sizeof(PlacementScatterPush) == 64, "PlacementScatterPush moved — update scatter kernels");

// ---------------------------------------------------------------------------
// PR03: the GPU-visible binning path (per-view scratch over the
// persistent instance buffers — the instances NEVER move per camera
// frame; only the visible-index list does).
//
// Pipeline (all GPU, no readback on the frame path):
//   cull     — per instance: frustum/proto-sphere + alive + species-
//              distance tests -> bitpack mask + per-proto atomic counts
//   prefix   — one thread: counts -> exclusive offsets + total
//   scatter  — per instance: mask bit set -> atomic per-bin append ->
//              visible[offset[proto] + n] = source slot
//   drawbuild— per proto: write the indirect commands' dynamic halves
//              (instanceCount = counts[p], firstInstance = offsets[p]);
//              the CPU seeds the immutable halves (indexCount/
//              firstIndex/vertexOffset) once per prototype table
// The VS then fetches: visible[SV_InstanceID] -> source slot -> the
// 40-byte instance (SV_InstanceID includes the command's firstInstance
// under SPIR-V, so bin p's draw walks exactly its own visible range).
//
// Atomic within-bin order is arbitrary per frame (documented): the
// buffer position is NOT an identity — overrides key by StableID (the
// plan's rule); a stable candidate-index scatter is the refinement if
// deterministic within-bin order is ever needed.
// ---------------------------------------------------------------------------

// The per-(tile slot, view) bin slab (128 bytes).
struct PlacementBinsGpu {
	uint32_t counts[8] = {};     // cull's atomics: per-proto visible counts
	uint32_t offsets[8] = {};    // prefix's exclusive sums (bin base in visible[])
	uint32_t appendNext[8] = {}; // scatter's per-bin append cursors (re-zeroed)
	uint32_t total = 0;         // sum of counts
	uint32_t pad[7] = {};
};
static_assert(sizeof(PlacementBinsGpu) == 128,
	"PlacementBinsGpu moved — update placement_cull.hlsli");

// VkDrawIndexedIndirectCommand shape (the gfxfilter GfxIndirectCmd
// precedent). The CPU seeds indexCount/firstIndex/vertexOffset per
// prototype; the GPU writes instanceCount/firstInstance per frame.
struct PlacementIndirectCmd {
	uint32_t indexCount = 0;
	uint32_t instanceCount = 0;
	uint32_t firstIndex = 0;
	int32_t vertexOffset = 0;
	uint32_t firstInstance = 0;
};
static_assert(sizeof(PlacementIndirectCmd) == 20,
	"PlacementIndirectCmd moved — update placement_cull.hlsli");
inline constexpr uint32_t kPlacementIndirectStride = sizeof(PlacementIndirectCmd);
inline constexpr uint32_t kPlacementMaxProtos = 8;
// The HLSL view bound (PlacementInstanceBuffer.items[...]) — the C++
// spelling (placement_common.hlsli's PLACEMENT_MAX_TILE_INSTANCES).
inline constexpr uint32_t kPlacementMaxTileInstances = 65536;

// Cull flag bits (PlacementCullPush.cullFlags + the VS debug tail).
inline constexpr uint32_t kPlacementCullFrustum = 1;
inline constexpr uint32_t kPlacementCullAlive = 2;
inline constexpr uint32_t kPlacementCullDistance = 4;

// Must match placement_cull.hlsli PlacementCullPush (240 bytes, dx-layout).
// The kernels SELF-BOUND the live count from the count buffer
// (min(raw, capacity)); the dispatch width is the CPU-known capacity.
struct PlacementCullPush {
	uint64_t instances = 0;  // BDA of the tile's PlacementInstanceBuffer
	uint64_t count = 0;      // BDA of the tile's PlacementCountBuffer
	uint64_t mask = 0;       // BDA of the bitpacked verdict buffer
	uint64_t bins = 0;       // BDA of PlacementBinsGpu
	uint64_t alive = 0;      // BDA of flag2, 0 = skip the alive test
	uint64_t renderViews = 0; // BDA of the RenderView slab
	uint32_t renderViewIndex = 0;
	uint32_t cullFlags = kPlacementCullFrustum | kPlacementCullAlive;
	uint32_t protoCount = 1;
	uint32_t padD = 0;       // (was liveCount — self-bounded on device)
	// Per-proto conservative spheres (center.xyz, radius.w) from the
	// cooked mesh bounds — NOT a made-up guard factor; and per-proto max
	// draw distances (0 = unlimited).
	glm::vec4 protoBounds[kPlacementMaxProtos]{ glm::vec4(0.0f, 0.0f, 0.0f, 0.5f) };
	float protoDist[kPlacementMaxProtos] = {};
	glm::vec3 camPos{ 0.0f };    // species-distance origin (per-view CPU data)
	float maxInstanceScale = 1.0f; // radius scaling bound (config.maxScale)
};
static_assert(sizeof(PlacementCullPush) == 240,
	"PlacementCullPush moved — update placement_cull.hlsli");

inline constexpr const char* kPlacementCullCsPath = "content/shaders/placement_cull.cs.hlsl.spv";
inline constexpr const char* kPlacementPrefixCsPath = "content/shaders/placement_prefix.cs.hlsl.spv";
inline constexpr const char* kPlacementScatterVisCsPath = "content/shaders/placement_scatter_visible.cs.hlsl.spv";
inline constexpr const char* kPlacementDrawBuildCsPath = "content/shaders/placement_drawbuild.cs.hlsl.spv";

// Must match placement_scatter_visible.cs.hlsl PlacementScatterVisPush
// (48 bytes). The kernels SELF-BOUND from the count buffer
// (min(raw, capacity)); the dispatch width is the CPU-known capacity.
struct PlacementScatterVisPush {
	uint64_t instances = 0;
	uint64_t count = 0;
	uint64_t mask = 0;
	uint64_t bins = 0;
	uint64_t visible = 0;
	uint32_t protoCount = 1; // chain parity; unused by the kernel
	uint32_t pad0 = 0;
};
static_assert(sizeof(PlacementScatterVisPush) == 48,
	"PlacementScatterVisPush moved — update placement_scatter_visible.cs.hlsl");

// Must match placement_drawbuild.cs.hlsl PlacementDrawBuildPush
// (128 bytes). The prototype table rides the push (8 protos x
// {indexCount, firstIndex, vertexOffset} = the 96B tail) so the KERNEL
// owns the COMPLETE command: the stranded-upload-write class (an
// UPLOAD's bytes seeding a buffer a same-frame kernel re-writes leaves
// the upload's write scope stranded from the draw's barrier — Vulkan
// memory dependencies are not transitive; see todo.txt) cannot exist
// here BY CONSTRUCTION: the indirect buffer is single-scope-written
// (COMPUTE) per frame, so the draw's compute-write ->
// indirect-command-read barrier is always complete.
struct PlacementDrawBuildPush {
	uint64_t bins = 0;      // 0  PlacementBinsGpu BDA
	uint64_t indirect = 0;  // 8  PlacementIndirectBuffer BDA
	uint32_t protoCount = 1; // 16 live bins (<= the table below)
	uint32_t pad0 = 0;      // 20
	uint32_t pad1 = 0;      // 24
	uint32_t pad2 = 0;      // 28
	// The immutable command halves (the prototype table's mesh
	// addressing). instanceCount/firstInstance are the per-frame
	// dynamic halves the kernel derives from the bins. Entries past
	// the registered table stay zero (a zero indexCount draws nothing —
	// the graceful unseeded state).
	uint32_t indexCount[kPlacementMaxProtos] = {};  // 32
	uint32_t firstIndex[kPlacementMaxProtos] = {};  // 64
	int32_t vertexOffset[kPlacementMaxProtos] = {}; // 96
};
static_assert(sizeof(PlacementDrawBuildPush) == 128,
	"PlacementDrawBuildPush moved — update placement_drawbuild.cs.hlsl");
static_assert(offsetof(PlacementDrawBuildPush, indexCount) == 32
	&& offsetof(PlacementDrawBuildPush, firstIndex) == 64
	&& offsetof(PlacementDrawBuildPush, vertexOffset) == 96,
	"PlacementDrawBuildPush array offsets moved — update the HLSL mirror");

} // namespace tour::placement


// COMPAT (the namespace/RHI boundary audit, todo item 8.4): this
// placement header's declarations live in tour::placement; the smvk::
// placement aliases keep the existing call sites compiling through the
// consumer migration.
namespace smvk::placement {
	using ::tour::placement::kPlacementConMaxCells;
	using ::tour::placement::kPlacementConMaxInstances;
	using ::tour::placement::kPlacementCullAlive;
	using ::tour::placement::kPlacementCullDistance;
	using ::tour::placement::kPlacementCullFrustum;
	using ::tour::placement::kPlacementFlag_HasNormals;
	using ::tour::placement::kPlacementFlag_HasUVs;
	using ::tour::placement::kPlacementIndirectStride;
	using ::tour::placement::kPlacementMaxFieldInstrs;
	using ::tour::placement::kPlacementMaxProtos;
	using ::tour::placement::kPlacementMaxTileInstances;
	using ::tour::placement::kPlacementScatterDomain_Heightfield;
	using ::tour::placement::kPlacementScatterDomain_Mesh;
	using ::tour::placement::kPlacementTileThreads;
	using ::tour::placement::PlacementBinsGpu;
	using ::tour::placement::PlacementConParams;
	using ::tour::placement::PlacementConPush;
	using ::tour::placement::PlacementCountGpu;
	using ::tour::placement::PlacementCullPush;
	using ::tour::placement::PlacementDispatchGroups;
	using ::tour::placement::PlacementDomainParams;
	using ::tour::placement::PlacementDrawBuildPush;
	using ::tour::placement::PlacementEcoView;
	using ::tour::placement::PlacementIndirectCmd;
	using ::tour::placement::PlacementInstanceBuffer;
	using ::tour::placement::PlacementProgramView;
	using ::tour::placement::PlacementScatterPush;
	using ::tour::placement::PlacementScatterVisPush;
	using ::tour::placement::PlacementTilePush;
	using ::tour::placement::kPlacementTileCsPath;
	using ::tour::placement::kPlacementDrawBuildCsPath;
	using ::tour::placement::GpuFieldInstr;
	using ::tour::placement::ProgramIsGpuEvaluable;
	using ::tour::placement::ProgramUsesEcotopeMask;
	using ::tour::placement::ValidateProgramForGpu;
}
