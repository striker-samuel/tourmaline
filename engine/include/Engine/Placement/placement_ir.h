// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// PlacementProgram IR: minimal field-expression SSA for the MVP.
// Editor graphs compile DOWN to this; CPU and GPU both evaluate it.
// MVP opcodes only — extended ops (textures, SDF, curves) arrive later
// and follow the same {Op, args, params} shape.
//
// SSA rule: instructions[i] writes value[i]. Operands a/b are indices
// into values[] (< i) or, for Input ops, ignored.

#include <cstdint>
#include <vector>

namespace tour::placement {

// Keep in sync with placement_field.hlsli (same numeric values).
enum class FieldOp : uint32_t {
	Const = 0,      // params[0] = literal
	PosX = 1,       // domainSample.p.x
	PosY = 2,
	PosZ = 3,
	NormalY = 4,    // domainSample.n.y (1 = flat, 0 = vertical)
	Noise2 = 5,     // value-noise(worldXZ * params[0] + params[1]) in [0,1]
	Add = 6,        // values[a] + values[b]
	Mul = 7,        // values[a] * values[b]
	Min = 8,
	Max = 9,
	Clamp01 = 10,   // clamp(values[a], 0, 1)
	Remap = 11,     // remap a from [p0,p1] to [p2,p3]
	Smoothstep = 12,// smoothstep(p0, p1, values[a])
	Power = 13,     // pow(values[a], params[0])
	Invert = 14,    // 1 - values[a]
	SlopeMask = 15, // 1 - smoothstep(p0=maxFlatDeg, p1=steepDeg, slopeDeg)
	                // slopeDeg derived from NormalY. a ignored.
	AltitudeMask = 16, // smoothstep(p0, p1, y) * (1 - smoothstep(p2, p3, y))
	EcotopeMask = 17, // painted ecotope weight at the sample's world XZ.
	                  // params[0] = the mask CHANNEL index (validated
	                  // against the layer's EcotopeMaskData::channels);
	                  // bilinear, edge-clamped. Needs the layer's ecotope
	                  // mask upload — a program using this op refuses
	                  // GPU evaluation (and CPU returns 0) when the layer
	                  // carries no mask.
};

struct FieldInstr {
	FieldOp op = FieldOp::Const;
	uint32_t a = 0;
	uint32_t b = 0;
	float params[4] = { 0, 0, 0, 0 };
};

enum : uint32_t {
	PlacementCapability_CPU      = 1u << 0,
	PlacementCapability_GPU      = 1u << 1,
	PlacementCapability_BakeOnly = 1u << 2,
};

struct PlacementProgram {
	uint32_t version = 1;
	uint64_t graphSeed = 0;
	uint32_t nodeId = 0;        // generator node feeding StableIDs
	uint32_t capabilities = PlacementCapability_CPU | PlacementCapability_GPU;
	std::vector<FieldInstr> instrs; // value[i] per instruction
	uint32_t acceptIndex = 0;   // which value is the density (index into values)

	bool valid() const { return !instrs.empty() && acceptIndex < instrs.size(); }
};

// Builders (keep test/graph code readable without an editor).
inline FieldInstr OpConst(float v) { FieldInstr i; i.op = FieldOp::Const; i.params[0] = v; return i; }
inline FieldInstr OpAdd(uint32_t a, uint32_t b) { FieldInstr i; i.op = FieldOp::Add; i.a = a; i.b = b; return i; }
inline FieldInstr OpMul(uint32_t a, uint32_t b) { FieldInstr i; i.op = FieldOp::Mul; i.a = a; i.b = b; return i; }

} // namespace tour::placement


// COMPAT (the namespace/RHI boundary audit, todo item 8.4): this
// placement header's declarations live in tour::placement; the smvk::
// placement aliases keep the existing call sites compiling through the
// consumer migration.
namespace smvk::placement {
	using ::tour::placement::FieldInstr;
	using ::tour::placement::PlacementCapability_CPU;
	using ::tour::placement::PlacementCapability_GPU;
	using ::tour::placement::PlacementCapability_BakeOnly;
	using ::tour::placement::FieldOp;
	using ::tour::placement::OpConst;
	using ::tour::placement::OpAdd;
	using ::tour::placement::OpMul;
	using ::tour::placement::PlacementProgram;
}
