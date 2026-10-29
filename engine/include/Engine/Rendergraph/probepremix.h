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

#include <algorithm>
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
	// The parallax box: WEIGHT-LERPED across the selected set (lerping
	// the capture positions + the plane distances by the mix weights is
	// exactly lerping the AABB corners - one smooth box that tracks the
	// blend, no popping when the dominant slot swaps while moving). The
	// same packing the lit walk reads off the light tail
	// (MakeProbeLightEntry); pos = the lerped capture position,
	// boxMinus/boxPlus = the lerped plane distances.
	float pos[3] = {};                                       // 36
	float boxMinus[3] = {};                                 // 48
	float boxPlus[3] = {};                                  // 60
	uint32_t pad0 = 0;                                      // 72
	uint32_t pad1 = 0;                                      // 76
};
static_assert(sizeof(BlendSelectionGpu) == 80);

// ---------------------------------------------------------------------------
// NDF blend weights (the Remember Me transition-region formula; the CPU
// mirror of the weights pass - the unit tests drive THIS, the HLSL pass
// implements the same math). Inputs are per-candidate NDF values:
//   d_i = (dist - influence) / fade   (<=0 inner, (0,1) transition, >=1 out)
// The published weighting (Lagarde, GPU Pro 4):
//   a_i = (1 - d_i / S_d) / (N - 1),  b_i = (1 - d_i) / S_r,
//   W_i = a_i * b_i / sum(a_j * b_j),  S_d = sum(d_j), S_r = sum(1 - d_j).
// Special cases (in evaluation order):
//   - no candidates: all weights 0.
//   - an INNER hit (d <= 0): the designated dominant gets EXCLUSIVE weight
//     (ties: the widest influence wins, then the lowest index - the
//     validated wide-first policy; the documented Remember Me limitation
//     at inner-region exits applies).
//   - one candidate: weight 1 (the a-term divides by N-1 = 0 - the
//     formula degenerates; the exclusive weight is the published fix).
//   - candidates with d >= 1 (outside the outer boundary) drop out first.
// The transition formula makes the blend spatially varying even where two
// influence volumes overlap fully (both at d in (0,1)): the probe nearer
// its inner region outweighs the outer one - the original's point.
// ---------------------------------------------------------------------------
struct NdfCandidate {
	uint32_t cubemap = UINT32_MAX; // the candidate's cubemap bindless index
	float ndf = 1.0f;               // clamped: 0 = inner, [0,1), 1 = out
	float influence = 0.0f;         // tie-break (wide wins)
};

inline void ComputeNdfWeights(const NdfCandidate* candidates, uint32_t n,
	float* outWeights, uint32_t maxOut = kMaxBlendProbes) {
	for (uint32_t i = 0; i < maxOut; ++i)
		outWeights[i] = 0.0f;
	if (n == 0 || n > maxOut)
		return;
	// The designated dominant inner hit (if any): widest influence wins,
	// then the lowest cubemap index.
	int32_t dominant = -1;
	for (uint32_t i = 0; i < n; ++i) {
		if (candidates[i].cubemap == UINT32_MAX || candidates[i].ndf >= 1.0f)
			continue; // out of influence entirely
		if (candidates[i].ndf <= 0.0f) {
			if (dominant < 0
				|| candidates[i].influence > candidates[dominant].influence
				|| (candidates[i].influence == candidates[dominant].influence
					&& candidates[i].cubemap < candidates[dominant].cubemap))
				dominant = int32_t(i);
		}
	}
	if (dominant >= 0) {
		// Inner-region hit: the designated dominant probe EXCLUSIVE
		// (everyone else 0 - including other inner hits).
		for (uint32_t i = 0; i < n; ++i)
			outWeights[i] = 0.0f;
		outWeights[dominant] = 1.0f;
		return;
	}
	// Transition region only. One candidate: the a-term degenerates
	// (N-1 = 0) - the exclusive weight 1.
	uint32_t count = 0;
	for (uint32_t i = 0; i < n; ++i)
		if (candidates[i].cubemap != UINT32_MAX && candidates[i].ndf < 1.0f)
			++count;
	if (count == 0)
		return;
	if (count == 1) {
		for (uint32_t i = 0; i < n; ++i)
			outWeights[i] =
				(candidates[i].cubemap != UINT32_MAX && candidates[i].ndf < 1.0f)
				? 1.0f : 0.0f;
		return;
	}
	// The published a*b weighting over the in-influence candidates.
	float sumD = 0.0f, sumR = 0.0f;
	for (uint32_t i = 0; i < n; ++i) {
		if (candidates[i].cubemap == UINT32_MAX || candidates[i].ndf >= 1.0f)
			continue;
		sumD += candidates[i].ndf;
		sumR += (1.0f - candidates[i].ndf);
	}
	sumD = std::max(sumD, 1e-6f);
	sumR = std::max(sumR, 1e-6f);
	float sumW = 0.0f;
	for (uint32_t i = 0; i < n; ++i) {
		if (candidates[i].cubemap == UINT32_MAX || candidates[i].ndf >= 1.0f) {
			outWeights[i] = 0.0f;
			continue;
		}
		const float d = candidates[i].ndf;
		const float a = (1.0f - d / sumD) / float(count - 1);
		const float b = (1.0f - d) / sumR;
		outWeights[i] = std::max(a, 0.0f) * std::max(b, 0.0f);
		sumW += outWeights[i];
	}
	if (sumW <= 1e-6f)
		return;
	for (uint32_t i = 0; i < n; ++i)
		outWeights[i] /= sumW;
}

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
	// Graph barrier handle for the SAMPLED cube view (the material's
	// single fetch through frame.probePremixIndex): the Main pass hints
	// a fragment ShaderRead on it, which pairs with the per-mip storage
	// writes below (CollectPassUsages keys sync on the resolved
	// PHYSICAL image - both view families alias blendedImage) to build
	// the mix-write -> Main-read edge + the General ->
	// ShaderReadOnlyOptimal transition the bindless fetch alone never
	// produces.
	LogicalImageViewHandle cubeLogical;
	uint32_t cubeBindlessIndex = UINT32_MAX;       // FrameUniforms field
	uint32_t size = 0;
	uint32_t mipCount = 0;

	// Selection buffer (GPU-owned, rewritten by the weights pass each frame).
	Holder<BufferHandle> selectionBuffer;
	uint64_t selectionBda = 0;
	// Graph barrier handle for the selection buffer. Minted ONCE in
	// Ensure and reused by BOTH AppendPasses hints - re-minting per
	// call allocated a fresh logical buffer per pass per frame
	// (NewFrame clears passes_ only, never the logical pools: two live
	// entries per frame until the 8192-entry pool cap, then
	// AddExternalBuffer hands back empty handles).
	LogicalBufferHandle selectionLogical;

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
