// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// GPU probe pre-mix (Remember Me architecture; docs/probe_premix_plan.md).
//
// Per frame, two compute passes turn the RESIDENT probe tail (the Forward+
// light buffer type-2 entries - the GPU-side selection we already have,
// NO CPU classification) into ONE blended cubemap every lit pixel samples
// with a single fetch (the per-pixel walk's <=8 fetches become 1):
//
//   [A] weights (1 group): reads the probe tail through the fpLights
//       buffer-address (the same entries the lit walk reads), evaluates
//       the CURRENT validated weighting - fadeWeight^3 * influence^2 at
//       the point of interest (the camera position) - selects the top-K
//       (K = 4) by weight (deterministic tie-break: weight, then
//       influence, then index), normalizes, and writes
//       {count, indices[K], weights[K]} to the selection buffer.
//       Zero candidates -> count 0 (the material falls back to the
//       global-cubemap/analytic-sky path untouched).
//
//   [B] mix (per face/mip groups, one dispatch per mip): each texel of
//       the blended cubemap's (face, mip) = sum(weights[i] *
//       sample(source_i, direction, mip)). Mip-to-mip ONLY (the Remember
//       Me rule: the GGX chains are already baked per capture - the mix
//       never re-prefilters). The blended writes go through the shared
//       bindless storage array (binding 2, evsmStorage/spdImages slot
//       space): the blended cubemap image is a 128^2 x 6 x 8-mip cube
//       image, and the module registers ONE 2D-array storage view per mip
//       (6 faces = array layers) - dst indices are CPU-known at creation.
//
// The material side (environment.hlsli, TOURMALINE_PROBE_PREMIX=1) reads
// the blended cubemap's bindless SAMPLED index from FrameUniforms and
// samples it once, with box parallax from the selection's dominant probe
// (the weights pass also writes the dominant probe's packed parallax box
// so the material keeps the depth-fitted correction without a second
// fetch of the light tail).
//
// Why one blend for the whole frame is acceptable: the blockiness class
// came from PER-PIXEL probe-set switching (tile slots, weight ties,
// candidate swaps). A single pre-blended texture cannot switch sets
// per pixel by construction; the cost is that pixels far from the POI
// sample a less-local mixture (the Remember Me trade-off, accepted by
// design and A/B-able via the pin).

#include <Engine/Memory/pool.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Rendergraph/forwardplus.h>
#include <Engine/platform.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace smvk::probepremix {

inline constexpr uint32_t kMaxBlendProbes = 4;
inline constexpr uint32_t kDefaultBlendSize = 128;

// Blend selection written by the weights pass (BDA-read by the mix pass
// and the material's single-fetch branch; layout must mirror
// probepremix_weights/mix.cs.hlsl + environment.hlsli). All GPU-owned:
// the CPU never reads it back (no classification, no readback stall).
struct alignas(16) BlendSelectionGpu {
	uint32_t count = 0;                                     // 0
	uint32_t indices[kMaxBlendProbes] = {};                 // 4  cubemap bindless
	float weights[kMaxBlendProbes] = {};                    // 20 normalized
	// The DOMINANT probe (slot 0 - the highest blend weight): the
	// material's single-fetch branch keeps the depth-fitted box parallax
	// from it. pos = the capture position; boxMinus/boxPlus = the packed
	// parallax box's plane distances (the same packing the lit walk reads
	// off the light tail - MakeProbeLightEntry).
	float pos[3] = {};                                       // 36
	float boxMinus[3] = {};                                 // 48
	float boxPlus[3] = {};                                  // 60
	uint32_t pad0 = 0;                                      // 72
	uint32_t pad1 = 0;                                      // 76
};
static_assert(sizeof(BlendSelectionGpu) == 80);

// Weights-pass push (must mirror probepremix_weights.cs.hlsl).
struct alignas(16) WeightsPush {
	uint64_t lights = 0;          // 0  fpLights buffer device address
	uint64_t selection = 0;       // 8  BlendSelectionGpu output BDA
	float poiX = 0.0f;            // 16 point of interest (camera position)
	float poiY = 0.0f;            // 20
	float poiZ = 0.0f;            // 24
	uint32_t lightCount = 0;     // 28 lights [0, lightCount) in the buffer
	uint32_t probeCount = 0;     // 32 probes [lightCount, +probeCount)
	uint32_t pad0 = 0;            // 36 (16-align tail)
	uint32_t pad1 = 0;            // 40
};
static_assert(sizeof(WeightsPush) == 48);

// Mix-pass push (must mirror probepremix_mix.cs.hlsl).
struct alignas(16) MixPush {
	uint64_t selection = 0;      // 0  BlendSelectionGpu input BDA
	uint32_t dstIndex = 0;       // 8  bindless storage index of THIS mip's
	                             //    2D-array view (6 face layers)
	uint32_t mip = 0;             // 12 mip being written (mix mip-to-mip)
	uint32_t faceSize = 0;       // 16 face extent at this mip
	uint32_t samplerIndex = 0;   // 20 bindless sampler (clamp, linear mips)
	uint32_t mipCount = 0;       // 24 (diagnostics only)
	uint32_t pad0 = 0;           // 28
};
static_assert(sizeof(MixPush) == 32);

struct ProbePremix {
	// The blended cubemap: 128^2 x 6 faces, mipCount mips, RGBA16F,
	// cube-compatible. Storage-written by the mix pass (one 2D-array
	// view per mip for the bindless storage array) and SAMPLED by the
	// material through one cube view (full chain).
	Holder<ImageHandle> blendedImage;
	Holder<ImageViewHandle> cubeView;         // sampled (all faces/mips)
	Holder<BindlessImageHandle> cubeBindless; // sample bindless (cubemaps[] space)
	// Per-mip storage views (binding-2 slot space) + their logical views.
	std::vector<Holder<ImageViewHandle>> mipStorageViews;
	std::vector<Holder<BindlessImageHandle>> mipStorageBindless;
	std::vector<LogicalImageViewHandle> mipLogical; // graph barrier handles
	std::vector<uint32_t> mipStorageIndex;         // push dstIndex values
	uint32_t cubeBindlessIndex = UINT32_MAX;       // FrameUniforms field
	uint32_t size = 0;
	uint32_t mipCount = 0;

	// Selection buffer (GPU-owned, rewritten by the weights pass each frame).
	Holder<BufferHandle> selectionBuffer;
	uint64_t selectionBda = 0;

	Holder<ShaderModuleHandle> weightsModule;
	Holder<ShaderModuleHandle> mixModule;
	ShaderProgram weightsProgram{};
	ShaderProgram mixProgram{};

	bool valid = false;

	// Idempotent resource + program creation. faceSizeIn default 128
	// (matches the bake's --cubemap-size; the mix mip chains align when
	// the sources share the chain depth - deeper source chains clamp).
	bool Ensure(ResourceManager& ctx, RenderGraph& rg, uint32_t faceSizeIn,
		uint32_t mipCountIn);
	void Destroy(RenderGraph& rg);

	// Appends the two compute passes (weights then mix). lightsBda =
	// the fpLights buffer address (the same buffer the lit walk reads);
	// lightCount/probeCount = the tail window; poi = the camera position;
	// samplerIndex = the bindless cubemap sampler (clamp, linear mips -
	// the same one the lit walk uses).
	bool AppendPasses(RenderGraph& rg, uint64_t lightsBda,
		uint32_t lightCount, uint32_t probeCount, const glm::vec3& poi,
		uint32_t samplerIndex);
};

} // namespace smvk::probepremix
