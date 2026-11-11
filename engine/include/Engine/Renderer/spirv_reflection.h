// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// SPIR-V reflection readback: push-constant ranges + specialization constants.
//
// Wraps the vendored SPIRV-Reflect library behind RHI-only types (no Vk,
// no spirv_reflect.h leakage). All functions are CPU-only and headless-safe:
// they operate on raw SPIR-V words/bytes, never on a device.
//
// Typical use:
//   auto refl = smvk::ReflectSpirvBytes(code);          // code = .spv bytes
//   for (auto& r : refl.pushConstants) { ... }          // offset/size/stages
//   for (auto& s : refl.specConstants) { ... }          // specId/type/default
//
// Pipeline integration:
//   * Shader.specialization carries per-stage VkSpecializationInfo payloads.
//     Use refl.specConstants to discover valid constantIDs, then
//     ShaderSpecialization::Set(id, value) to override them.
//   * Push-constant reflection is informational: the engine keeps a single
//     global pipeline layout (256 bytes, ALL stages). Use
//     ValidatePushConstants() or MergePushConstantRanges() to check that a
//     program's reflected usage fits, and to derive minimal ranges when
//     building custom layouts.

#include <Engine/smvk.h>

#include <cassert>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace smvk {

// ---------------------------------------------------------------------------
// Reflected specialization constant (one OpSpecId).
// ---------------------------------------------------------------------------
struct SpirvSpecConstant {
    enum class Type : uint8_t {
        Bool,
        Int,
        UInt,
        Float,
        Double,
        Int64,
        UInt64,
        Unknown,
    };

    uint32_t specId = 0;
    Type type = Type::Unknown;
    // Size of one scalar default value in bytes (4 or 8).
    uint32_t sizeBytes = 0;
    bool hasDefault = false;
    // Raw little-endian default payload (sizeBytes long when hasDefault).
    std::vector<uint8_t> defaultValue;
    std::string name;

    bool operator==(const SpirvSpecConstant&) const = default;
};

// ---------------------------------------------------------------------------
// Reflected vertex-stage input (one non-builtin input variable: the vertex
// attribute locations the module actually consumes — DXC dead-code-strips
// unused inputs, so declared == consumed).
// ---------------------------------------------------------------------------
struct SpirvStageInput {
    uint32_t location = 0;
    // The input's scalar component count (1 for scalars, N for
    // vectors). The pipeline builder pairs the location against the
    // bound attribute's FORMAT and flags an input that consumes MORE
    // components than the format provides (the fetch would read
    // undefined data — the old reflection carried only the location,
    // so this mismatch passed silently).
    uint32_t componentCount = 0;
    std::string name;

    bool operator==(const SpirvStageInput&) const = default;
};

// ---------------------------------------------------------------------------
// Full per-module reflection result.
// ---------------------------------------------------------------------------
struct SpirvReflection {
    bool valid = false;
    std::string error;
    // One entry per push-constant block, with owning stage flags.
    // Offset/size are in bytes (lowest member offset .. block end).
    std::vector<PushConstantRange> pushConstants;
    std::vector<SpirvSpecConstant> specConstants;
    std::vector<std::string> entryPoints;
    // Non-builtin stage inputs (built-ins like gl_VertexIndex excluded;
    // meaningful for the VERTEX stage — the attribute locations the
    // module consumes; the pipeline builder filters the vertex format's
    // attribute list to these so no pipeline binds unconsumed
    // attributes, validation-clean).
    std::vector<SpirvStageInput> stageInputs;

    bool operator==(const SpirvReflection&) const = default;
};

// Reflect a single SPIR-V module. Words must start with the SPIR-V magic
// number (0x07230203). Returns valid=false + error on malformed input.
SpirvReflection ReflectSpirv(std::span<const uint32_t> words);

// Byte-view convenience wrapper (size must be a multiple of 4).
SpirvReflection ReflectSpirvBytes(std::span<const char> bytes);
SpirvReflection ReflectSpirvBytes(std::span<const uint8_t> bytes);

// Merge push-constant ranges from several modules (e.g. VS+FS) into a
// minimal set of non-overlapping ranges with unioned stage flags, suitable
// for VkPushConstantRange-style layouts. Ranges are sorted by offset.
std::vector<PushConstantRange> MergePushConstantRanges(
    const std::vector<std::vector<PushConstantRange>>& perModule);

// Validate that every reflected range lies within [0, maxSize), is a
// multiple of 4 (VUID-vkCmdPushConstants-offset-01795), and that total
// extent fits. Returns true when fits; otherwise fills error (with
// "unsupported ..." for limit/alignment failures).
bool ValidatePushConstants(const std::vector<PushConstantRange>& ranges,
                           uint32_t maxSize,
                           std::string* error = nullptr);

// Total push-constant footprint (max end offset) across merged ranges.
uint32_t PushConstantFootprint(const std::vector<PushConstantRange>& ranges);

// Rewrite OpDemoteToHelperInvocation -> OpKill and strip the
// DemoteToHelperInvocation capability, so the module no longer requires
// shaderDemoteToHelperInvocation (VUID-VkShaderModuleCreateInfo-pCode-08740).
// DXC maps HLSL `discard` to OpDemote when targeting vulkan1.3+ with no
// opt-out flag; the engine policy is classic kill semantics (no extra device
// feature, no extra PSOs). Demote is NOT a block terminator (DXC emits
// `demote; OpBranch %merge`), while kill IS, so each rewrite also erases the
// now-dead block tail through the terminator; a bare opcode swap would leave
// a branch after a terminator ("Branch must appear in a block"). Result ids
// outside the erased tails and the id bound are untouched. Operates in place
// on raw SPIR-V bytes (size must be a multiple of 4); returns the number of
// demote ops rewritten (0 = untouched, including malformed input which is
// left as-is). CPU-only, headless-safe.
uint32_t SanitizeDemoteToKill(std::vector<char>& spirvBytes);

// Expand a vkCmdPushConstants-style stage mask so it satisfies
// VUID-vkCmdPushConstants-offset-01796 against the given pipeline layout
// ranges: for every byte in [offset, offset + size) covered by a layout
// range, the layout range's stages are unioned into the result. Bytes with
// no covering range leave the mask unchanged. CPU-only, headless-safe.
ShaderStage ExpandPushStagesForLayout(ShaderStage requested,
                                      std::span<const PushConstantRange> layoutRanges,
                                      uint32_t offset,
                                      uint32_t size);

// Packs a 4x4 column-major matrix (glm layout, 16 floats) into the
// motion-vector push blob (12 floats) as THREE float4 ROWS of the affine
// transform (row-major: row 0 = [m00 m01 m02 tx], row 1 = [m10 m11 m12 ty],
// row 2 = [m20 m21 m22 tz]; the last row (0,0,0,1) is implied). For a
// column-major src, M[row][col] = src[4*col + row].
//
// The HLSL side reads these as a float4[3] and applies the transform as
// row dot-products (dot(prevModel[row], localPos)) instead of a matrix mul:
// a float4[3] carries no matrix-stride decoration, so the push constant
// passes spirv-val's relaxed block-layout rules (a float3x4 would emit
// stride 12, which spirv-val rejects as misaligned to 16). CPU-only.
inline void PackPushPrevModel(const float* srcColMajor16, float* dst12) {
    assert(srcColMajor16 && dst12);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            dst12[row * 4 + col] = srcColMajor16[4 * col + row];
        }
    }
}

} // namespace smvk
