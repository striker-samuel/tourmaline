#pragma once
// Convex GI volumes: shaped local probe grids cooked by cubemap
// reprojection (Black Ops 3 style), blended over the world field.
//
// The GI pivot: probes only. The world is covered by the sparse octree
// (SgGiVolume, probe_volume.h); confined interiors/regions get AUTHORED
// convex volumes (one per room/street/alley) carrying a dense SG probe
// grid each. Lightmaps are deprecated and no longer cooked (the runtime
// loader stays for existing content).
//
// Stacking model (TPP/HZD/BO3 lineage — volumes BLEND, grids never
// cross-interpolate across density levels):
//
//   E = SampleGlobalOctree(P, N)      // or the sky fallback
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
// Bake (BakeCvVolume): dense probes cannot be path-traced per node at
// interior densities, so the cook follows the published BO3 pipeline —
// bake sparse radiance+depth captures (conceptually small cubemaps; here
// an equal-solid-angle direction set), then REPROJECT every capture into
// every nearby probe position:
//   1. capture lattice (coarser than the probe grid) inside the volume;
//   2. per capture: radiance + first-hit distance along each direction
//      (TraceIncoming, same transport as every other baked source);
//   3. per dense probe: trilinear weights over the <= 8 enclosing
//      captures; each capture's texels are parallax-shifted onto the
//      probe (direction to the recorded surface point), depth-gated by
//      the reprojection radius, and accumulated into the SG fit with
//      capture weight x texel solid angle;
//   4. diagonal SG projection (sg.h), validity dilation for probes in
//      solid geometry (regular_volume.cpp pattern), optional Laplacian
//      volume smoothing (BO3's "volume smoothing").
// A bake with captureSpacing == spacing at the capture points reduces
// exactly to the direct per-probe trace (the reprojection is the
// identity there), which is the calibration point for the math.
//
// Runtime: hardware trilinear over 7 RGBA16F 3D textures per volume
// (identical GPU layout to SgRegularVolume), weight/priority blend in
// sg_cvol.hlsli. File format: .cvol ("CVOL" v1, LZ4 half payload).

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

inline constexpr uint32_t kCvMaxPlanes = 16; // must match SG_CV_MAX_PLANES

struct CvPlane {
	glm::vec3 normal{ 0.0f, 1.0f, 0.0f }; // unit; inside: dot(n, P) + d <= 0
	float d = 0.0f;                       // plane offset (n . P + d)
	float insideFade = 0.1f;  // blend width INSIDE the face (m)
	float outsideFade = 0.1f; // blend width OUTSIDE the face (m) — the
	                          // volume keeps partial influence in the
	                          // doorway bleed zone
};

inline float CvSmoothStep(float e0, float e1, float x) {
	if (e1 - e0 < 1e-5f)
		return x >= e1 ? 1.0f : 0.0f;
	const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

// Per-plane fade weight (authored-fades reference; the GPU packs this
// into fadeScale/fadeBias form at upload time — see sg_cvol.hlsli for
// the algebraic equivalence and the degenerate-span note).
inline float CvPlaneWeight(const CvPlane& p, const glm::vec3& pos) {
	const float inside = -(glm::dot(p.normal, pos) + p.d);
	return CvSmoothStep(-p.outsideFade, p.insideFade, inside);
}

struct CvVolumeDef {
	std::vector<CvPlane> planes; // must bound a closed convex region
	uint32_t priority = 0;      // higher = composited later = wins
	std::string name;           // authoring id (bake output naming)

	// Per-face weight: min over planes of the per-plane smoothstep.
	float Weight(const glm::vec3& pos) const {
		float w = 1.0f;
		for (const CvPlane& p : planes) {
			w = std::min(w, CvPlaneWeight(p, pos));
			if (w <= 0.0f)
				break;
		}
		return w;
	}

	bool Contains(const glm::vec3& pos) const {
		for (const CvPlane& p : planes)
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
std::vector<CvPlane> CvBoxPlanes(const glm::vec3& mn, const glm::vec3& mx,
	float insideFade = 0.1f, float outsideFade = 0.1f);

// ROTATED box (OBB) helper: center + orthonormal axes as mat3 COLUMNS
// (normalized defensively) + half extents -> six planes. The authored
// cvolume nodes' rotation support: the dense probe lattice stays
// axis-aligned over the OBB's AABB (Bounds); the plane weights carve
// the rotated hull. Identity axes degenerate to CvBoxPlanes exactly.
// Face order: -u0, +u0, -u1, +u1, -u2, +u2.
std::vector<CvPlane> CvBoxPlanesRotated(const glm::vec3& center,
	const glm::mat3& axes, const glm::vec3& half,
	float insideFade = 0.1f, float outsideFade = 0.1f);

// ---------------------------------------------------------------------------
// Volume (cooked)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kCvMaxDim = 256; // per-axis grid cap (sgrv parity)

class CvVolume {
public:
	CvVolumeDef def;
	glm::vec3 gridMin{ 0.0f }; // AABB lattice origin (def.Bounds)
	float spacing = 0.5f;      // probe spacing (m)
	uint32_t dims[3] = { 1, 1, 1 };
	// Voxel-major, lobe-minor: coeffs[(z*Ny + y)*Nx + x][lobe] — the
	// SgRegularVolume layout, shared by the GPU packer below.
	std::vector<glm::vec3> coeffs;

	uint64_t VoxelCount() const;
	void Resize(); // allocate coeffs per dims (zeroed)

	// Trilinear coefficient interpolation (CPU mirror of the hardware
	// filtered 3D-texture path). Returns false outside the AABB.
	bool Sample(const glm::vec3& p, glm::vec3 outAmps[kSgLobeCount]) const;

	// 7 RGBA16F 3D-texture payloads, byte-identical layout to
	// SgRegularVolume::PackGpuLayers (matches SgRegularUnpack).
	void PackGpuLayers(std::vector<uint16_t> out[7]) const;
	// Isotropic-mean payload (the volumetric fog's one-fetch-per-volume
	// path ΓÇö SgCvCompositeMean / sg_cvol.hlsli): one RGBA16F texel per
	// voxel, rgb = 0.124959 * ╬ú lobes. VolGiMean is linear in the amps,
	// so trilerp(╬ú) == ╬ú(trilerp) ΓÇö the SAME mean the fog's 7-fetch +
	// unpack + lobe-sum path computes, at 1/7 the fetches and none of
	// the lobe-array register pressure.
	void PackGpuMeanLayer(std::vector<uint16_t>& out) const;
	size_t GpuBytes() const; // 56 B/voxel resident

	bool WriteFile(const std::string& path, std::string& err) const;
	// Same bytes as WriteFile (header + LZ4 half payload), into memory —
	// the pak-embed path (PakCooker's --cvolume stage bakes volumes
	// straight into pak Texture blobs).
	bool WriteToMemory(std::vector<uint8_t>& out, std::string& err) const;
	bool ReadFromMemory(std::span<const uint8_t> bytes, std::string& err);
	static bool ReadFile(const std::string& path, CvVolume& out, std::string& err);
};

// ---------------------------------------------------------------------------
// Bake (sparse captures + cubemap reprojection)
// ---------------------------------------------------------------------------

struct CvBakeConfig {
	uint32_t captureDirs = 1536; // equal-solid-angle directions per
	                             // capture (6 * 16^2; the conceptual
	                             // cubemap face resolution)
	uint32_t maxBounces = 3;     // diffuse path depth per capture ray
	float captureSpacing = 0.0f;     // capture lattice (m); 0 = 3x spacing
	float maxReprojectRadius = 0.0f; // texel trust radius (m);
	                                 // 0 = 4x capture spacing
	uint32_t smoothingPasses = 0; // Laplacian passes over the fitted
	                              // grid (BO3 "volume smoothing")
	float smoothingLambda = 0.4f; // per-pass step (0..1)
	float probeMinClearance = 0.05f;  // probe validity (m, to geometry)
	float captureMinClearance = 0.10f; // capture validity (m)
	uint32_t threadCount = 0;     // 0 = hardware concurrency
	uint32_t seed = 1;
};

struct CvBakeStats {
	uint64_t captures = 0;      // valid captures baked
	uint64_t captureRays = 0;   // path-traced capture rays
	uint64_t probes = 0;        // dense grid nodes
	uint64_t probesInvalid = 0; // inside solid geometry (dilated)
	uint64_t reprojected = 0;   // texel contributions accumulated
	uint64_t samplesClamped = 0; // firefly clamps at capture time
	double seconds = 0.0;
};

class GiTraceScene;
using GiBakeProgress = void (*)(const char* line);

// Bakes the volume's dense grid from sparse radiance+depth captures
// (see the file banner). The AABB grid covers def.Bounds() plus the
// fade band; probes outside the convex still bake (their runtime weight
// is zero but trilinear taps near the boundary read them).
bool BakeCvVolume(const GiTraceScene& scene, const CvVolumeDef& def,
	float spacing, const CvBakeConfig& config, CvVolume& outVolume,
	CvBakeStats& stats, GiBakeProgress progress = nullptr);

} // namespace smvk::gi
