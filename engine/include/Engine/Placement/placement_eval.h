// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// CPU reference evaluator for PlacementProgram + acceptance + transforms.
// This is the oracle: GPU kernels and baked caches must agree with it on
// accept/reject + prototype decisions (strong determinism), transforms
// may differ in the last ULP.

#include <Engine/Placement/placement_domain.h>
#include <Engine/Placement/placement_ids.h>
#include <Engine/Placement/placement_ir.h>

#include <glm/glm.hpp>

namespace smvk::placement {

// Evaluates the field program at one domain sample. Returns values[] parallel
// to program.instrs; density = values[program.acceptIndex].
std::vector<float> EvaluateProgram(const PlacementProgram& prog, const DomainSample& s);

// Quantize density to 8 bit BEFORE threshold comparison so CPU/GPU float
// differences cannot flip accept/reject (strong determinism, §35).
inline float QuantizeDensity(float d) {
	if (d <= 0.0f) return 0.0f;
	if (d >= 1.0f) return 1.0f;
	return (float)((int)(d * 255.0f)) * (1.0f / 255.0f);
}

// Stochastic acceptance: stable reveal (raising density only ADDS instances).
inline bool AcceptCandidate(float density, StableID id, float densityScale = 1.0f) {
	float d = QuantizeDensity(density * densityScale);
	if (d <= 0.0f) return false;
	if (d >= 1.0f) return true;
	return Random01(id, RandomChannel::Density) < d;
}

// Transform helpers (own RNG channels — never Density/Variant).
inline float YawFromId(StableID id) { // [-pi, +pi]
	return (Random01(id, RandomChannel::Rotation) * 2.0f - 1.0f) * 3.14159265f;
}
inline float UniformScaleFromId(StableID id, float minS, float maxS) {
	return minS + Random01(id, RandomChannel::Scale) * (maxS - minS);
}

// Deterministic integer value-noise in [0,1], worldXZ * freq + offset.
// Pure integer lattice hash — safe to mirror in HLSL.
float ValueNoise2D(float x, float z, uint64_t seed);

} // namespace smvk::placement
