// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// GPU MESH-particle system (PR06): spinning 3D mesh variants on the
// existing particle machinery's pattern (the GPU pool + free-list stack
// + the emitter fractional carry — the billboard system stays untouched
// and correct). The binning/indirect path reuses the PR03 chain's
// shapes (PlacementVisibleSet's mask/bins/visible/indirect buffers and
// the placement_cull.hlsli forms).
//
// Per frame (pass order matters; the rendergraph emits the barriers):
//   1. MeshParticlesEmit compute (one thread per emitter): rate*dt via
//      the GPU-OWNED carry, free-list pops, the MESH payload (a uniform
//      random quaternion, an angular velocity, the mesh bin, the
//      sizeStart->sizeEnd envelope, tint jitter).
//   2. MeshParticlesSim compute (one thread per pool slot): gravity,
//      aging, the QUATERNION SPIN integration, the size envelope, the
//      free-list recycle on death.
//   3. The visibility chain (cull -> prefix -> scatter -> drawbuild):
//      per-mesh bins, the visible-index list, and the GPU-written
//      indirect commands — the renderer records one DrawIndexedIndirect
//      per bin with the mesh's geometry/material (the immutable halves
//      seeded once per mesh table).
//
// No CPU per-particle work anywhere: the CPU only uploads emitter rows
// (the per-flight ring) and pushes BDAs + dt + gravity.

#include <Engine/Memory/pool.h>
#include <Engine/Placement/placement_gpu.h>
#include <Engine/Rendergraph/placement_pass.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <glm/glm.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace smvk {

// ---------------------------------------------------------------------------
// GPU mesh particle (must match HLSL MeshParticleGpu in
// meshparticles.hlsli — 96 bytes, 16-aligned). posLife.w <= 0 marks the
// slot dead (the sim recycles it; the cull mask keeps it out of the
// bins). params: x = 1/lifeMax, y = meshId, z = sizeStart, w = sizeEnd —
// the envelope endpoints persist so the per-frame size is the clean
// lerp, never a drifting value.
// ---------------------------------------------------------------------------
struct alignas(16) MeshParticleGpu {
	glm::vec4 posLife = glm::vec4(0.0f);    // 0  xyz world pos, w life remaining (s)
	glm::vec4 velSize = glm::vec4(0.0f);    // 16 xyz velocity (m/s), w CURRENT size (m)
	glm::vec4 rot = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f); // 32 quaternion (xyzw)
	glm::vec4 angVelGrav = glm::vec4(0.0f); // 48 xyz angular velocity (axis*rad/s), w gravityScale
	glm::vec4 colorEmit = glm::vec4(1.0f);  // 64 rgb tint, w emissive intensity
	glm::vec4 params = glm::vec4(0.0f);    // 80 x invLifeMax, y meshId, z sizeStart, w sizeEnd
};
static_assert(sizeof(MeshParticleGpu) == 96);
static_assert(sizeof(MeshParticleGpu) % 16 == 0);

// Must match HLSL MeshParticleEmitterGpu (128 bytes, 16-aligned).
struct alignas(16) MeshParticleEmitterGpu {
	glm::vec4 position = glm::vec4(0.0f, 0.0f, 0.0f, 0.1f); // 0  xyz center, w spawn radius
	glm::vec4 direction = glm::vec4(0.0f, 1.0f, 0.0f, 0.2f); // 16 xyz cone axis, w spread (rad)
	glm::vec4 velocity = glm::vec4(0.5f, 1.5f, 0.0f, 1.0f); // 32 speedMin, speedMax, drag(unused v1), gravityScale
	glm::vec4 life = glm::vec4(1.5f, 3.5f, 24.0f, 0.0f);   // 48 lifeMin, lifeMax, rate (1/s), meshId
	glm::vec4 spin = glm::vec4(1.0f, 4.0f, 0.0f, 0.1f);     // 64 angVelMin, angVelMax (rad/s), seed, scaleJitter
	glm::vec4 size = glm::vec4(0.4f, 0.15f, 0.25f, 0.0f);   // 80 sizeStart, sizeEnd (m), tintJitter, emissive
	glm::vec4 color = glm::vec4(1.0f, 0.8f, 0.5f, 0.0f);    // 96 rgb base tint, w unused
	glm::vec4 tail = glm::vec4(0.0f);                       // 112 carry (GPU-owned; rows upload 0)
};
static_assert(sizeof(MeshParticleEmitterGpu) == 128);
static_assert(sizeof(MeshParticleEmitterGpu) % 16 == 0);
static_assert(offsetof(MeshParticleEmitterGpu, tail) == 112);

struct MeshParticlesDesc {
	uint32_t maxParticles = 65536; // the pool bound (must match MESHPARTICLE_MAX)
	uint32_t maxEmitters = 16;     // must match MESHPARTICLE_MAX_EMITTERS
	uint32_t simThreads = 64;      // must match the sim CS numthreads
	const char* name = "mesh particles";
};

// Must match meshparticles.hlsli MeshParticlesEmitPush (48 bytes).
struct MeshParticlesEmitPush {
	uint64_t emitters = 0;      // 0  flight-slice emitter params BDA
	uint64_t emitterState = 0;  // 8  GPU carry array BDA
	uint64_t particles = 0;     // 16 pool BDA
	uint64_t freeList = 0;      // 24 free-list stack BDA
	uint32_t emitterCount = 0;  // 32 live emitters this frame
	uint32_t maxParticles = 0;  // 36
	float dt = 0.0f;            // 40
	uint32_t frameSeed = 0;     // 44 RNG stream salt (monotonic frame)
};
static_assert(sizeof(MeshParticlesEmitPush) == 48);

// Must match meshparticles.hlsli MeshParticlesSimPush (48 bytes).
struct MeshParticlesSimPush {
	uint64_t particles = 0;   // 0  pool BDA
	uint64_t freeList = 0;    // 8  free-list stack BDA
	float gravityX = 0.0f;    // 16 global gravity (m/s^2)
	float gravityY = -9.81f;  // 20
	float gravityZ = 0.0f;    // 24
	float dt = 0.0f;          // 28
	uint32_t maxParticles = 0; // 32
	uint32_t frameSeed = 0;   // 36 (unused by the spin)
	uint32_t pad0 = 0;        // 40
	uint32_t pad1 = 0;        // 44
};
static_assert(sizeof(MeshParticlesSimPush) == 48);

// Must match meshparticles.hlsli MeshParticlesCullPush (208 bytes).
struct MeshParticlesCullPush {
	uint64_t particles = 0;    // 0  pool BDA
	uint64_t mask = 0;         // 8  bitpack verdict BDA
	uint64_t bins = 0;         // 16 bin slab BDA
	uint64_t renderViews = 0;  // 24 the RenderView slab BDA
	uint32_t renderViewIndex = 0; // 32
	uint32_t cullFlags = 0;     // 36 the placement CULL_* bits
	uint32_t binCount = 1;      // 40 <= MESHPARTICLE_MAX_BINS
	uint32_t maxParticles = 0;  // 44 the pool bound (the dispatch width)
	uint32_t pad0 = 0;          // 48
	uint32_t pad1 = 0;          // 52
	uint32_t pad2 = 0;          // 56
	uint32_t pad3 = 0;          // 60
	glm::vec4 meshBounds[8]{
		glm::vec4(0.0f, 0.0f, 0.0f, 0.5f)}; // 64..192 per-mesh conservative spheres
	glm::vec3 camPos{ 0.0f };  // 192 (species distance unused v1)
	float padE = 0.0f;         // 204
};
static_assert(sizeof(MeshParticlesCullPush) == 208);

// Must match meshparticles.hlsli MeshParticlesScatterPush (48 bytes).
struct MeshParticlesScatterPush {
	uint64_t particles = 0;   // 0
	uint64_t mask = 0;        // 8
	uint64_t bins = 0;        // 16
	uint64_t visible = 0;     // 24
	uint32_t binCount = 1;    // 32
	uint32_t maxParticles = 0; // 36
	uint32_t pad0 = 0;        // 40
	uint32_t pad1 = 0;        // 44
};
static_assert(sizeof(MeshParticlesScatterPush) == 48);

// Must match meshparticles_prefix.cs.hlsl MeshParticlesPrefixPush (32B).
struct MeshParticlesPrefixPush {
	uint64_t bins = 0; // 0
	uint32_t binCount = 1; // 8
	uint32_t pad0 = 0; // 12
	uint32_t pad1 = 0; // 16
	uint32_t pad2 = 0; // 20
	uint32_t pad3 = 0; // 24
	uint32_t pad4 = 0; // 28
};
static_assert(sizeof(MeshParticlesPrefixPush) == 32);

// Must match meshparticles_draw.vert.hlsl MeshParticlesVsPush tail (32B
// at byte offset 192 — the placement-instanced overlay precedent).
struct MeshParticlesVsPush {
	uint64_t visible = 0;     // 192 the visible-index list BDA
	uint64_t particles = 0;   // 200 the pool BDA
	uint64_t renderViews = 0; // 208 the RenderView slab BDA
	uint32_t renderViewIndex = 0; // 216
	uint32_t flags = 0;       // 220 (reserved)
};
static_assert(sizeof(MeshParticlesVsPush) == 32);
inline constexpr uint32_t kMeshParticlesVsPushOffset = 192;

inline constexpr const char* kMeshParticlesEmitCsPath =
	"content/shaders/meshparticles_emit.cs.hlsl.spv";
inline constexpr const char* kMeshParticlesSimCsPath =
	"content/shaders/meshparticles_sim.cs.hlsl.spv";
inline constexpr const char* kMeshParticlesCullCsPath =
	"content/shaders/meshparticles_cull.cs.hlsl.spv";
inline constexpr const char* kMeshParticlesPrefixCsPath =
	"content/shaders/meshparticles_prefix.cs.hlsl.spv";
inline constexpr const char* kMeshParticlesScatterCsPath =
	"content/shaders/meshparticles_scatter.cs.hlsl.spv";
// The drawbuild pass reuses the PR03 chain's kernel verbatim (its push
// — bins/indirect/protoCount — is bin-agnostic).
inline constexpr uint32_t kMeshParticlesMaxBins = 8;
// The shader-side fixed bounds (meshparticles.hlsli's MESHPARTICLE_MAX /
// MESHPARTICLE_MAX_EMITTERS) — Create() rejects descriptors beyond them.
inline constexpr uint32_t kMeshParticlesMaxPool = 65536;
inline constexpr uint32_t kMeshParticlesMaxEmitters = 16;

struct MeshParticles {
	MeshParticlesDesc desc{};
	uint32_t framesInFlight = 1; // ring slices in emittersBuffer

	// GPU-owned persistent state (single copy; barriers serialize).
	Holder<BufferHandle> particlesBuffer;
	Holder<BufferHandle> freeListBuffer;
	Holder<BufferHandle> emitterStateBuffer;
	LogicalBufferHandle particlesLogical{};
	LogicalBufferHandle freeListLogical{};
	LogicalBufferHandle emitterStateLogical{};

	// Emitter parameter ring (one maxEmitters slice per flight).
	Holder<BufferHandle> emittersBuffer;
	LogicalBufferHandle emittersLogical{};

	// The per-frame chain scratch (mask/bins/visible/indirect — the
	// PR03 PlacementVisibleSet shapes, reused for mesh bins).
	Holder<BufferHandle> maskBuffer;
	Holder<BufferHandle> binsBuffer;
	Holder<BufferHandle> visibleBuffer;
	Holder<BufferHandle> indirectBuffer;
	LogicalBufferHandle maskLogical{}, binsLogical{}, visibleLogical{},
		indirectLogical{};

	Holder<ShaderModuleHandle> emitModule, simModule, cullModule,
		prefixModule, scatterModule, buildModule;
	ShaderProgram emitProgram{}, simProgram{}, cullProgram{},
		prefixProgram{}, scatterProgram{}, buildProgram{};
	// buildProgram reuses the PR03 chain's placement_drawbuild kernel
	// (its push — bins/indirect/protoCount — is bin-agnostic).

	[[nodiscard]] bool Valid() const {
		return !particlesBuffer.empty() && !freeListBuffer.empty()
			&& !emitterStateBuffer.empty() && !emittersBuffer.empty()
			&& !maskBuffer.empty() && !binsBuffer.empty()
			&& !visibleBuffer.empty() && !indirectBuffer.empty()
			&& !particlesLogical.empty() && !freeListLogical.empty()
			&& !emitterStateLogical.empty() && !emittersLogical.empty()
			&& !maskLogical.empty() && !binsLogical.empty()
			&& !visibleLogical.empty() && !indirectLogical.empty();
	}
	[[nodiscard]] bool ShadersValid() const {
		return emitProgram.shaderCount != 0 && simProgram.shaderCount != 0
			&& cullProgram.shaderCount != 0 && prefixProgram.shaderCount != 0
			&& scatterProgram.shaderCount != 0 && buildProgram.shaderCount != 0
			&& !emitModule.empty() && !simModule.empty() && !cullModule.empty()
			&& !prefixModule.empty() && !scatterModule.empty()
			&& !buildModule.empty();
	}

	// The CPU mirror of the emit CS spawn scheduling (the billboard
	// Particles::SpawnQuotaForFrame arithmetic — float-exact same ops).
	static uint32_t SpawnQuotaForFrame(float rate, float dt, float carryIn,
		float* carryOut = nullptr) {
		const float acc = carryIn + rate * dt;
		const float quota = std::floor(acc);
		const float carry = acc - quota;
		if (carryOut) *carryOut = carry;
		return quota > 0.0f ? (uint32_t)quota : 0u;
	}

	uint32_t SimGroupsFor(uint32_t maxParticles) const {
		return (maxParticles + desc.simThreads - 1u) / desc.simThreads;
	}

	// ---- lifecycle -----------------------------------------------------
	bool Create(ResourceManager& ctx, RenderGraph& rg,
		uint32_t framesInFlight, const MeshParticlesDesc& d = {});
	void Destroy(RenderGraph& rg);
	bool Ensure(ResourceManager& ctx, RenderGraph& rg,
		uint32_t framesInFlight, const MeshParticlesDesc& d = {});

	// Emitter rows for THIS frame (the flight slice). Pointer + count.
	// The rows' tail.x MUST be 0 (the GPU carry owns scheduling).
	void UpdateEmitters(ResourceManager& ctx, uint32_t frame,
		const MeshParticleEmitterGpu* rows, uint32_t count) const {
		assert(Valid() && count <= desc.maxEmitters);
		if (!rows || count == 0) return;
		const size_t slice = size_t(frame % framesInFlight);
		const size_t base = slice * size_t(desc.maxEmitters);
		ctx.UploadDataToBuffer(emittersBuffer, rows,
			sizeof(MeshParticleEmitterGpu) * count,
			PipelineStageBit_Compute, AccessFlags::ShaderRead,
			sizeof(MeshParticleEmitterGpu) * base, 0);
	}

	// ---- pass shells (the rendergraph emits the barriers) --------------
	RenderPass& AddEmitPass(RenderGraph& rg, const char* name = "MeshParticlesEmit") const {
		assert(Valid());
		RenderPass& rp = rg.AddComputePass(name);
		BufferSyncRequest rw{};
		rw.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		rw.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(emittersLogical, rw);
		rp.AddBufferHint(emitterStateLogical, rw);
		rp.AddBufferHint(particlesLogical, rw);
		rp.AddBufferHint(freeListLogical, rw);
		return rp;
	}
	RenderPass& AddSimPass(RenderGraph& rg, const char* name = "MeshParticlesSim") const {
		assert(Valid());
		RenderPass& rp = rg.AddComputePass(name);
		BufferSyncRequest rw{};
		rw.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		rw.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(particlesLogical, rw);
		rp.AddBufferHint(freeListLogical, rw);
		return rp;
	}
	// The chain: cull (pool read; mask/bins RW; the view slab read),
	// prefix (bins RW), scatter (pool/mask read; bins/visible RW),
	// drawbuild (bins read; indirect RW — the PR03 kernel reused).
	RenderPass& AddCullPass(RenderGraph& rg,
		LogicalBufferHandle renderViewsLogical,
		const char* name = "MeshParticlesCull") const {
		assert(Valid());
		RenderPass& rp = rg.AddComputePass(name);
		BufferSyncRequest read{};
		read.barrierMasks.accessMask = AccessFlags::ShaderRead;
		read.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(particlesLogical, read);
		if (!renderViewsLogical.empty()) rp.AddBufferHint(renderViewsLogical, read);
		BufferSyncRequest rw{};
		rw.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		rw.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(maskLogical, rw);
		rp.AddBufferHint(binsLogical, rw);
		return rp;
	}
	RenderPass& AddPrefixPass(RenderGraph& rg,
		const char* name = "MeshParticlesPrefix") const {
		RenderPass& rp = rg.AddComputePass(name);
		BufferSyncRequest rw{};
		rw.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		rw.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(binsLogical, rw);
		return rp;
	}
	RenderPass& AddScatterPass(RenderGraph& rg,
		const char* name = "MeshParticlesScatter") const {
		assert(Valid());
		RenderPass& rp = rg.AddComputePass(name);
		BufferSyncRequest read{};
		read.barrierMasks.accessMask = AccessFlags::ShaderRead;
		read.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(particlesLogical, read);
		rp.AddBufferHint(maskLogical, read);
		BufferSyncRequest rw{};
		rw.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		rw.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(binsLogical, rw);
		rp.AddBufferHint(visibleLogical, rw);
		return rp;
	}
	RenderPass& AddDrawBuildPass(RenderGraph& rg,
		const char* name = "MeshParticlesDrawBuild") const {
		RenderPass& rp = rg.AddComputePass(name);
		BufferSyncRequest read{};
		read.barrierMasks.accessMask = AccessFlags::ShaderRead;
		read.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(binsLogical, read);
		BufferSyncRequest rw{};
		rw.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		rw.barrierMasks.stageMask = PipelineStageBit_Compute;
		rp.AddBufferHint(indirectLogical, rw);
		return rp;
	}

	// ---- recording helpers ----------------------------------------------
	static void PushAndDispatchEmit(CommandBuffer& cmd, ShaderProgram& program,
		const MeshParticlesEmitPush& push) {
		assert(push.emitters != 0 && push.emitterState != 0
			&& push.particles != 0 && push.freeList != 0);
		cmd.BindComputePipeline(program);
		cmd.PushConstants(0, sizeof(push), &push, ShaderStage::All);
		cmd.Dispatch(push.emitterCount, 1, 1);
	}
	static void PushAndDispatchSim(CommandBuffer& cmd, ShaderProgram& program,
		const MeshParticlesSimPush& push, uint32_t groups) {
		assert(push.particles != 0 && push.freeList != 0 && groups > 0);
		cmd.BindComputePipeline(program);
		cmd.PushConstants(0, sizeof(push), &push, ShaderStage::All);
		cmd.Dispatch(groups, 1, 1);
	}
	static void PushAndDispatchCull(CommandBuffer& cmd, ShaderProgram& program,
		const MeshParticlesCullPush& push) {
		assert(push.particles != 0 && push.mask != 0 && push.bins != 0
			&& push.renderViews != 0);
		cmd.BindComputePipeline(program);
		cmd.PushConstants(0, sizeof(push), &push, ShaderStage::All);
		cmd.Dispatch((push.maxParticles + 63u) / 64u, 1, 1);
	}
	static void PushAndDispatchPrefix(CommandBuffer& cmd, ShaderProgram& program,
		const MeshParticlesPrefixPush& push) {
		assert(push.bins != 0);
		cmd.BindComputePipeline(program);
		cmd.PushConstants(0, sizeof(push), &push, ShaderStage::All);
		cmd.Dispatch(1, 1, 1);
	}
	static void PushAndDispatchScatter(CommandBuffer& cmd, ShaderProgram& program,
		const MeshParticlesScatterPush& push) {
		assert(push.particles != 0 && push.mask != 0 && push.bins != 0
			&& push.visible != 0);
		cmd.BindComputePipeline(program);
		cmd.PushConstants(0, sizeof(push), &push, ShaderStage::All);
		cmd.Dispatch((push.maxParticles + 63u) / 64u, 1, 1);
	}
	static void PushAndDispatchDrawBuild(CommandBuffer& cmd,
		ShaderProgram& program, const placement::PlacementDrawBuildPush& push) {
		assert(push.bins != 0 && push.indirect != 0);
		cmd.BindComputePipeline(program);
		cmd.PushConstants(0, sizeof(push), &push, ShaderStage::All);
		cmd.Dispatch(1, 1, 1);
	}

	// ---- addresses + the frame chain clear ------------------------------
	uint64_t ParticlesAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(particlesBuffer);
	}
	uint64_t FreeListAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(freeListBuffer);
	}
	uint64_t EmitterStateAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(emitterStateBuffer);
	}
	// The flight-slice emitter BDA (the slice this frame uploads).
	uint64_t EmittersSliceAddress(ResourceManager& ctx, uint32_t frame) const {
		const size_t slice = size_t(frame % framesInFlight);
		return ctx.GetBufferDeviceAddress(emittersBuffer)
			+ sizeof(MeshParticleEmitterGpu) * slice * size_t(desc.maxEmitters);
	}
	uint64_t MaskAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(maskBuffer);
	}
	uint64_t BinsAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(binsBuffer);
	}
	uint64_t VisibleAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(visibleBuffer);
	}
	uint64_t IndirectAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(indirectBuffer);
	}
	// The indirect buffer HANDLE (the renderer's DrawIndexedIndirect).
	BufferHandle IndirectHandle() const { return indirectBuffer; }

	// Per-frame chain zeroing: the mask + the bin atomics (the PR03
	// PlacementVisibleSet::ClearFrame contract — creation-zeroed once,
	// re-zeroed every frame BEFORE the cull records).
	void ClearFrameChain(ResourceManager& ctx) const {
		assert(Valid());
		const std::vector<uint32_t> zeros(
			(size_t(desc.maxParticles) + 31u) / 32u, 0u);
		ctx.UploadDataToBuffer(maskBuffer, zeros.data(),
			sizeof(uint32_t) * zeros.size(),
			PipelineStageBit_Transfer, AccessFlags::TransferWrite, 0, 0);
		placement::PlacementBinsGpu zeroBins{};
		ctx.UploadDataToBuffer(binsBuffer, &zeroBins, sizeof(zeroBins),
			PipelineStageBit_Transfer, AccessFlags::TransferWrite, 0, 0);
	}
	// Seeds the immutable indirect halves ONCE per mesh table (the
	// drawbuild pass rewrites instanceCount/firstInstance per frame).
	// Pointer + count (cmdCount <= kMeshParticlesMaxBins).
	bool SeedIndirect(ResourceManager& ctx,
		const placement::PlacementIndirectCmd* seeds, uint32_t cmdCount) const {
		assert(Valid());
		if (!seeds || cmdCount == 0 || cmdCount > kMeshParticlesMaxBins)
			return false;
		std::vector<placement::PlacementIndirectCmd> slab(
			kMeshParticlesMaxBins, placement::PlacementIndirectCmd{});
		for (uint32_t i = 0; i < cmdCount; ++i) {
			slab[i].indexCount = seeds[i].indexCount;
			slab[i].firstIndex = seeds[i].firstIndex;
			slab[i].vertexOffset = seeds[i].vertexOffset;
		}
		ctx.UploadDataToBuffer(indirectBuffer, slab.data(),
			placement::kPlacementIndirectStride * kMeshParticlesMaxBins,
			PipelineStageBit_Transfer, AccessFlags::TransferWrite, 0, 0);
		return true;
	}
};

// ---- lifecycle (out-of-line bodies; header-only repo convention) --------
inline bool MeshParticles::Create(ResourceManager& ctx, RenderGraph& rg,
	uint32_t framesInFlight, const MeshParticlesDesc& d) {
	if (d.maxParticles == 0 || d.maxEmitters == 0 || framesInFlight == 0)
		return false;
	// The SHADER-side fixed limits (meshparticles.hlsli): the pool is
	// indexed against MESHPARTICLE_MAX (65536) and the emitters loop is
	// capped at MESHPARTICLE_MAX_EMITTERS (16) — the emit kernel's
	// self-bound is `e >= MAX_EMITTERS`, so a larger host table would
	// silently drop emitters. Reject loudly instead.
	if (d.maxParticles > kMeshParticlesMaxPool
		|| d.maxEmitters > kMeshParticlesMaxEmitters)
		return false;
	Destroy(rg);
	// Programs: the five mesh kernels + the reused drawbuild.
	try {
		emitModule = ctx.CreateShaderModule(readFile(kMeshParticlesEmitCsPath),
			"meshParticlesEmitCS");
		simModule = ctx.CreateShaderModule(readFile(kMeshParticlesSimCsPath),
			"meshParticlesSimCS");
		cullModule = ctx.CreateShaderModule(readFile(kMeshParticlesCullCsPath),
			"meshParticlesCullCS");
		prefixModule = ctx.CreateShaderModule(
			readFile(kMeshParticlesPrefixCsPath), "meshParticlesPrefixCS");
		scatterModule = ctx.CreateShaderModule(
			readFile(kMeshParticlesScatterCsPath), "meshParticlesScatterCS");
		buildModule = ctx.CreateShaderModule(
			readFile(placement::kPlacementDrawBuildCsPath),
			"meshParticlesDrawBuildCS");
	} catch (...) {
		return false;
	}
	if (emitModule.empty() || simModule.empty() || cullModule.empty()
		|| prefixModule.empty() || scatterModule.empty() || buildModule.empty())
		return false;
	{
		Shader e{ .module = emitModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		emitProgram = { .shaders = { e }, .shaderCount = 1 };
		Shader s{ .module = simModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		simProgram = { .shaders = { s }, .shaderCount = 1 };
		Shader c{ .module = cullModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		cullProgram = { .shaders = { c }, .shaderCount = 1 };
		Shader p{ .module = prefixModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		prefixProgram = { .shaders = { p }, .shaderCount = 1 };
		Shader sc{ .module = scatterModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		scatterProgram = { .shaders = { sc }, .shaderCount = 1 };
		Shader b{ .module = buildModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		buildProgram = { .shaders = { b }, .shaderCount = 1 };
	}

	// The pool + the free list (creation-zeroed with the FULL stack —
	// every slot free — the AGENTS zero-on-creation convention for
	// buffers an atomic pops from on first use).
	particlesBuffer = ctx.CreateBuffer(sizeof(MeshParticleGpu) * d.maxParticles,
		BufferType_Storage, "mesh particles pool");
	freeListBuffer = ctx.CreateBuffer(
		sizeof(uint32_t) * (size_t(d.maxParticles) + 1u),
		BufferType_Storage, "mesh particles free list");
	emitterStateBuffer = ctx.CreateBuffer(sizeof(float) * d.maxEmitters,
		BufferType_Storage, "mesh particles carry");
	emittersBuffer = ctx.CreateBuffer(
		sizeof(MeshParticleEmitterGpu) * size_t(d.maxEmitters) * framesInFlight,
		BufferType_Storage, "mesh particles emitters");
	maskBuffer = ctx.CreateBuffer(
		sizeof(uint32_t) * ((size_t(d.maxParticles) + 31u) / 32u),
		BufferType_Storage, "mesh particles mask");
	binsBuffer = ctx.CreateBuffer(sizeof(placement::PlacementBinsGpu),
		BufferType_Storage, "mesh particles bins");
	visibleBuffer = ctx.CreateBuffer(sizeof(uint32_t) * d.maxParticles,
		BufferType_Storage, "mesh particles visible");
	indirectBuffer = ctx.CreateBuffer(
		placement::kPlacementIndirectStride * kMeshParticlesMaxBins,
		BufferType_Indirect, "mesh particles indirect");
	if (particlesBuffer.empty() || freeListBuffer.empty()
		|| emitterStateBuffer.empty() || emittersBuffer.empty()
		|| maskBuffer.empty() || binsBuffer.empty() || visibleBuffer.empty()
		|| indirectBuffer.empty()) {
		OnResourceExhaustion("mesh particles buffers");
		return false;
	}
	particlesLogical = rg.AddExternalBuffer(particlesBuffer, "mesh particles pool");
	freeListLogical = rg.AddExternalBuffer(freeListBuffer, "mesh particles free list");
	emitterStateLogical = rg.AddExternalBuffer(emitterStateBuffer, "mesh particles carry");
	emittersLogical = rg.AddExternalBuffer(emittersBuffer, "mesh particles emitters");
	maskLogical = rg.AddExternalBuffer(maskBuffer, "mesh particles mask");
	binsLogical = rg.AddExternalBuffer(binsBuffer, "mesh particles bins");
	visibleLogical = rg.AddExternalBuffer(visibleBuffer, "mesh particles visible");
	indirectLogical = rg.AddExternalBuffer(indirectBuffer, "mesh particles indirect");
	if (particlesLogical.empty() || freeListLogical.empty()
		|| emitterStateLogical.empty() || emittersLogical.empty()
		|| maskLogical.empty() || binsLogical.empty() || visibleLogical.empty()
		|| indirectLogical.empty()) {
		OnResourceExhaustion("mesh particles logicals");
		return false;
	}

	// Creation-zero the atomics + the pool: the free list's FULL stack
	// (slots[0..N) = 0..N-1, count = N — every slot free), the carry at
	// 0, the pool dead (posLife.w = 0), and the chain's mask/bins zero.
	// desc is assigned BEFORE this block: ClearFrameChain() sizes its
	// mask upload from desc.maxParticles — with the assignment after,
	// the initial clear used the DEFAULT 65536 against a pool-sized mask
	// buffer (an over-sized upload: 8 KB against a 512-byte buffer at
	// maxParticles 4096 — the review's finding).
	desc = d;
	this->framesInFlight = framesInFlight;
	{
		std::vector<uint32_t> stack(size_t(d.maxParticles) + 1u, 0u);
		stack[0] = d.maxParticles;
		for (uint32_t i = 0; i < d.maxParticles; ++i)
			stack[size_t(i) + 1u] = i;
		ctx.UploadDataToBuffer(freeListBuffer, stack.data(),
			sizeof(uint32_t) * stack.size(),
			PipelineStageBit_Transfer, AccessFlags::TransferWrite, 0, 0);
		const std::vector<float> zerosCarry(d.maxEmitters, 0.0f);
		ctx.UploadDataToBuffer(emitterStateBuffer, zerosCarry.data(),
			sizeof(float) * zerosCarry.size(),
			PipelineStageBit_Transfer, AccessFlags::TransferWrite, 0, 0);
		const std::vector<MeshParticleGpu> dead(d.maxParticles,
			MeshParticleGpu{}); // posLife.w = 0 = dead
		ctx.UploadDataToBuffer(particlesBuffer, dead.data(),
			sizeof(MeshParticleGpu) * dead.size(),
			PipelineStageBit_Transfer, AccessFlags::TransferWrite, 0, 0);
		ClearFrameChain(ctx);
	}
	return true;
}

inline void MeshParticles::Destroy(RenderGraph& rg) {
	for (LogicalBufferHandle l : { particlesLogical, freeListLogical,
			emitterStateLogical, emittersLogical, maskLogical, binsLogical,
			visibleLogical, indirectLogical })
		if (!l.empty()) rg.FreeBuffer(l);
	particlesLogical = {}; freeListLogical = {}; emitterStateLogical = {};
	emittersLogical = {}; maskLogical = {}; binsLogical = {};
	visibleLogical = {}; indirectLogical = {};
	particlesBuffer = {}; freeListBuffer = {}; emitterStateBuffer = {};
	emittersBuffer = {}; maskBuffer = {}; binsBuffer = {};
	visibleBuffer = {}; indirectBuffer = {};
	// (the paren value-init forms: the GCC 13 brace-temporary ICE)
	emitProgram = ShaderProgram(); simProgram = ShaderProgram();
	cullProgram = ShaderProgram(); prefixProgram = ShaderProgram();
	scatterProgram = ShaderProgram(); buildProgram = ShaderProgram();
	emitModule = {}; simModule = {}; cullModule = {};
	prefixModule = {}; scatterModule = {}; buildModule = {};
}

inline bool MeshParticles::Ensure(ResourceManager& ctx, RenderGraph& rg,
	uint32_t framesInFlight, const MeshParticlesDesc& d) {
	const bool shapeOk = Valid() && framesInFlight == this->framesInFlight
		&& desc.maxParticles == d.maxParticles && desc.maxEmitters == d.maxEmitters;
	if (shapeOk) return true;
	return Create(ctx, rg, framesInFlight, d);
}

} // namespace smvk
