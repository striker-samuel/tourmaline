// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// SgRegularVolume: Ready-at-Dawn-style regular SG probe lattice.
//
// (The historical contrast — the adaptive scattered octree point cloud
// with bricks, relocated probes, support spheres, cell lists and
// 6-axis visibility — was SgGiVolume, the deleted probe_volume.h,
// retired with the .sggi octree: stage 3 of the octree retirement.)
//
// This is the boring-and-regular alternative:
//
//   artist OBB -> regular Nx x Ny x Nz lattice -> trace each lattice point
//   -> diagonal project onto 9 fixed SGs -> store 9 RGB coefficients.
//
// Runtime is hardware trilinear coefficient interpolation:
//
//   worldPos -> OBB uvw -> 7 filtered 3D texture reads -> evaluate 9 SGs.
//
// No support radius, no cell lists, no visibility, no fallback, no
// smoothing, no relocation. Walls become volume boundaries: author one
// volume per room/street instead of inferring topology per probe.
//
// File format: .sgrv (SGRV v1, LZ4-compressed half payload, see .cpp).
// GPU layout: 27 halves/voxel packed into 7 RGBA16F texels (28 slots,
// 1 spare); PackGpuLayers() produces the 7 layer payloads in that order.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/GI/sg.h>

namespace smvk::gi {

// ---------------------------------------------------------------------------
// OBB descriptor (axis-aligned is orientation == identity)
// ---------------------------------------------------------------------------

struct SgRegularObb {
	glm::vec3 center{ 0.0f };       // world-space box center
	glm::vec3 axisX{ 1.0f, 0.0f, 0.0f }; // orthonormal orientation columns
	glm::vec3 axisY{ 0.0f, 1.0f, 0.0f };
	glm::vec3 axisZ{ 0.0f, 0.0f, 1.0f };
	glm::vec3 halfExtents{ 1.0f };  // half sizes along each axis (m)
	uint32_t dims[3] = { 8, 8, 8 }; // lattice resolution Nx, Ny, Nz

	glm::vec3 LatticePos(uint32_t ix, uint32_t iy, uint32_t iz) const;
	bool Contains(const glm::vec3& p) const;
	// World -> [-1, 1]^3 local per axis; uvw = local * 0.5 + 0.5.
	glm::vec3 WorldToLocal(const glm::vec3& p) const;
	glm::vec3 VoxelSpacing() const; // full spacing between neighbors (m)
};

// ---------------------------------------------------------------------------
// Volume
// ---------------------------------------------------------------------------

class SgRegularVolume {
public:
	SgRegularObb obb{};
	// Voxel-major, lobe-minor: coeffs[(z*Ny + y)*Nx + x][lobe].
	std::vector<glm::vec3> coeffs;

	uint64_t VoxelCount() const;
	void Resize(); // allocate coeffs per obb.dims (zeroed)

	// Trilinear coefficient interpolation (CPU mirror of the hardware
	// filtered 3D-texture path). Returns false when p is outside the OBB.
	bool Sample(const glm::vec3& p, glm::vec3 outAmps[kSgLobeCount]) const;

	// 7 RGBA16F 3D-texture payloads: out[l] holds Nx*Ny*Nz RGBA texels
	// (uint16 halves, row-major x, then y, then z). Texel t of voxel v
	// carries stream halves [t*4, t*4+3] of the 27-half lobe-major RGB
	// stream (h = lobe*3 + channel); slot 27 is zero padding.
	// Matches sg_regular.hlsli SgRegularUnpack.
	void PackGpuLayers(std::vector<uint16_t> out[7]) const;

	size_t GpuBytes() const; // 56 B/voxel resident (7 RGBA16F texels)

	bool WriteFile(const std::string& path, std::string& err) const;
	bool ReadFromMemory(std::span<const uint8_t> bytes, std::string& err);
	static bool ReadFile(const std::string& path, SgRegularVolume& out, std::string& err);
};

// ---------------------------------------------------------------------------
// Bake (fixed lattice, diagonal projection, no relocation/smoothing)
// ---------------------------------------------------------------------------

struct SgRegularBakeConfig {
	uint32_t raysPerProbe = 256; // uniform-sphere samples per lattice point
	uint32_t maxBounces = 3;     // diffuse path depth after the first hit
	uint32_t threadCount = 0;    // 0 = hardware concurrency
	uint32_t seed = 1;
	// Firefly clamp (GiVolumeBakeConfig::clampRatio's twin — 0 = OFF,
	// the energy A/B; the stats log the discarded fraction).
	float clampRatio = 8.0f;
};

struct SgRegularBakeStats {
	uint64_t voxelsBaked = 0;
	uint64_t pathsTraced = 0;
	uint64_t samplesClamped = 0;
	// The clamp's energy ledger (GiVolumeBakeStats::clampedEnergyRatio).
	double clampedEnergyRatio = 0.0;
	double seconds = 0.0;
};

class GiTraceScene;
using GiBakeProgress = void (*)(const char* line);

// Traces every lattice point at its FIXED position (never moved), fits
// with the unconditional diagonal projection, stores. Shared Fibonacci
// direction set across voxels (correlated error = smooth bias, not
// per-voxel speckle). Two-pass 8x-mean firefly clamp kept: sun pools
// are ~1e4 contrast and fat lobes turn that into hard-edged dots.
bool BakeSgRegularVolume(const GiTraceScene& scene, const SgRegularObb& obb,
	const SgRegularBakeConfig& config, SgRegularVolume& outVolume,
	SgRegularBakeStats& stats, GiBakeProgress progress = nullptr);

} // namespace smvk::gi
