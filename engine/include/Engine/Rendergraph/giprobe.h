// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// GI probe visualization — the convex volumes' probe lattice as small
// instanced spheres carrying their OWN baked directional GI (the ImGui
// probe-point dots' 3D successor): one hardcoded single-subdivision
// icosphere (42 verts / 80 tris, fixed 0.12 m radius) instanced over a
// per-volume GPU instance table (probe position + integer grid coords);
// the VERTEX shader fetches the probe's EXACT texel (Texture3D.Load —
// never the filtered Sample) from the volume's 7 SG layers, unpacks the
// 9 lobe amplitudes (SgRegularUnpack) and evaluates the
// WHITE-LAMBERTIAN probe answer — SgGiDiffuseIrradiance(amps, dir) *
// giIntensity / PI, albedo 1 — the same quantity a matte white surface
// renders as ambient there (the lit body's kD * baseColor * E/PI with
// baseColor = 1), so each sphere reads as a tiny scene surface showing
// the baked field's directional answer, not raw radiance (which washed
// out ~PI x too white). The fragment shader is a pure pass-through into
// HDR (no lighting, no sampling).
//
// The probeball.h pattern with ONE divergence: the depth attachment is
// tested AND WRITTEN (GreaterEqual reversed-Z) — the write is
// load-bearing for sphere-vs-sphere occlusion (a lattice of overlapping
// instanced spheres needs it; probeball's few balls got away with
// test-only), and it is safe at this slot: nothing after the HDR tail
// reads depth. One DrawIndexed INSTANCED call per volume (the shared
// geometry, SV_InstanceID-indexed rows through a BDA).
//
// The old ImGui overlay's filters live on: the camera-distance filter
// (m_giProbePointMaxDist) runs GPU-side in the VS (every vertex of an
// out-of-range instance collapses to one degenerate point — zero
// pixels), and the instance-count cap (the old 6000-dot ImGui budget)
// runs at INSTANCE-TABLE BUILD time as a lattice stride (the tables are
// static — probes never move — built once, zero per-frame CPU work).

#include <Engine/Memory/pool.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/platform.h>

#include <glm/glm.hpp>

#include <cstdint>

namespace smvk::giprobe {

inline constexpr const char* kGiProbeVsPath =
	"content/shaders/giprobe.vert.hlsl.spv";
inline constexpr const char* kGiProbePsPath =
	"content/shaders/giprobe.frag.hlsl.spv";

// The instance-table cap (the old overlay's ImGui budget, kept as the
// GPU draw budget; HLSL mirror: GI_PROBE_MAX_INSTANCES in
// giprobe.vert.hlsl).
inline constexpr uint32_t kGiProbeMaxInstances = 6000;

// One instance row: the probe's world position + its INTEGER lattice
// coordinates (the exact Texture3D texel the VS Loads). 32 bytes, the
// HLSL mirror's layout (giprobe.vert.hlsl GiProbeInstance).
struct GiProbeInstance {
	glm::vec3 pos{ 0.0f };
	float pad_ = 0.0f;
	uint32_t grid[3]{};
	uint32_t pad2_ = 0;
};
static_assert(sizeof(GiProbeInstance) == 32, "HLSL mirror (giprobe.vert.hlsl)");

// One per-volume draw: the instance buffer + its device address + the
// volume's FrameUniforms.giVolumeLayers ROW index (v * 7 — the VS reads
// layer slots as giVolumeLayers[volumeIndex * 7 + t], exactly like
// SgGiVolumeSample).
struct GiProbeVolumeRow {
	BufferHandle instances{};
	uint64_t instancesBda = 0;
	uint32_t instanceCount = 0;
	uint32_t volumeIndex = 0;
};

// The pass's own push block — must mirror giprobe.vert.hlsl exactly
// (the pass is standalone: NO scene pc). frame carries the scene
// FrameUniforms BDA (the giVolumeLayers bindless slots); instances
// carries this volume's instance-table BDA.
struct alignas(16) GiProbePush {
	glm::mat4 viewProj;  // 0  the rendering view's VP (reversed-Z, Y-flipped)
	glm::vec4 eyeMaxDist; // 64 xyz camera, w the distance filter (m)
	uint64_t frame = 0;     // 72 FrameUniforms BDA
	uint64_t instances = 0; // 80 the volume's instance-table BDA
	uint32_t volumeIndex = 0; // 88 the giVolumeLayers row / 7
	uint32_t pad_[3]{};       // 92 (16-align tail; 112 total)
};
static_assert(sizeof(GiProbePush) == 112);

struct GiProbeVisual {
	Holder<BufferHandle> sphereVB;   // TerrainVertex unit icosphere
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

	// Hardcoded icosphere (1-subdivision icosahedron: 42 verts, 80
	// tris, fixed radius baked into the shader) + the programs.
	// Non-fatal on shader failure: AppendPass checks ShadersValid and
	// skips.
	bool Ensure(ResourceManager& ctx);
	void Destroy();

	// Appends the dedicated probe-sphere pass: the HDR color
	// (load/store) + the scene depth (load, TEST-ONLY) at `extent`, ONE
	// instanced DrawIndexed per volume row. Rows are COPIED (handles +
	// BDAs are plain values — the caller's tables may mutate after the
	// call). Returns false when nothing can draw (no geometry/shaders/
	// valid rows).
	bool AppendPass(RenderGraph& rg,
		LogicalImageViewHandle hdrView, LogicalImageViewHandle depthView,
		Extent2D extent, const glm::mat4& viewProj, const glm::vec3& eye,
		float maxDist, const GiProbeVolumeRow* rows, uint32_t rowCount,
		uint64_t frameUniformsAddress);
};

} // namespace smvk::giprobe
