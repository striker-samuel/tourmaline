// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// GPU particle system builder (compute emitters + simulation + billboard draw).
//
// Ports the Forward+ / ExposureMeter builder pattern to a fully GPU-resident
// particle pool: no CPU per-particle work exists anywhere. The CPU only
// uploads emitter parameter rows (per-flight ring, like the decal ring) and
// pushes buffer-device-addresses + dt.
//
// Per frame (pass order matters; the rendergraph emits the barriers):
//   1. ParticleEmit compute (particles_emit.cs.hlsl): one thread per
//      emitter. Each thread turns its rate*dt into an integer spawn quota
//      using a GPU-OWNED fractional carry (emitterState buffer, persistent,
//      never uploaded by the CPU after creation), pops quota slots from a
//      GPU free-list stack (atomic) and initializes them (shape-cone/
//      sphere/point spawn position, cone velocity, life, tint jitter,
//      rotation) with a per-spawn PCG RNG.
//   2. ParticleSim compute (particles_sim.cs.hlsl): one thread per pool
//      slot. Alive particles integrate velocity (per-emitter drag +
//      gravityScale over the global gravity push), age, spin; dead-on-this-
//      frame slots are pushed back onto the free-list (atomic).
//   3. ParticlesDraw graphics (particles_draw.vert/frag.hlsl): camera-
//      facing quads expanded in the VS from SV_VertexID/SV_InstanceID (no
//      vertex buffer; dead slots collapse to off-screen degenerates, so the
//      draw is a fixed Draw(6, maxParticles) with zero CPU involvement).
//      Additive HDR blend against the resolved HDR target, depth-tested
//      read-only against the prepass depth. The fragment shader lights each
//      particle through the Forward+ per-tile light lists — same tile
//      fetch as default_lit_body.hlsli — plus a brute-force fallback loop
//      when the tile lists are not fresh (deferred-shadow pipeline owns
//      the lights that frame).
//
// Emitter carry: rate*dt is fractional, so exact scheduling needs a
// persistent accumulator. It lives on the GPU (emitterStateBuffer) so a
// parameter upload can never clobber it; the CPU-side emitter rows carry a
// `tail.x` field the CPU leaves at 0.
//
// Buffers (all BufferType_Storage, device-local, BDA-addressed):
//   particles    – ParticleGpu * maxParticles, single GPU-owned copy
//                  (state persists across frames; barriers serialize).
//   freeList     – uint32[maxParticles + 1]: [0] = live entry count,
//                  [1..] = slot stack. Initialized once at creation.
//   emitters     – ParticleEmitterGpu * maxEmitters * framesInFlight ring,
//                  CPU-uploaded every frame (flight slice like fpLights).
//   emitterState – float * maxEmitters GPU-owned carry array.
//
// Usage (app/src/world_renderer.cpp):
//   smvk::Particles particles;
//   particles.Ensure(ctx, rg, framesInFlight, desc);   // resize-safe
//   particles.UpdateEmitters(ctx, frame, rows, count); // ring upload
//   auto& emit = particles.AddEmitPass(rg);
//   emit.SetRecordingFunc(... push ParticlesEmitPush, Dispatch(maxEmitters,1,1));
//   auto& sim = particles.AddSimPass(rg);
//   sim.SetRecordingFunc(... Dispatch(SimGroupsFor(...),1,1));
//   auto& draw = particles.AddDrawPass(rg, hdrView, depthView, extent);
//   Particles::AddDrawLightHints(draw, fpLightsLogical, tileCounts, tileIndices);

#include <Engine/Memory/pool.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/platform.h>

#include <glm/glm.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

namespace smvk {

// ---------------------------------------------------------------------------
// GPU particle (must match HLSL ParticleGpu in particles.hlsli).
// 64 bytes, 16-byte aligned.
//
// posLife.w <= 0 marks the slot dead (the VS collapses it to a degenerate
// quad and the sim pass pushes the slot back onto the free-list).
// params: x = 1/lifeMax (fade/size normalization), y = emitter index,
// z = rotation speed (rad/s), w = spawn seed (kept for future flicker).
// ---------------------------------------------------------------------------
struct alignas(16) ParticleGpu {
	glm::vec4 posLife = glm::vec4(0.0f);   // 0  xyz world pos, w life remaining (s)
	glm::vec4 velSize = glm::vec4(0.0f);   // 16 xyz velocity (m/s), w unused
	glm::vec4 colorRot = glm::vec4(0.0f);  // 32 rgb spawn tint, w rotation (rad)
	glm::vec4 params = glm::vec4(0.0f);    // 48 x invLifeMax, y emitterIdx, z rotSpeed, w seed
};
static_assert(sizeof(ParticleGpu) == 64);
static_assert(sizeof(ParticleGpu) % 16 == 0);

// Spawn-volume shapes (ParticleEmitterGpu::life.w).
static constexpr uint32_t kParticleShape_Cone = 0;   // disc at center, velocity in a cone around the axis
static constexpr uint32_t kParticleShape_Sphere = 1; // uniform in the ball, velocity outward random
static constexpr uint32_t kParticleShape_Point = 2;  // center point, velocity cone around the axis

// ---------------------------------------------------------------------------
// GPU emitter (must match HLSL ParticleEmitterGpu in particles.hlsli).
// 128 bytes, 16-byte aligned.
//
// position:  xyz world center, w spawn radius (m; cone = base disc radius,
//            sphere = ball radius, point ignores).
// direction: xyz cone axis (normalized at upload), w spread (rad, 0 = tight
//            along the axis; the cone solid angle is spread^2 weighted).
// color:     rgb base color, w emissive intensity (self-lit HDR term).
// velocity:  x speedMin, y speedMax (m/s), z drag (1/s, exponential),
//            w gravityScale (multiplies the global sim gravity; negative =
//            buoyant embers).
// size:      x sizeStart, y sizeEnd (m, quad half-extent lerp over life),
//            z rotSpeed (rad/s, random sign per particle), w RNG seed.
// life:      x lifeMin, y lifeMax (s), z rate (particles/s), w shape
//            (kParticleShape_*).
// fade:      x fadeIn (fraction of life), y fadeOut (from the end),
//            z colorJitter (per-channel spawn tint variation 0..1),
//            w AMBIENT wrap half-extent (m): 0 = a plain world emitter;
//            > 0 = camera-box mode - the CPU recenters the spawn volume
//            on the camera every frame and the sim wraps each particle
//            toroidally into the box (a permanently populated volume for
//            floating dust / rain: what leaves through one side re-enters
//            through the opposite - rain falls out the bottom and
//            re-enters at the top, dust drifts forever).
// tail:      x = GPU-OWNED fractional spawn carry. Uploaded rows must leave
//            it 0; the emit compute owns it via the separate emitterState
//            buffer (kept in the struct only for layout symmetry with HLSL).
// ---------------------------------------------------------------------------
struct alignas(16) ParticleEmitterGpu {
	glm::vec4 position = glm::vec4(0.0f, 0.0f, 0.0f, 0.1f);   // 0
	glm::vec4 direction = glm::vec4(0.0f, 1.0f, 0.0f, 0.2f);  // 16
	glm::vec4 color = glm::vec4(1.0f, 0.6f, 0.25f, 4.0f);     // 32
	glm::vec4 velocity = glm::vec4(0.5f, 1.5f, 0.2f, 1.0f);   // 48
	glm::vec4 size = glm::vec4(0.02f, 0.06f, 1.5f, 0.0f);     // 64
	glm::vec4 life = glm::vec4(1.5f, 3.5f, 60.0f,
		static_cast<float>(kParticleShape_Cone));             // 80
	glm::vec4 fade = glm::vec4(0.08f, 0.35f, 0.25f, 0.0f);    // 96
	glm::vec4 tail = glm::vec4(0.0f);                         // 112 carry (GPU-owned) + pads
};
static_assert(sizeof(ParticleEmitterGpu) == 128);
static_assert(sizeof(ParticleEmitterGpu) % 16 == 0);
static_assert(offsetof(ParticleEmitterGpu, tail) == 112);

struct ParticlesDesc {
	uint32_t maxParticles = 65536; // must match MAX_PARTICLES (particles.hlsli)
	uint32_t maxEmitters = 16;     // must match MAX_PARTICLE_EMITTERS
	uint32_t simThreads = 64;      // must match the sim CS numthreads
	const char* name = "particles";
};

// Must match particles_emit.cs.hlsl ParticlesEmitPush (48 bytes).
struct ParticlesEmitPush {
	uint64_t emitters = 0;      // 0  flight-slice emitter params BDA
	uint64_t emitterState = 0;  // 8  GPU carry array BDA
	uint64_t particles = 0;     // 16 pool BDA
	uint64_t freeList = 0;      // 24 free-list stack BDA
	uint32_t emitterCount = 0;  // 32 live emitters this frame
	uint32_t maxParticles = 0;  // 36
	float dt = 0.0f;            // 40
	uint32_t frameSeed = 0;     // 44 RNG stream salt (monotonic frame)
};
static_assert(sizeof(ParticlesEmitPush) == 48);

// Must match particles_sim.cs.hlsl ParticlesSimPush (48 bytes).
struct ParticlesSimPush {
	uint64_t particles = 0;   // 0  pool BDA
	uint64_t freeList = 0;    // 8  free-list stack BDA
	uint64_t emitters = 0;    // 16 flight-slice emitter params BDA (drag/gravityScale)
	float gravityX = 0.0f;    // 24 global gravity (m/s^2)
	float gravityY = -9.81f;  // 28
	float gravityZ = 0.0f;    // 32
	float dt = 0.0f;          // 36
	uint32_t maxParticles = 0; // 40
	uint32_t pad0 = 0;        // 44
	// Ambient (camera-box) wrapping: particles of an emitter with
	// fade.w > 0 toroidally wrap into this box around the camera (see
	// particles_sim.cs.hlsl). Always pushed (harmless for world emitters).
	float cameraX = 0.0f;     // 48 camera world pos (ambient wrap center)
	float cameraY = 0.0f;     // 52
	float cameraZ = 0.0f;     // 56
	uint32_t pad1 = 0;       // 60
};
static_assert(sizeof(ParticlesSimPush) == 64);

// Must match particles_draw.vert/frag.hlsl ParticlesDrawPush (80 bytes).
// fpTileCountX == 0 tells the fragment shader the tile lists are NOT fresh
// this frame (deferred-shadow pipeline owns the lights): it then falls back
// to a capped brute-force loop over the light buffer.
struct ParticlesDrawPush {
	uint64_t frameUniforms = 0; // 0
	uint64_t particles = 0;     // 8  pool BDA
	uint64_t emitters = 0;      // 16 flight-slice emitter params BDA
	uint64_t fpLights = 0;      // 24 ForwardPlusLightBuffer BDA
	uint64_t fpTileCounts = 0;  // 32
	uint64_t fpTileIndices = 0; // 40
	uint32_t fpTileCountX = 0;  // 48 0 = brute-force light fallback
	uint32_t fpLightCount = 0;  // 52
	uint32_t fpTileSize = 0;    // 56
	uint32_t fpMaxPerTile = 0;  // 60
	uint32_t maxParticles = 0;  // 64 instance cap for the VS guard
	uint32_t pad0 = 0;          // 68
	uint32_t pad1 = 0;          // 72
	uint32_t pad2 = 0;          // 76
};
static_assert(sizeof(ParticlesDrawPush) == 80);

inline constexpr const char* kParticlesEmitCsPath =
	"content/shaders/particles_emit.cs.hlsl.spv";
inline constexpr const char* kParticlesSimCsPath =
	"content/shaders/particles_sim.cs.hlsl.spv";
inline constexpr const char* kParticlesDrawVsPath =
	"content/shaders/particles_draw.vert.hlsl.spv";
inline constexpr const char* kParticlesDrawPsPath =
	"content/shaders/particles_draw.frag.hlsl.spv";

struct Particles {
	ParticlesDesc desc{};
	uint32_t framesInFlight = 1; // ring slices in emittersBuffer

	// GPU-owned persistent state (single copy; rendergraph barriers
	// serialize producer/consumer passes across frames).
	Holder<BufferHandle> particlesBuffer;
	Holder<BufferHandle> freeListBuffer;
	Holder<BufferHandle> emitterStateBuffer;
	LogicalBufferHandle particlesLogical{};
	LogicalBufferHandle freeListLogical{};
	LogicalBufferHandle emitterStateLogical{};

	// Emitter parameter ring: one ParticleEmitterGpu[maxEmitters] slice per
	// flight frame, CPU-uploaded every frame (live sliders). The GPU carry
	// lives in emitterState, so re-uploads never clobber spawn scheduling.
	Holder<BufferHandle> emittersBuffer;
	LogicalBufferHandle emittersLogical{};

	// Programs (owned): emit CS, sim CS, billboard draw VS+PS.
	Holder<ShaderModuleHandle> emitModule;
	Holder<ShaderModuleHandle> simModule;
	Holder<ShaderModuleHandle> drawVsModule;
	Holder<ShaderModuleHandle> drawPsModule;
	ShaderProgram emitProgram{};
	ShaderProgram simProgram{};
	ShaderProgram drawProgram{};

	[[nodiscard]] bool Valid() const {
		return !particlesBuffer.empty() && !freeListBuffer.empty()
			&& !emitterStateBuffer.empty() && !emittersBuffer.empty()
			&& !particlesLogical.empty() && !freeListLogical.empty()
			&& !emitterStateLogical.empty() && !emittersLogical.empty();
	}
	[[nodiscard]] bool ShadersValid() const {
		return emitProgram.shaderCount != 0 && simProgram.shaderCount != 0
			&& drawProgram.shaderCount != 0 && !emitModule.empty()
			&& !simModule.empty() && !drawVsModule.empty() && !drawPsModule.empty();
	}

	// CPU mirror of the emit CS spawn scheduling (rate*dt + carry): kept
	// here so tests can pin the GPU arithmetic (float-exact same ops).
	static uint32_t SpawnQuotaForFrame(float rate, float dt, float carryIn,
		float* carryOut = nullptr) {
		const float acc = carryIn + rate * dt;
		const float quota = std::floor(acc);
		const float carry = acc - quota;
		if (carryOut)
			*carryOut = carry;
		return quota > 0.0f ? static_cast<uint32_t>(quota) : 0u;
	}

	// Free-list init image: [0] = maxParticles, [i] = i - 1 (every slot
	// free, stack order irrelevant). One uint extra for the count head.
	static std::vector<uint32_t> BuildFreeListInitData(uint32_t maxParticles) {
		std::vector<uint32_t> data;
		data.reserve(static_cast<size_t>(maxParticles) + 1);
		data.push_back(maxParticles);
		for (uint32_t i = 0; i < maxParticles; ++i)
			data.push_back(i);
		return data;
	}

	// Sim dispatch group count: ceil(maxParticles / simThreads).
	static uint32_t SimGroupsFor(uint32_t maxParticles, uint32_t simThreads) {
		if (maxParticles == 0)
			return 0;
		simThreads = simThreads == 0 ? 64 : simThreads;
		return (maxParticles + simThreads - 1) / simThreads;
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* emitCsPath = kParticlesEmitCsPath,
		const char* simCsPath = kParticlesSimCsPath,
		const char* drawVsPath = kParticlesDrawVsPath,
		const char* drawPsPath = kParticlesDrawPsPath) {
		if (ShadersValid())
			return true;
		try {
			if (emitModule.empty())
				emitModule = ctx.CreateShaderModule(
					readFile(emitCsPath ? emitCsPath : kParticlesEmitCsPath),
					"particlesEmitCS");
			if (simModule.empty())
				simModule = ctx.CreateShaderModule(
					readFile(simCsPath ? simCsPath : kParticlesSimCsPath),
					"particlesSimCS");
			if (drawVsModule.empty())
				drawVsModule = ctx.CreateShaderModule(
					readFile(drawVsPath ? drawVsPath : kParticlesDrawVsPath),
					"particlesDrawVS");
			if (drawPsModule.empty())
				drawPsModule = ctx.CreateShaderModule(
					readFile(drawPsPath ? drawPsPath : kParticlesDrawPsPath),
					"particlesDrawPS");
		} catch (...) {
			return false;
		}
		if (emitModule.empty() || simModule.empty()
			|| drawVsModule.empty() || drawPsModule.empty())
			return false;
		Shader ecs{ .module = emitModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		emitProgram = { .shaders = { ecs }, .shaderCount = 1 };
		Shader scs{ .module = simModule, .entryPoint = "mainCS", .stage = ShaderStage::Compute };
		simProgram = { .shaders = { scs }, .shaderCount = 1 };
		Shader vs{ .module = drawVsModule, .entryPoint = "mainVS", .stage = ShaderStage::Vertex };
		Shader ps{ .module = drawPsModule, .entryPoint = "mainPS", .stage = ShaderStage::Fragment };
		drawProgram = { .shaders = { vs, ps }, .shaderCount = 2 };
		return true;
	}

	void DestroyShaders() {
		emitProgram = ShaderProgram{};
		simProgram = ShaderProgram{};
		drawProgram = ShaderProgram{};
		emitModule = {};
		simModule = {};
		drawVsModule = {};
		drawPsModule = {};
	}

	bool Create(ResourceManager& ctx, RenderGraph& rg, uint32_t frames,
		const ParticlesDesc& d,
		const char* emitCsPath = kParticlesEmitCsPath,
		const char* simCsPath = kParticlesSimCsPath,
		const char* drawVsPath = kParticlesDrawVsPath,
		const char* drawPsPath = kParticlesDrawPsPath) {
		if (d.maxParticles == 0 || d.maxEmitters == 0)
			return false;
		if (frames == 0)
			return false;
		Destroy(rg);

		Holder<BufferHandle> particles = ctx.CreateBuffer(
			sizeof(ParticleGpu) * d.maxParticles, BufferType_Storage,
			"particles pool");
		Holder<BufferHandle> freeList = ctx.CreateBuffer(
			sizeof(uint32_t) * (static_cast<size_t>(d.maxParticles) + 1),
			BufferType_Storage, "particles freeList");
		Holder<BufferHandle> state = ctx.CreateBuffer(
			sizeof(float) * d.maxEmitters, BufferType_Storage,
			"particles emitterState");
		Holder<BufferHandle> emitters = ctx.CreateBuffer(
			sizeof(ParticleEmitterGpu) * d.maxEmitters * frames,
			BufferType_Storage, "particles emitters ring");
		if (particles.empty() || freeList.empty() || state.empty() || emitters.empty())
			return false;

		LogicalBufferHandle particlesL = rg.AddExternalBuffer(particles, "particles pool");
		LogicalBufferHandle freeListL = rg.AddExternalBuffer(freeList, "particles freeList");
		LogicalBufferHandle stateL = rg.AddExternalBuffer(state, "particles emitterState");
		LogicalBufferHandle emittersL = rg.AddExternalBuffer(emitters, "particles emitters ring");
		if (particlesL.empty() || freeListL.empty() || stateL.empty() || emittersL.empty()) {
			rg.FreeBuffer(particlesL);
			rg.FreeBuffer(freeListL);
			rg.FreeBuffer(stateL);
			rg.FreeBuffer(emittersL);
			return false;
		}

		desc = d;
		framesInFlight = frames;
		particlesBuffer = std::move(particles);
		freeListBuffer = std::move(freeList);
		emitterStateBuffer = std::move(state);
		emittersBuffer = std::move(emitters);
		particlesLogical = particlesL;
		freeListLogical = freeListL;
		emitterStateLogical = stateL;
		emittersLogical = emittersL;

		// Prime the pool: zeroed particles (life <= 0 = dead), a full
		// free-list stack and zeroed emitter carry. Fresh device memory is
		// NOT guaranteed zero — the sim pass would read garbage lives and
		// double-push slots onto the stack without this.
		const std::vector<char> zeros(sizeof(ParticleGpu) * d.maxParticles, 0);
		ctx.UploadDataToBuffer(particlesBuffer, zeros.data(), zeros.size(),
			PipelineStageBit_Transfer, AccessFlags::TransferWrite, 0, 0);
		const std::vector<uint32_t> freeInit = BuildFreeListInitData(d.maxParticles);
		ctx.UploadDataToBuffer(freeListBuffer, freeInit.data(),
			sizeof(uint32_t) * freeInit.size(),
			PipelineStageBit_Transfer, AccessFlags::TransferWrite, 0, 0);
		const std::vector<float> stateZeros(d.maxEmitters, 0.0f);
		ctx.UploadDataToBuffer(emitterStateBuffer, stateZeros.data(),
			sizeof(float) * stateZeros.size(),
			PipelineStageBit_Transfer, AccessFlags::TransferWrite, 0, 0);
		return EnsureShaders(ctx, emitCsPath, simCsPath, drawVsPath, drawPsPath);
	}

	void Destroy(RenderGraph& rg) {
		if (!particlesLogical.empty())
			rg.FreeBuffer(particlesLogical);
		if (!freeListLogical.empty())
			rg.FreeBuffer(freeListLogical);
		if (!emitterStateLogical.empty())
			rg.FreeBuffer(emitterStateLogical);
		if (!emittersLogical.empty())
			rg.FreeBuffer(emittersLogical);
		particlesLogical = {};
		freeListLogical = {};
		emitterStateLogical = {};
		emittersLogical = {};
		particlesBuffer = {};
		freeListBuffer = {};
		emitterStateBuffer = {};
		emittersBuffer = {};
		framesInFlight = 1;
	}

	// Extent-independent; recreates only when the desc/flight count drifts.
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, uint32_t frames,
		const ParticlesDesc& d,
		const char* emitCsPath = kParticlesEmitCsPath,
		const char* simCsPath = kParticlesSimCsPath,
		const char* drawVsPath = kParticlesDrawVsPath,
		const char* drawPsPath = kParticlesDrawPsPath) {
		const bool shapeOk = Valid() && frames != 0
			&& desc.maxParticles == d.maxParticles
			&& desc.maxEmitters == d.maxEmitters
			&& desc.simThreads == d.simThreads
			&& framesInFlight == frames;
		if (shapeOk) {
			desc = d;
			return EnsureShaders(ctx, emitCsPath, simCsPath, drawVsPath, drawPsPath);
		}
		return Create(ctx, rg, frames, d, emitCsPath, simCsPath, drawVsPath, drawPsPath);
	}

	// Emitter ring upload: `rows` are the CPU's live parameters (tail.x
	// carry is ignored — the GPU owns scheduling state). Uploads one full
	// maxEmitters row set (zero-filled past `count`) into this frame's
	// flight slice; returns the live emitter count.
	uint32_t UpdateEmitters(ResourceManager& ctx, uint32_t frameIndex,
		const ParticleEmitterGpu* rows, uint32_t count) const {
		assert(Valid());
		assert(frameIndex < framesInFlight);
		count = std::min(count, desc.maxEmitters);
		std::vector<ParticleEmitterGpu> slice(desc.maxEmitters);
		for (uint32_t i = 0; i < count; ++i)
			slice[i] = rows[i]; // tail.x stays 0: carry lives in emitterState
		const size_t frameStride = sizeof(ParticleEmitterGpu) * desc.maxEmitters;
		ctx.UploadDataToBuffer(emittersBuffer, slice.data(), frameStride,
			PipelineStageBit_Compute | PipelineStageBit_Vertex | PipelineStageBit_Fragment,
			AccessFlags::ShaderRead, 0, frameIndex * frameStride);
		return count;
	}

	uint64_t ParticlesAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(particlesBuffer);
	}
	uint64_t FreeListAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(freeListBuffer);
	}
	uint64_t EmitterStateAddress(ResourceManager& ctx) const {
		return ctx.GetBufferDeviceAddress(emitterStateBuffer);
	}
	uint64_t EmittersAddress(ResourceManager& ctx, uint32_t frameIndex) const {
		assert(frameIndex < framesInFlight);
		return ctx.GetBufferDeviceAddress(emittersBuffer)
			+ frameIndex * sizeof(ParticleEmitterGpu) * desc.maxEmitters;
	}

	// ---- Pass shells (caller sets the recording func) ----

	// Emit: one [1,1,1] thread group per emitter. Reads the flight-slice
	// emitter rows + the persistent carry; writes the carry, pops/initializes
	// pool slots. Caller binds emitProgram, pushes ParticlesEmitPush and
	// dispatches (maxEmitters, 1, 1).
	RenderPass& AddEmitPass(RenderGraph& rg,
		const char* name = "ParticlesEmit") const {
		assert(Valid());
		RenderPass& pass = rg.AddComputePass(name);
		BufferSyncRequest read{};
		read.barrierMasks.accessMask = AccessFlags::ShaderRead;
		read.barrierMasks.stageMask = PipelineStageBit_Compute;
		pass.AddBufferHint(emittersLogical, read);
		BufferSyncRequest carryRW{};
		carryRW.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		carryRW.barrierMasks.stageMask = PipelineStageBit_Compute;
		pass.AddBufferHint(emitterStateLogical, carryRW);
		BufferSyncRequest poolRW{};
		poolRW.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		poolRW.barrierMasks.stageMask = PipelineStageBit_Compute;
		pass.AddBufferHint(particlesLogical, poolRW);
		pass.AddBufferHint(freeListLogical, poolRW);
		return pass;
	}

	// Sim: one thread per pool slot. Reads emitter rows (drag/gravityScale),
	// integrates + ages alive slots, pushes dead slots back onto the
	// free-list. Caller binds simProgram, pushes ParticlesSimPush and
	// dispatches (SimGroupsFor(maxParticles, simThreads), 1, 1).
	RenderPass& AddSimPass(RenderGraph& rg,
		const char* name = "ParticlesSim") const {
		assert(Valid());
		RenderPass& pass = rg.AddComputePass(name);
		BufferSyncRequest read{};
		read.barrierMasks.accessMask = AccessFlags::ShaderRead;
		read.barrierMasks.stageMask = PipelineStageBit_Compute;
		pass.AddBufferHint(emittersLogical, read);
		BufferSyncRequest poolRW{};
		poolRW.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		poolRW.barrierMasks.stageMask = PipelineStageBit_Compute;
		pass.AddBufferHint(particlesLogical, poolRW);
		pass.AddBufferHint(freeListLogical, poolRW);
		return pass;
	}

	// Draw: additive camera-facing quads over the resolved HDR, depth-tested
	// read-only (GreaterEqual reversed-Z, no writes) against the prepass
	// depth. StoreOp Store on depth keeps it valid for the post chain
	// (SSR/volumetric sample it after Main; same rule as the Main pass).
	// Caller binds drawProgram with BlendSettings::Add() +
	// VertexFormat::Empty, pushes ParticlesDrawPush and calls
	// Draw(6, maxParticles, 0, 0).
	RenderPass& AddDrawPass(RenderGraph& rg,
		LogicalImageViewHandle colorView,
		LogicalImageViewHandle depthView,
		Extent2D extent,
		const char* name = "ParticlesDraw") const {
		assert(Valid());
		assert(!colorView.empty() && !depthView.empty());
		RenderPass& pass = rg.AddPass(name);
		pass.SetOffset(0, 0)
			.SetExtent(extent)
			.SetColor({ colorView },
				AttachmentLoadOp::Load,
				AttachmentStoreOp::Store,
				ClearColor(0, 0, 0, 1))
			.SetDepth(depthView,
				AttachmentLoadOp::Load,
				AttachmentStoreOp::Store,
				ClearDepthStencil(0.0f, 0));
		BufferSyncRequest poolRead{};
		poolRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
		poolRead.barrierMasks.stageMask =
			PipelineStageBit_Vertex | PipelineStageBit_Fragment;
		pass.AddBufferHint(particlesLogical, poolRead);
		pass.AddBufferHint(emittersLogical, poolRead);
		return pass;
	}

	// Forward+ lighting inputs for the draw pass: the per-frame light buffer
	// (fragment read) and the tile lists (fragment read; orders the pass
	// after the cull dispatch). Only call when the tile lists are fresh.
	static void AddDrawLightHints(RenderPass& drawPass,
		LogicalBufferHandle fpLightsLogical,
		LogicalBufferHandle tileCountsLogical,
		LogicalBufferHandle tileIndicesLogical) {
		BufferSyncRequest read{};
		read.barrierMasks.accessMask = AccessFlags::ShaderRead;
		read.barrierMasks.stageMask = PipelineStageBit_Fragment;
		drawPass.AddBufferHint(fpLightsLogical, read);
		drawPass.AddBufferHint(tileCountsLogical, read);
		drawPass.AddBufferHint(tileIndicesLogical, read);
	}
};

} // namespace smvk
