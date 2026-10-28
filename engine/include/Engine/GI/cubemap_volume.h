// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// GI volumes: shaped local probe grids cooked by cubemap
// reprojection (Black Ops 3 style), blended over the sky base.
//
// The GI pivot: probes only. The ambient is carried by AUTHORED GI
// volumes (one per room/street/alley), each a dense SG probe grid;
// uncovered space falls to the analytic sky IBL. (The legacy sparse
// octree world field — SgGiVolume, the deleted probe_volume.h — is
// retired: stage 3 of the octree retirement.) Lightmaps are deprecated
// and no longer cooked (the runtime loader stays for existing content).
//
// Stacking model (TPP/HZD/BO3 lineage — volumes BLEND, grids never
// cross-interpolate across density levels):
//
//   E = SkyDiffuseBase(P, N)        // the analytic-sky fallback
//   for (v : volumesSortedLowToHighPriority)
//       E = lerp(E, SampleVolumeTrilinear(v, P, N), v.Weight(P))
//
// A shading point samples each density level INDEPENDENTLY; the sparse
// octree never becomes a vertex in a dense grid's interpolation stencil
// (that creates resolution-dependent gradients where supports change
// abruptly), and vice versa. Ordered compositing makes overrides trivial:
// global octree -> town volume -> building volume -> small room, one
// algorithm for all of them.
//
// Volume shape: an intersection of half-spaces
//     n_i . P + d_i <= 0        (inside plane i)
// with PER-PLANE blend distances (the FOX innerScales idea): walls get
// 0.1 m fades, a doorway plane gets a 2 m fade, so the outdoor field
// transitions gradually through openings and never through walls:
//
//     d_i(P) = -(n_i . P + d_i)                  signed distance inside
//     w_i(P) = smoothstep(-outsideFade_i, insideFade_i, d_i)
//     Weight(P) = min_i w_i(P)                   nearest face dominates
//
// min-combining keeps a doorway fade alive next to a tight floor (a
// product would crush it to zero at the threshold corner).
//
// Bake (BakeGiVolume): DIRECT per-probe path trace. This shipped as the
// published BO3 pipeline — sparse radiance+depth captures reprojected
// into the dense probes — but the capture-support dependency cost real
// coverage: probes whose enclosing capture cell had no valid capture
// (solid interiors, clearance skips) baked zero, and the parallax/
// depth gates rejected texels near walls (the sponza bake: 48k of 526k
// probes invalid, reading as holes in every probe preview). The Embree
// backend traces the dense grid directly now: every probe runs its own
// equal-solid-angle direction set through TraceIncoming (the same
// transport as every other baked source) and feeds the diagonal SG
// projection with exact texel solid angles — the old pipeline's own
// mathematical identity case (captureSpacing == spacing), minus the
// amortize, plus per-probe firefly clamping (8x the probe's own mean),
// validity dilation for probes in solid geometry (the
// regular_volume.cpp pattern), and the optional Laplacian volume
// smoothing (BO3's "volume smoothing").
//
// Runtime: hardware trilinear over 7 RGBA16F 3D textures per volume
// (identical GPU layout to SgRegularVolume), weight/priority blend in
// sg_gi_volume.hlsli. File format: .givolume ("GIVOL" v1, LZ4 half payload).

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/GI/sg.h>

namespace smvk::gi {

// ---------------------------------------------------------------------------
// Convex definition (authored)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kGiVolumeMaxPlanes = 16; // must match SG_GI_VOLUME_MAX_PLANES

struct GiVolumePlane {
	glm::vec3 normal{ 0.0f, 1.0f, 0.0f }; // unit; inside: dot(n, P) + d <= 0
	float d = 0.0f;                       // plane offset (n . P + d)
	float insideFade = 0.1f;  // blend width INSIDE the face (m)
	float outsideFade = 0.1f; // blend width OUTSIDE the face (m) — the
	                          // volume keeps partial influence in the
	                          // doorway bleed zone
};

inline float GiVolumeSmoothStep(float e0, float e1, float x) {
	if (e1 - e0 < 1e-5f)
		return x >= e1 ? 1.0f : 0.0f;
	const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

// Per-plane fade weight (authored-fades reference; the GPU packs this
// into one folded float4 at upload time — weightEquation =
// (-n*fadeScale, fadeBias - d*fadeScale) — see sg_gi_volume.hlsli for the
// algebraic equivalence and the degenerate-span note).
inline float GiVolumePlaneWeight(const GiVolumePlane& p, const glm::vec3& pos) {
	const float inside = -(glm::dot(p.normal, pos) + p.d);
	return GiVolumeSmoothStep(-p.outsideFade, p.insideFade, inside);
}

struct GiVolumeDef {
	std::vector<GiVolumePlane> planes; // must bound a closed convex region
	uint32_t priority = 0;      // higher = composited later = wins
	std::string name;           // authoring id (bake output naming)

	// Per-face weight: min over planes of the per-plane smoothstep.
	float Weight(const glm::vec3& pos) const {
		float w = 1.0f;
		for (const GiVolumePlane& p : planes) {
			w = std::min(w, GiVolumePlaneWeight(p, pos));
			if (w <= 0.0f)
				break;
		}
		return w;
	}

	bool Contains(const glm::vec3& pos) const {
		for (const GiVolumePlane& p : planes)
			if (glm::dot(p.normal, pos) + p.d > 0.0f)
				return false;
		return !planes.empty();
	}

	// AABB of the convex region: a generous box clipped plane-by-plane
	// (inside corners + edge-plane intersections; a mild superset for
	// non-box shapes — the fade band and min-weight rule make the extra
	// border texels harmless). Returns false when the region is empty
	// or unbounded (the plane set must close).
	bool Bounds(glm::vec3& mn, glm::vec3& mx) const;
};

// Axis-aligned box helper: six planes with one fade pair each. Face
// order: -x, +x, -y, +y, -z, +z (indices for per-face fade retuning).
std::vector<GiVolumePlane> GiVolumeBoxPlanes(const glm::vec3& mn, const glm::vec3& mx,
	float insideFade = 0.1f, float outsideFade = 0.1f);

// ROTATED box (OBB) helper: center + orthonormal axes as mat3 COLUMNS
// (normalized defensively) + half extents -> six planes. The authored
// GI volume nodes' rotation support: the dense probe lattice stays
// axis-aligned over the OBB's AABB (Bounds); the plane weights carve
// the rotated hull. Identity axes degenerate to GiVolumeBoxPlanes exactly.
// Face order: -u0, +u0, -u1, +u1, -u2, +u2.
std::vector<GiVolumePlane> GiVolumeBoxPlanesRotated(const glm::vec3& center,
	const glm::mat3& axes, const glm::vec3& half,
	float insideFade = 0.1f, float outsideFade = 0.1f);

// ---------------------------------------------------------------------------
// Volume (cooked)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kGiVolumeMaxDim = 256; // per-axis grid cap (sgrv parity)

class GiVolume {
public:
	GiVolumeDef def;
	glm::vec3 gridMin{ 0.0f }; // AABB lattice origin (def.Bounds)
	float spacing = 0.5f;      // probe spacing (m)
	uint32_t dims[3] = { 1, 1, 1 };
	// Voxel-major, lobe-minor: coeffs[(z*Ny + y)*Nx + x][lobe] — the
	// SgRegularVolume layout, shared by the GPU packer below.
	std::vector<glm::vec3> coeffs;

	// ---- Per-voxel 6-axis visibility (the through-wall rejection
	// data) ----
	// 6 obstacle distances per voxel, measured from the voxel center
	// along the lattice axes, order +x,-x,+y,-y,+z,-z, in METERS,
	// capped at the bake's visibilityMaxDistance (a miss = the cap =
	// "open"). 0 = BLOCKED (the probe is inside solid geometry — every
	// runtime tap whose offset exceeds the slack rejects, so
	// inside-solid texels never contribute). This is the dense-grid
	// rebirth of the retired .sggi octree's 6-axis probe visibility
	// (the wall-awareness the octree walker used to reject through-wall
	// probes — SgGiVisibilityFactor; the runtime twin is
	// SgGiVolumeVisFactor in sg_gi_volume.hlsli, and the bake's
	// dilation/smoothing gates below consume the same data). EMPTY
	// (a hand-built volume, or the pre-visibility bakes) = "no
	// rejection": PackGpuVisLayers materializes all-open distances +
	// all-clean flags, and Sample degenerates to the plain trilerp —
	// exactly the pre-visibility behavior.
	std::vector<float> vis; // 6 per voxel (meters; empty = open)
	// Per-voxel fast-path flag: 1 = the FORWARD cell (this voxel ..
	// +1 per axis) is CLEAN — every corner's every axis distance
	// >= 1.35 spacing units (the max in-cell tap offset along an axis
	// is 1 spacing; rejection starts at dist+0.35), so no tap can
	// reject and the hardware trilerp IS the visibility-weighted
	// blend. The shader point-fetches it at the cell's base corner
	// and takes the 7-fetch hardware path when set. Baked once;
	// empty = all clean (paired with the open-vis default above).
	std::vector<uint8_t> visCellClean;

	uint64_t VoxelCount() const;
	void Resize(); // allocate coeffs per dims (zeroed)

	// Visibility-weighted trilinear interpolation — the CPU MIRROR of
	// the runtime path (SgGiVolumeSample: each of the 8 cell corners
	// is weighted by its 6-axis obstacle distances vs the tap offset,
	// the renormalized sum replaces the plain trilerp; absent vis =
	// the plain trilerp). Returns false outside the AABB.
	bool Sample(const glm::vec3& p, glm::vec3 outAmps[kSgLobeCount]) const;

	// 7 RGBA16F 3D-texture payloads, byte-identical layout to
	// SgRegularVolume::PackGpuLayers (matches SgRegularUnpack).
	void PackGpuLayers(std::vector<uint16_t> out[7]) const;
	// 2 RGBA8_UNORM 3D-texture payloads — the visibility data
	// (visA = +x,-x,+y,-y; visB = +z,-z,cellClean,spare), each axis
	// distance quantized unorm8 over [0, 15] SPACING UNITS (15 units
	// is beyond any in-cell tap offset ~1.7, so the top of the range
	// reads as fully open; ~3 cm granularity at 0.5 m spacing — the
	// rejection ramp is 0.25*dist, an order coarser). Absent vis packs
	// all-open + all-clean = the no-rejection contract above.
	void PackGpuVisLayers(std::vector<uint8_t> out[2]) const;
	// Isotropic-mean payload (the volumetric fog's one-fetch-per-volume
	// path — SgGiVolumeSampleMean / sg_gi_volume.hlsli, consumed through the
	// ray-space composite SgGiVolumeRayComposite): one RGBA16F texel per
	// voxel, rgb = 0.124959 * Σ lobes. VolGiMean is linear in the amps,
	// so trilerp(Σ) == Σ(trilerp) — the SAME mean the fog's 7-fetch +
	// unpack + lobe-sum path computes, at 1/7 the fetches and none of
	// the lobe-array register pressure.
	void PackGpuMeanLayer(std::vector<uint16_t>& out) const;
	size_t GpuBytes() const; // 64 B/voxel resident (56 + 2 vis layers)

	bool WriteFile(const std::string& path, std::string& err) const;
	// Same bytes as WriteFile (header + LZ4 half payload), into memory —
	// the pak-embed path (PakCooker's --gi-volume stage bakes volumes
	// straight into pak Texture blobs).
	bool WriteToMemory(std::vector<uint8_t>& out, std::string& err) const;
	bool ReadFromMemory(std::span<const uint8_t> bytes, std::string& err);
	static bool ReadFile(const std::string& path, GiVolume& out, std::string& err);
};

// ---------------------------------------------------------------------------
// Bake (per-probe direct path trace)
// ---------------------------------------------------------------------------

struct GiVolumeBakeConfig {
	uint32_t captureDirs = 1536; // equal-solid-angle directions per
	                             // PROBE (6 * 16^2; the historical
	                             // name: these were the capture
	                             // "cubemap" texels before the
	                             // per-probe rework)
	uint32_t maxBounces = 3;     // diffuse path depth per probe ray
	// INERT since the per-probe rework (kept for CLI/Args compat):
	// the capture lattice + reprojection they tuned is gone.
	float captureSpacing = 0.0f;     // was: capture lattice (m)
	float maxReprojectRadius = 0.0f; // was: texel trust radius (m)
	uint32_t smoothingPasses = 3; // Laplacian passes over the fitted
	                              // grid (BO3 "volume smoothing"). The
	                              // per-probe bake needs it (the capture
	                              // reprojection was smooth by
	                              // construction; independent per-probe
	                              // estimates are not) and it softens the
	                              // dilation's near-surface seams.
	float smoothingLambda = 0.4f; // per-pass step (0..1)
	float probeMinClearance = 0.05f;  // probe validity (m, to geometry)
	float captureMinClearance = 0.10f; // INERT (was capture validity)
	// 6-axis visibility cap (m): obstacle distances record at
	// min(hitT, cap); a miss records the cap = "open". The oct-bake
	// parity value (2.5 m comfortably exceeds any in-cell tap offset —
	// spacing*sqrt(3) — so "capped" reads as fully open to the runtime
	// factor).
	float visibilityMaxDistance = 2.5f;
	uint32_t threadCount = 0;     // 0 = hardware concurrency
	uint32_t seed = 1;
};

struct GiVolumeBakeStats {
	uint64_t probes = 0;        // dense grid nodes
	uint64_t probesInvalid = 0; // inside solid geometry (dilated)
	uint64_t samplesClamped = 0; // firefly clamps (8x per-probe mean)
	double seconds = 0.0;
};

class GiTraceScene;
using GiBakeProgress = void (*)(const char* line);

// Bakes the volume's dense grid with a DIRECT per-probe path trace
// (see the file banner). The AABB grid covers def.Bounds() plus the
// fade band; probes outside the convex still bake (their runtime weight
// is zero but trilinear taps near the boundary read them).
bool BakeGiVolume(const GiTraceScene& scene, const GiVolumeDef& def,
	float spacing, const GiVolumeBakeConfig& config, GiVolume& outVolume,
	GiVolumeBakeStats& stats, GiBakeProgress progress = nullptr);

} // namespace smvk::gi
