// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Chrome-ball visualization of the loaded reflection cubemap probes — a
// DEDICATED rendergraph pass (NOT part of the lit Main pass): a small
// sphere-drawing pass whose fragment shader paints THE PROBE'S OWN baked
// cubemap onto the sphere surface (reflect(-V, N) -> TextureCube sample at
// LOD 0 — the sharp, non-prefiltered base mip), so each ball reads exactly
// what its capture saw. No scene pc, no lit-path lighting: the pass pushes
// its own block (ProbeBallPush, mirrored in probeball.vert/frag.hlsl).
//
// Compositing: the pass runs AFTER Main on the 1x HDR target (load/store —
// the balls composite over the scene and bloom/tonemap see them) with the
// scene depth as a TEST-ONLY attachment (GreaterEqual, no write: occluded
// balls hide behind geometry; the debug visual never pollutes the depth
// the post-Main consumers march). 2x MSAA mode degrades to the prepass
// depth (Main wrote the transient MSAA depth) — a conservative test, not
// a wrong one. Geometry: ONE shared unit sphere in the terrain's vertex
// format (POSITION + NORMAL read; tangent/color bound but undeclared).
//
// The editor's old chrome balls were scene nodes (outliner leaks + a second
// transform-composition path — the "ball in the sky" bug); this pass owns
// nothing but the sphere + the programs.

#include <Engine/Memory/pool.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/platform.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace smvk::probeball {

inline constexpr const char* kProbeBallVsPath =
	"content/shaders/probeball.vert.hlsl.spv";
inline constexpr const char* kProbeBallPsPath =
	"content/shaders/probeball.frag.hlsl.spv";

// One drawable probe row (the WorldRenderer's CubemapProbeGpu shape,
// duplicated here to keep the module free of the WorldRenderer header).
struct ProbeBallRow {
	glm::vec3 center{ 0.0f };
	float influenceRadius = 10.0f;
	float fadeRadius = 5.0f;
	int32_t cubemapIndex = -1; // bindless TextureCube slot (-1 = skip)
	float mipLevels = 1.0f;
};

// The pass's own push block — must mirror probeball.vert.hlsl /
// probeball.frag.hlsl exactly (the pass is standalone: NO scene pc).
// probe.x carries the cubemap bindless index AS FLOAT (the lit path's
// convention: `uint cubemapIdx = (uint)L.direction.y;` — a plain cast);
// probe.w carries the bindless SAMPLER slot the FS casts the same way.
// frame carries the scene FrameUniforms buffer address so the FS can
// read the frame's env (the sun disc parity term — see below); 0 = no
// sun term (headless/test callers).
struct alignas(16) ProbeBallPush {
	glm::mat4 viewProj; // 0  the rendering view's VP (reversed-Z, Y-flipped)
	glm::vec4 eye;      // 64 xyz camera, w unused
	glm::vec4 xform;    // 80 xyz ball center, w radius
	glm::vec4 probe;    // 96 x cubemap index (float, cast in FS), y mips,
	                    //    z intensity, w sampler slot
	uint64_t frame = 0; // 112 FrameUniforms BDA (the env sun fields)
};
static_assert(sizeof(ProbeBallPush) == 128); // 120 + alignas(16) tail pad

struct ProbeBallVisual {
	Holder<BufferHandle> sphereVB;   // TerrainVertex unit sphere
	Holder<BufferHandle> sphereIB;   // uint32 indices
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
	bool geometryValid = false;

	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> psModule;
	ShaderProgram program{};

	[[nodiscard]] bool ShadersValid() const {
		return program.shaderCount == 2 && !vsModule.empty()
			&& !psModule.empty();
	}

	// Unit sphere (24x16 UV sphere — the check-scene chrome-ball pattern) +
	// the shader programs. Non-fatal on shader failure: AppendPass checks
	// ShadersValid and skips.
	bool Ensure(ResourceManager& ctx);
	void Destroy();

	// Appends the dedicated probe-ball pass to the rendergraph: the HDR
	// color (load/store) + the scene depth (load, TEST-ONLY) at `extent`,
	// one cubemap-painting draw per probe row (rows COPIED into the
	// recording — the caller's table may mutate after the call).
	// frameUniformsAddress carries the scene FrameUniforms BDA so the FS
	// can add the analytic sun's image to the mirror (the captures exclude
	// the sun disc by design — the lit path's analytic direct owns the sun
	// ON surfaces — and the overlay has no lit-path direct term, so without
	// it a chrome ball's mirror of the open sky showed a sunless band
	// while the direct sky bloomed the disc; the FS mirrors
	// EvaluateSkyColor's disc+rim math from the frame env, so the ball's
	// sky matches the runtime sky by construction). Returns
	// false when nothing can draw (no geometry/shaders/valid rows).
	bool AppendPass(RenderGraph& rg,
		LogicalImageViewHandle hdrView, LogicalImageViewHandle depthView,
		Extent2D extent, const glm::mat4& viewProj, const glm::vec3& eye,
		const ProbeBallRow* rows, uint32_t rowCount, float intensity,
		uint32_t samplerIndex, uint64_t frameUniformsAddress = 0);
};

} // namespace smvk::probeball
