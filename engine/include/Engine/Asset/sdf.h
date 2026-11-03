// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// A mesh-local, versioned signed/unsigned distance volume. This is a
// derived companion to a .geometry artifact, not a replacement for its
// meshlets/LODs. Grid origin is the CENTER of voxel (0,0,0); x is fastest.
// Samples are R16_SNORM: world distance = sample / 32767 * maxDistance.
// If signedDistance is false, the source was not watertight and samples
// are UNSIGNED; consumers must not interpret them as interior occupancy.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <glm/vec3.hpp>

namespace tour::asset {

inline constexpr uint32_t kSdfVersion = 1;

struct SdfVolume {
	uint32_t resolution = 0; // cubic grid, x fastest
	glm::vec3 origin{0.0f};  // center of first voxel, mesh-local units
	float voxelSize = 0.0f;
	float maxDistance = 0.0f; // saturating narrow band, mesh-local units
	bool signedDistance = false;
	std::vector<int16_t> samples;
};

struct SdfMesh {
	std::string fragment; // .geometry URI fragment, e.g. "mesh.0/prim.0"
	SdfVolume volume;
};

struct SdfAsset {
	std::vector<SdfMesh> meshes;
};

struct SdfBakeOptions {
	uint32_t resolution = 24; // [8,128], including padding on all sides
	uint32_t padding = 2;     // voxels on each edge, >=1
	uint32_t bandVoxels = 8;  // max distance encoded, >=1
	// Experimental meshoptimizer 1.3 remesher: reconstructs topology first.
	// OFF by default: it can close gaps/erase thin wires and change signs.
	bool voxelRemesh = false;
	int remeshResolution = 64; // meshopt_remesh accepts [4,256]
};

// Indexed triangles, mesh-local positions, CPU-only. On open or
// nonmanifold meshes the bake succeeds but returns unsigned distances.
// Rejects empty/degenerate input and impossible settings. Raw pointer +
// count at the boundary (the caller owns the memory; {nullptr, 0} is a
// valid empty input that fails validation loudly).
bool BakeMeshSdf(const glm::vec3* positions, size_t positionCount,
	const uint32_t* indices, size_t indexCount,
	const SdfBakeOptions& options, SdfVolume& out, std::string& err);

// TSDF v1, little-endian, bounds-checked, no struct padding or host
// endianness dependencies. A single sidecar groups a .geometry family's
// primitive volumes by stable URI fragment. Parse is transactional.
std::vector<uint8_t> WriteSdfAsset(const SdfAsset& asset);
bool ParseSdfAsset(const uint8_t* bytes, size_t byteCount, SdfAsset& out,
	std::string& err);

} // namespace tour::asset
