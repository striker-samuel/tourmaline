// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Placement/placement_eval.h>

#include <algorithm>
#include <cmath>

namespace tour::placement {
using namespace ::smvk;

float ValueNoise2D(float x, float z, uint64_t seed) {
	// Integer lattice, hash-based gradient-free value noise (32-bit lanes).
	int64_t xi = (int64_t)std::floor(x);
	int64_t zi = (int64_t)std::floor(z);
	float fx = x - (float)xi;
	float fz = z - (float)zi;
	const uint32_t seedLo = (uint32_t)(seed & 0xFFFFFFFFull);
	const uint32_t seedHi = (uint32_t)(seed >> 32);
	auto lattice = [seedLo, seedHi](int64_t lx, int64_t lz) -> float {
		uint32_t h = seedLo ^ (seedHi + 0x9e3779b9u);
		h = HashMix32(h, (uint32_t)(lx & 0xFFFFFFFF));
		h = HashMix32(h, (uint32_t)(lz & 0xFFFFFFFF));
		return (float)(h >> 8) * (1.0f / 16777216.0f);
	};
	float v00 = lattice(xi, zi), v10 = lattice(xi + 1, zi);
	float v01 = lattice(xi, zi + 1), v11 = lattice(xi + 1, zi + 1);
	// Smootherstep weights.
	float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
	float uz = fz * fz * fz * (fz * (fz * 6.0f - 15.0f) + 10.0f);
	float a = v00 + (v10 - v00) * ux;
	float b = v01 + (v11 - v01) * ux;
	return a + (b - a) * uz;
}

static float Smoothstep(float e0, float e1, float x) {
	if (!(e1 > e0)) return x >= e1 ? 1.0f : 0.0f;
	float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

std::vector<float> EvaluateProgram(const PlacementProgram& prog, const DomainSample& s,
	const EcotopeMaskData* eco) {
	std::vector<float> v(prog.instrs.size(), 0.0f);
	const float slopeDeg = (s.n.y >= 1.0f) ? 0.0f
		: (s.n.y <= 0.0f) ? 90.0f
		: std::acos(std::clamp(s.n.y, -1.0f, 1.0f)) * 57.29578f;
	for (size_t i = 0; i < prog.instrs.size(); ++i) {
		const FieldInstr& in = prog.instrs[i];
		const float a = (in.a < v.size()) ? v[in.a] : 0.0f;
		const float b = (in.b < v.size()) ? v[in.b] : 0.0f;
		switch (in.op) {
		case FieldOp::Const: v[i] = in.params[0]; break;
		case FieldOp::PosX: v[i] = s.p.x; break;
		case FieldOp::PosY: v[i] = s.p.y; break;
		case FieldOp::PosZ: v[i] = s.p.z; break;
		case FieldOp::NormalY: v[i] = s.n.y; break;
		case FieldOp::Noise2:
			v[i] = ValueNoise2D(s.p.x * in.params[0] + in.params[1],
				s.p.z * in.params[0] + in.params[2], prog.graphSeed);
			break;
		case FieldOp::Add: v[i] = a + b; break;
		case FieldOp::Mul: v[i] = a * b; break;
		case FieldOp::Min: v[i] = std::min(a, b); break;
		case FieldOp::Max: v[i] = std::max(a, b); break;
		case FieldOp::Clamp01: v[i] = std::clamp(a, 0.0f, 1.0f); break;
		case FieldOp::Remap: {
			float denom = in.params[1] - in.params[0];
			float t = (denom != 0.0f) ? (a - in.params[0]) / denom : 0.0f;
			v[i] = in.params[2] + t * (in.params[3] - in.params[2]);
			break;
		}
		case FieldOp::Smoothstep: v[i] = Smoothstep(in.params[0], in.params[1], a); break;
		case FieldOp::Power: v[i] = std::pow(std::max(a, 0.0f), in.params[0]); break;
		case FieldOp::Invert: v[i] = 1.0f - a; break;
		case FieldOp::SlopeMask:
			v[i] = 1.0f - Smoothstep(in.params[0], in.params[1], slopeDeg);
			break;
		case FieldOp::AltitudeMask: {
			float lo = Smoothstep(in.params[0], in.params[1], s.p.y);
			float hi = 1.0f - Smoothstep(in.params[2], in.params[3], s.p.y);
			v[i] = lo * hi;
			break;
		}
		case FieldOp::EcotopeMask: {
			// params[0] = the paint channel. Out-of-range channels (a
			// malformed program — the GPU gate refuses them) read 0
			// defensively, mirroring the HLSL evaluator.
			const uint32_t ch = in.params[0] >= 0.0f
				? (uint32_t)in.params[0] : 0u;
			v[i] = eco ? eco->Sample(ch, s.p.x, s.p.z) : 0.0f;
			break;
		}
		}
	}
	return v;
}

} // namespace tour::placement
