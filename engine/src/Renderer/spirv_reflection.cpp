#include <Engine/Renderer/spirv_reflection.h>

#include <algorithm>
#include <cstring>

#include "spirv_reflect.h"

namespace smvk {
namespace {

constexpr uint32_t kSpirvMagic = 0x07230203u;

ShaderStage ToRhiStages(uint32_t reflectStages)
{
	ShaderStage out = ShaderStage::None;
	if (reflectStages & SPV_REFLECT_SHADER_STAGE_VERTEX_BIT)
		out |= ShaderStage::Vertex;
	if (reflectStages & SPV_REFLECT_SHADER_STAGE_TESSELLATION_CONTROL_BIT)
		out |= ShaderStage::TessControl;
	if (reflectStages & SPV_REFLECT_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)
		out |= ShaderStage::TessEvaluation;
	if (reflectStages & SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT)
		out |= ShaderStage::Geometry;
	if (reflectStages & SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT)
		out |= ShaderStage::Fragment;
	if (reflectStages & SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT)
		out |= ShaderStage::Compute;
	return out;
}

SpirvSpecConstant::Type ClassifySpecType(const SpvReflectTypeDescription* td)
{
	if (!td)
		return SpirvSpecConstant::Type::Unknown;
	if (td->type_flags & SPV_REFLECT_TYPE_FLAG_BOOL)
		return SpirvSpecConstant::Type::Bool;
	if (td->type_flags & SPV_REFLECT_TYPE_FLAG_INT) {
		const uint32_t w = td->traits.numeric.scalar.width;
		const uint32_t s = td->traits.numeric.scalar.signedness;
		if (w > 32)
			return s ? SpirvSpecConstant::Type::Int64 : SpirvSpecConstant::Type::UInt64;
		return s ? SpirvSpecConstant::Type::Int : SpirvSpecConstant::Type::UInt;
	}
	if (td->type_flags & SPV_REFLECT_TYPE_FLAG_FLOAT) {
		const uint32_t w = td->traits.numeric.scalar.width;
		return (w == 64) ? SpirvSpecConstant::Type::Double : SpirvSpecConstant::Type::Float;
	}
	return SpirvSpecConstant::Type::Unknown;
}

const char* ReflectResultString(SpvReflectResult r)
{
	switch (r) {
	case SPV_REFLECT_RESULT_SUCCESS: return "success";
	case SPV_REFLECT_RESULT_NOT_READY: return "not ready";
	case SPV_REFLECT_RESULT_ERROR_PARSE_FAILED: return "parse failed";
	case SPV_REFLECT_RESULT_ERROR_ALLOC_FAILED: return "alloc failed";
	case SPV_REFLECT_RESULT_ERROR_RANGE_EXCEEDED: return "range exceeded";
	case SPV_REFLECT_RESULT_ERROR_NULL_POINTER: return "null pointer";
	case SPV_REFLECT_RESULT_ERROR_INTERNAL_ERROR: return "internal error";
	case SPV_REFLECT_RESULT_ERROR_COUNT_MISMATCH: return "count mismatch";
	case SPV_REFLECT_RESULT_ERROR_ELEMENT_NOT_FOUND: return "element not found";
	case SPV_REFLECT_RESULT_ERROR_SPIRV_INVALID_CODE_SIZE: return "invalid code size";
	case SPV_REFLECT_RESULT_ERROR_SPIRV_INVALID_MAGIC_NUMBER: return "invalid magic";
	case SPV_REFLECT_RESULT_ERROR_SPIRV_UNEXPECTED_EOF: return "unexpected EOF";
	case SPV_REFLECT_RESULT_ERROR_SPIRV_INVALID_ID_REFERENCE: return "invalid id ref";
	case SPV_REFLECT_RESULT_ERROR_SPIRV_SET_NUMBER_OVERFLOW: return "set number overflow";
	case SPV_REFLECT_RESULT_ERROR_SPIRV_INVALID_STORAGE_CLASS: return "invalid storage class";
	case SPV_REFLECT_RESULT_ERROR_SPIRV_RECURSION: return "recursion";
	case SPV_REFLECT_RESULT_ERROR_SPIRV_INVALID_INSTRUCTION: return "invalid instruction";
	case SPV_REFLECT_RESULT_ERROR_SPIRV_UNEXPECTED_BLOCK_DATA: return "unexpected block data";
	case SPV_REFLECT_RESULT_ERROR_SPIRV_INVALID_BLOCK_MEMBER_REFERENCE: return "invalid block member ref";
	case SPV_REFLECT_RESULT_ERROR_SPIRV_INVALID_ENTRY_POINT: return "invalid entry point";
	case SPV_REFLECT_RESULT_ERROR_SPIRV_INVALID_EXECUTION_MODE: return "invalid execution mode";
	default: return "unknown error";
	}
}

} // namespace

SpirvReflection ReflectSpirv(std::span<const uint32_t> words)
{
	SpirvReflection out;
	if (words.empty()) {
		out.error = "empty SPIR-V";
		return out;
	}
	if (words[0] != kSpirvMagic) {
		out.error = "invalid SPIR-V magic";
		return out;
	}

	SpvReflectShaderModule module{};
	const size_t byteSize = words.size() * sizeof(uint32_t);
	const SpvReflectResult cr =
		spvReflectCreateShaderModule(byteSize, words.data(), &module);
	if (cr != SPV_REFLECT_RESULT_SUCCESS) {
		out.error = std::string("spvReflectCreateShaderModule: ") +
			ReflectResultString(cr);
		return out;
	}

	// Entry points (informational; push/spec enumeration below follows the
	// first entry point, matching SPIRV-Reflect semantics).
	for (uint32_t i = 0; i < module.entry_point_count; ++i) {
		const char* name = module.entry_points[i].name;
		out.entryPoints.emplace_back(name ? name : "");
	}

	// Push-constant blocks -> RHI ranges. Stage coverage comes from the
	// module's (first entry point) stage flags.
	const ShaderStage stages = ToRhiStages(module.shader_stage);
	uint32_t pcCount = 0;
	if (spvReflectEnumeratePushConstantBlocks(&module, &pcCount, nullptr) ==
		SPV_REFLECT_RESULT_SUCCESS) {
		std::vector<SpvReflectBlockVariable*> blocks(pcCount);
		if (spvReflectEnumeratePushConstantBlocks(&module, &pcCount, blocks.data()) ==
			SPV_REFLECT_RESULT_SUCCESS) {
			for (uint32_t i = 0; i < pcCount; ++i) {
				const auto* b = blocks[i];
				if (!b || b->size == 0)
					continue;
				PushConstantRange r{};
				r.offset = b->offset;
				r.size = b->size;
				r.stages = stages;
				out.pushConstants.push_back(r);
			}
		}
	}
	std::sort(out.pushConstants.begin(), out.pushConstants.end(),
		[](const PushConstantRange& a, const PushConstantRange& b) {
			return a.offset < b.offset;
		});

	// Specialization constants (OpSpecId decoracted OpSpecConstant*).
	uint32_t specCount = 0;
	if (spvReflectEnumerateSpecializationConstants(&module, &specCount, nullptr) ==
		SPV_REFLECT_RESULT_SUCCESS) {
		std::vector<SpvReflectSpecializationConstant*> specs(specCount);
		if (spvReflectEnumerateSpecializationConstants(&module, &specCount, specs.data()) ==
			SPV_REFLECT_RESULT_SUCCESS) {
			for (uint32_t i = 0; i < specCount; ++i) {
				const auto* s = specs[i];
				if (!s)
					continue;
				SpirvSpecConstant e{};
				e.specId = s->constant_id;
				e.type = ClassifySpecType(s->type_description);
				e.sizeBytes = s->default_value_size;
				e.hasDefault = (s->default_value != nullptr && s->default_value_size > 0);
				if (e.hasDefault) {
					const auto* bytes = static_cast<const uint8_t*>(s->default_value);
					e.defaultValue.assign(bytes, bytes + s->default_value_size);
				}
				if (s->name)
					e.name = s->name;
				out.specConstants.push_back(std::move(e));
			}
		}
	}
	std::sort(out.specConstants.begin(), out.specConstants.end(),
		[](const SpirvSpecConstant& a, const SpirvSpecConstant& b) {
			return a.specId < b.specId;
		});

	spvReflectDestroyShaderModule(&module);
	out.valid = true;
	return out;
}

SpirvReflection ReflectSpirvBytes(std::span<const char> bytes)
{
	if (bytes.size() % sizeof(uint32_t) != 0) {
		SpirvReflection out;
		out.error = "SPIR-V byte size not a multiple of 4";
		return out;
	}
	const auto* w = reinterpret_cast<const uint32_t*>(bytes.data());
	return ReflectSpirv(std::span<const uint32_t>(w, bytes.size() / sizeof(uint32_t)));
}

SpirvReflection ReflectSpirvBytes(std::span<const uint8_t> bytes)
{
	if (bytes.size() % sizeof(uint32_t) != 0) {
		SpirvReflection out;
		out.error = "SPIR-V byte size not a multiple of 4";
		return out;
	}
	const auto* w = reinterpret_cast<const uint32_t*>(bytes.data());
	return ReflectSpirv(std::span<const uint32_t>(w, bytes.size() / sizeof(uint32_t)));
}

std::vector<PushConstantRange> MergePushConstantRanges(
	const std::vector<std::vector<PushConstantRange>>& perModule)
{
	// Union stage flags per byte, then re-emit minimal ranges. Byte-wise is
	// simple and exact for the small sizes involved (<= a few hundred bytes).
	constexpr uint32_t kMaxBytes = 4096;
	std::vector<uint32_t> stageBits(kMaxBytes, 0);
	uint32_t extent = 0;
	for (const auto& mod : perModule) {
		for (const auto& r : mod) {
			if (r.size == 0)
				continue;
			const uint32_t end = r.offset + r.size;
			if (end > kMaxBytes)
				continue; // validated separately; skip here
			extent = std::max(extent, end);
			const auto bits = static_cast<uint32_t>(r.stages);
			for (uint32_t b = r.offset; b < end; ++b)
				stageBits[b] |= bits;
		}
	}
	std::vector<PushConstantRange> merged;
	uint32_t cur = 0;
	while (cur < extent) {
		if (stageBits[cur] == 0) {
			++cur;
			continue;
		}
		uint32_t end = cur + 1;
		while (end < extent && stageBits[end] == stageBits[cur])
			++end;
		PushConstantRange r{};
		r.offset = cur;
		r.size = end - cur;
		r.stages = static_cast<ShaderStage>(stageBits[cur]);
		merged.push_back(r);
		cur = end;
	}
	return merged;
}

bool ValidatePushConstants(const std::vector<PushConstantRange>& ranges,
                           uint32_t maxSize,
                           std::string* error)
{
	for (const auto& r : ranges) {
		if (r.size == 0) {
			if (error)
				*error = "zero-size push-constant range";
			return false;
		}
		if (r.offset + r.size > maxSize || r.offset + r.size < r.offset) {
			if (error)
				*error = "push-constant range [" + std::to_string(r.offset) + ", " +
					std::to_string(r.offset + r.size) + ") exceeds limit " +
					std::to_string(maxSize);
			return false;
		}
		if (r.stages == ShaderStage::None) {
			if (error)
				*error = "push-constant range has empty stage mask";
			return false;
		}
	}
	return true;
}

uint32_t PushConstantFootprint(const std::vector<PushConstantRange>& ranges)
{
	uint32_t end = 0;
	for (const auto& r : ranges)
		end = std::max(end, r.offset + r.size);
	return end;
}

ShaderStage ExpandPushStagesForLayout(ShaderStage requested,
                                      std::span<const PushConstantRange> layoutRanges,
                                      uint32_t offset,
                                      uint32_t size)
{
	if (size == 0)
		return requested;
	const uint64_t end = static_cast<uint64_t>(offset) + size;
	uint32_t bits = static_cast<uint32_t>(requested);
	for (const auto& r : layoutRanges) {
		if (r.size == 0 || r.stages == ShaderStage::None)
			continue;
		const uint64_t rEnd = static_cast<uint64_t>(r.offset) + r.size;
		// Overlap test on [offset, end) vs [r.offset, rEnd).
		if (static_cast<uint64_t>(r.offset) < end && offset < rEnd)
			bits |= static_cast<uint32_t>(r.stages);
	}
	return static_cast<ShaderStage>(bits);
}

} // namespace smvk
