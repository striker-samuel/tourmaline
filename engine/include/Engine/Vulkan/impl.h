#pragma once

#include <Engine/smvk.h>

constexpr VkBufferUsageFlags bufferTypeToUsageFlags[9] = {
	VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,              // BufferType_Index
	VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,             // BufferType_Vertex
	VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, // BufferType_Uniform
	VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, // BufferType_Storage
	VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,           // BufferType_Indirect
	VK_BUFFER_USAGE_TRANSFER_SRC_BIT,              // BufferType_TransferSrc
	VK_BUFFER_USAGE_TRANSFER_DST_BIT,              // BufferType_TransferDst
	VK_BUFFER_USAGE_TRANSFER_SRC_BIT,              // BufferType_Staging: transfer with host visibility
	0,                                             // BufferType_Count sentinel
};

constexpr VmaMemoryUsage bufferTypeToVmaUsage[9] = {
	VMA_MEMORY_USAGE_GPU_ONLY,           // BufferType_Index: device-local only
	VMA_MEMORY_USAGE_GPU_ONLY,           // BufferType_Vertex: device-local only
	VMA_MEMORY_USAGE_GPU_ONLY,           // BufferType_Uniform: device-local only (with SHADER_DEVICE_ADDRESS_BIT)
	VMA_MEMORY_USAGE_GPU_ONLY,           // BufferType_Storage: device-local only (with SHADER_DEVICE_ADDRESS_BIT)
	VMA_MEMORY_USAGE_GPU_ONLY,           // BufferType_Indirect: device-local only
	VMA_MEMORY_USAGE_AUTO,               // BufferType_TransferSrc: auto-detect memory properties
	VMA_MEMORY_USAGE_AUTO,               // BufferType_TransferDst: auto-detect memory properties
	VMA_MEMORY_USAGE_AUTO_PREFER_HOST, // BufferType_Staging: auto-prefer host for CPU mapping
	VMA_MEMORY_USAGE_GPU_ONLY,           // BufferType_Count sentinel
};



// ============================================================================
// DepthSettings and BlendSettings are RHI-defined types in smvk.h.
// Translators convert them to Vk types for pipeline cache usage.
// ============================================================================

namespace smvk {

	inline bool FormatHasStencil(VkFormat format)
	{
		switch (format)
		{
		case VK_FORMAT_S8_UINT:
		case VK_FORMAT_D16_UNORM_S8_UINT:
		case VK_FORMAT_D24_UNORM_S8_UINT:
		case VK_FORMAT_D32_SFLOAT_S8_UINT:
			return true;

		default:
			return false;
		}
	}
	inline bool FormatHasDepth(VkFormat format)
	{
		switch (format)
		{
		case VK_FORMAT_D16_UNORM:
		case VK_FORMAT_X8_D24_UNORM_PACK32:
		case VK_FORMAT_D32_SFLOAT:
		case VK_FORMAT_D16_UNORM_S8_UINT:
		case VK_FORMAT_D24_UNORM_S8_UINT:
		case VK_FORMAT_D32_SFLOAT_S8_UINT:
			return true;

		default:
			return false;
		}
	}

	// ============================================================================
	// Type translators: convert RHI types from smvk.h to Vk types for pipeline state
	// ============================================================================


	inline VkShaderStageFlagBits toVkShaderStage(ShaderStage stage)
	{
		switch (stage)
		{
		case ShaderStage::Vertex:
			return VK_SHADER_STAGE_VERTEX_BIT;

		case ShaderStage::Fragment:
			return VK_SHADER_STAGE_FRAGMENT_BIT;

		case ShaderStage::Compute:
			return VK_SHADER_STAGE_COMPUTE_BIT;

		case ShaderStage::Geometry:
			return VK_SHADER_STAGE_GEOMETRY_BIT;

		case ShaderStage::TessControl:
			return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;

		case ShaderStage::TessEvaluation:
			return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;

		//case ShaderStage::Task:
		//	return VK_SHADER_STAGE_TASK_BIT_EXT;

		//case ShaderStage::Mesh:
		//	return VK_SHADER_STAGE_MESH_BIT_EXT;

		//case ShaderStage::RayGen:
		//	return VK_SHADER_STAGE_RAYGEN_BIT_KHR;

		//case ShaderStage::AnyHit:
		//	return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

		//case ShaderStage::ClosestHit:
		//	return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

		//case ShaderStage::Miss:
		//	return VK_SHADER_STAGE_MISS_BIT_KHR;

		//case ShaderStage::Intersection:
		//	return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;

		//case ShaderStage::Callable:
		//	return VK_SHADER_STAGE_CALLABLE_BIT_KHR;

		default:
			assert(false && "Unsupported ShaderStage");
			return VK_SHADER_STAGE_VERTEX_BIT;
		}
	}

	inline VkBlendFactor toVkSrcBlendFactor(
		BlendAttachmentState::SrcColorBlendFactor factor)
	{
		using Factor =
			BlendAttachmentState::SrcColorBlendFactor;

		switch (factor)
		{
		case Factor::Zero:
			return VK_BLEND_FACTOR_ZERO;

		case Factor::One:
			return VK_BLEND_FACTOR_ONE;

		case Factor::SrcAlpha:
			return VK_BLEND_FACTOR_SRC_ALPHA;

		case Factor::OneMinusSrcAlpha:
			return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		}

		assert(false);
		return VK_BLEND_FACTOR_ONE;
	}

	inline VkBlendFactor toVkDstBlendFactor(
		BlendAttachmentState::DstColorBlendFactor factor)
	{
		using Factor =
			BlendAttachmentState::DstColorBlendFactor;

		switch (factor)
		{
		case Factor::Zero:
			return VK_BLEND_FACTOR_ZERO;

		case Factor::One:
			return VK_BLEND_FACTOR_ONE;

		case Factor::OneMinusSrcAlpha:
			return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		}

		assert(false);
		return VK_BLEND_FACTOR_ONE;
	}

	inline VkBlendOp toVkBlendOp(
		BlendAttachmentState::ColorBlendOp op)
	{
		using Op =
			BlendAttachmentState::ColorBlendOp;

		switch (op)
		{
		case Op::Add:
			return VK_BLEND_OP_ADD;

		case Op::Subtract:
			return VK_BLEND_OP_SUBTRACT;

		case Op::ReverseSubtract:
			return VK_BLEND_OP_REVERSE_SUBTRACT;

		case Op::Min:
			return VK_BLEND_OP_MIN;

		case Op::Max:
			return VK_BLEND_OP_MAX;
		}

		assert(false);
		return VK_BLEND_OP_ADD;
	}

	inline VkBlendOp toVkBlendOp(
		BlendAttachmentState::AlphaBlendOp op)
	{
		using Op =
			BlendAttachmentState::AlphaBlendOp;

		switch (op)
		{
		case Op::Add:
			return VK_BLEND_OP_ADD;

		case Op::Subtract:
			return VK_BLEND_OP_SUBTRACT;

		case Op::ReverseSubtract:
			return VK_BLEND_OP_REVERSE_SUBTRACT;

		case Op::Min:
			return VK_BLEND_OP_MIN;

		case Op::Max:
			return VK_BLEND_OP_MAX;
		}

		assert(false);
		return VK_BLEND_OP_ADD;
	}

	inline VkColorComponentFlags toVkColorWriteMask(
		uint8_t mask)
	{
		using Flag =
			BlendAttachmentState::ColorComponentFlag;

		VkColorComponentFlags result = 0;

		if (mask & static_cast<uint8_t>(Flag::R))
			result |= VK_COLOR_COMPONENT_R_BIT;

		if (mask & static_cast<uint8_t>(Flag::G))
			result |= VK_COLOR_COMPONENT_G_BIT;

		if (mask & static_cast<uint8_t>(Flag::B))
			result |= VK_COLOR_COMPONENT_B_BIT;

		if (mask & static_cast<uint8_t>(Flag::A))
			result |= VK_COLOR_COMPONENT_A_BIT;

		return result;
	}

	inline VkPrimitiveTopology toVkPrimitiveTopology(
		PrimitiveTopology topology)
	{
		switch (topology)
		{
		case PrimitiveTopology::PointList:
			return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

		case PrimitiveTopology::LineList:
			return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

		case PrimitiveTopology::LineStrip:
			return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;

		case PrimitiveTopology::TriangleList:
			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		case PrimitiveTopology::TriangleStrip:
			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

		case PrimitiveTopology::TriangleFan:
			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
		}

		assert(false);
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	}

	inline VkPolygonMode toVkPolygonMode(
		PolygonMode mode)
	{
		switch (mode)
		{
		case PolygonMode::Fill:
			return VK_POLYGON_MODE_FILL;

		case PolygonMode::Line:
			return VK_POLYGON_MODE_LINE;

		case PolygonMode::Point:
			return VK_POLYGON_MODE_POINT;
		}

		assert(false);
		return VK_POLYGON_MODE_FILL;
	}

	inline VkCullModeFlags toVkCullMode(
		CullMode mode)
	{
		switch (mode)
		{
		case CullMode::None:
			return VK_CULL_MODE_NONE;

		case CullMode::Front:
			return VK_CULL_MODE_FRONT_BIT;

		case CullMode::Back:
			return VK_CULL_MODE_BACK_BIT;

		case CullMode::FrontAndBack:
			return VK_CULL_MODE_FRONT_AND_BACK;
		}

		assert(false);
		return VK_CULL_MODE_NONE;
	}

	inline VkFrontFace toVkFrontFace(
		FrontFace face)
	{
		switch (face)
		{
		case FrontFace::CounterClockwise:
			return VK_FRONT_FACE_COUNTER_CLOCKWISE;

		case FrontFace::Clockwise:
			return VK_FRONT_FACE_CLOCKWISE;
		}

		assert(false);
		return VK_FRONT_FACE_COUNTER_CLOCKWISE;
	}

	inline VkSampleCountFlagBits toVkSampleCount(
		SampleCount samples)
	{
		switch (samples)
		{
		case SampleCount::Count1:
			return VK_SAMPLE_COUNT_1_BIT;

		case SampleCount::Count2:
			return VK_SAMPLE_COUNT_2_BIT;

		case SampleCount::Count4:
			return VK_SAMPLE_COUNT_4_BIT;

		case SampleCount::Count8:
			return VK_SAMPLE_COUNT_8_BIT;

		case SampleCount::Count16:
			return VK_SAMPLE_COUNT_16_BIT;

		case SampleCount::Count32:
			return VK_SAMPLE_COUNT_32_BIT;

		case SampleCount::Count64:
			return VK_SAMPLE_COUNT_64_BIT;
		}

		assert(false);
		return VK_SAMPLE_COUNT_1_BIT;
	}

	// CompareOp (RHI enum) -> VkCompareOp
	inline VkCompareOp toVkCompareOp(smvk::CompareOp op)
	{
		switch (op)
		{
		case smvk::CompareOp::Never:        return VK_COMPARE_OP_NEVER;
		case smvk::CompareOp::Less:         return VK_COMPARE_OP_LESS;
		case smvk::CompareOp::Equal:        return VK_COMPARE_OP_EQUAL;
		case smvk::CompareOp::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
		case smvk::CompareOp::Greater:      return VK_COMPARE_OP_GREATER;
		case smvk::CompareOp::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
		case smvk::CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
		case smvk::CompareOp::Always:       return VK_COMPARE_OP_ALWAYS;
		default:                            return VK_COMPARE_OP_ALWAYS;
		}
	}
	inline VkFormat toVkFormat(smvk::Format format)
	{
		switch (format)
		{
		case smvk::Format::Undefined:           return VK_FORMAT_UNDEFINED;

			// 8-bit
		case smvk::Format::R8_UNorm:            return VK_FORMAT_R8_UNORM;
		case smvk::Format::R8_SNorm:            return VK_FORMAT_R8_SNORM;
		case smvk::Format::R8_UInt:             return VK_FORMAT_R8_UINT;
		case smvk::Format::R8_SInt:             return VK_FORMAT_R8_SINT;

		case smvk::Format::RG8_UNorm:           return VK_FORMAT_R8G8_UNORM;
		case smvk::Format::RG8_SNorm:           return VK_FORMAT_R8G8_SNORM;
		case smvk::Format::RG8_UInt:            return VK_FORMAT_R8G8_UINT;
		case smvk::Format::RG8_SInt:            return VK_FORMAT_R8G8_SINT;

		case smvk::Format::RGBA8_UNorm:         return VK_FORMAT_R8G8B8A8_UNORM;
		case smvk::Format::RGBA8_SNorm:         return VK_FORMAT_R8G8B8A8_SNORM;
		case smvk::Format::RGBA8_UInt:          return VK_FORMAT_R8G8B8A8_UINT;
		case smvk::Format::RGBA8_SInt:          return VK_FORMAT_R8G8B8A8_SINT;
		case smvk::Format::RGBA8_SRGB:          return VK_FORMAT_R8G8B8A8_SRGB;

		case smvk::Format::BGRA8_UNorm:         return VK_FORMAT_B8G8R8A8_UNORM;
		case smvk::Format::BGRA8_SRGB:          return VK_FORMAT_B8G8R8A8_SRGB;

			// 16-bit
		case smvk::Format::R16_UNorm:           return VK_FORMAT_R16_UNORM;
		case smvk::Format::R16_SNorm:           return VK_FORMAT_R16_SNORM;
		case smvk::Format::R16_UInt:            return VK_FORMAT_R16_UINT;
		case smvk::Format::R16_SInt:            return VK_FORMAT_R16_SINT;
		case smvk::Format::R16_Float:           return VK_FORMAT_R16_SFLOAT;

		case smvk::Format::RG16_UNorm:          return VK_FORMAT_R16G16_UNORM;
		case smvk::Format::RG16_SNorm:          return VK_FORMAT_R16G16_SNORM;
		case smvk::Format::RG16_UInt:           return VK_FORMAT_R16G16_UINT;
		case smvk::Format::RG16_SInt:           return VK_FORMAT_R16G16_SINT;
		case smvk::Format::RG16_Float:          return VK_FORMAT_R16G16_SFLOAT;

		case smvk::Format::RGBA16_UNorm:        return VK_FORMAT_R16G16B16A16_UNORM;
		case smvk::Format::RGBA16_SNorm:        return VK_FORMAT_R16G16B16A16_SNORM;
		case smvk::Format::RGBA16_UInt:         return VK_FORMAT_R16G16B16A16_UINT;
		case smvk::Format::RGBA16_SInt:         return VK_FORMAT_R16G16B16A16_SINT;
		case smvk::Format::RGBA16_Float:        return VK_FORMAT_R16G16B16A16_SFLOAT;

			// 32-bit
		case smvk::Format::R32_UInt:            return VK_FORMAT_R32_UINT;
		case smvk::Format::R32_SInt:            return VK_FORMAT_R32_SINT;
		case smvk::Format::R32_Float:           return VK_FORMAT_R32_SFLOAT;

		case smvk::Format::RG32_UInt:           return VK_FORMAT_R32G32_UINT;
		case smvk::Format::RG32_SInt:           return VK_FORMAT_R32G32_SINT;
		case smvk::Format::RG32_Float:          return VK_FORMAT_R32G32_SFLOAT;

		case smvk::Format::RGB32_UInt:          return VK_FORMAT_R32G32B32_UINT;
		case smvk::Format::RGB32_SInt:          return VK_FORMAT_R32G32B32_SINT;
		case smvk::Format::RGB32_Float:         return VK_FORMAT_R32G32B32_SFLOAT;

		case smvk::Format::RGBA32_UInt:         return VK_FORMAT_R32G32B32A32_UINT;
		case smvk::Format::RGBA32_SInt:         return VK_FORMAT_R32G32B32A32_SINT;
		case smvk::Format::RGBA32_Float:        return VK_FORMAT_R32G32B32A32_SFLOAT;

			// packed
		case smvk::Format::RGB10A2_UNorm:       return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		case smvk::Format::RGB10A2_UInt:        return VK_FORMAT_A2B10G10R10_UINT_PACK32;
		case smvk::Format::R11G11B10_Float:     return VK_FORMAT_B10G11R11_UFLOAT_PACK32;

			// depth / stencil
		case smvk::Format::D16_UNorm:           return VK_FORMAT_D16_UNORM;
		case smvk::Format::D24_UNorm_S8_UInt:   return VK_FORMAT_D24_UNORM_S8_UINT;
		case smvk::Format::D32_Float:           return VK_FORMAT_D32_SFLOAT;
		case smvk::Format::D32_Float_S8_UInt:   return VK_FORMAT_D32_SFLOAT_S8_UINT;

			// compressed
		case smvk::Format::BC1_RGBA_UNorm:      return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
		case smvk::Format::BC1_RGBA_SRGB:       return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
		case smvk::Format::BC2_UNorm:           return VK_FORMAT_BC2_UNORM_BLOCK;
		case smvk::Format::BC2_SRGB:            return VK_FORMAT_BC2_SRGB_BLOCK;
		case smvk::Format::BC3_UNorm:           return VK_FORMAT_BC3_UNORM_BLOCK;
		case smvk::Format::BC3_SRGB:            return VK_FORMAT_BC3_SRGB_BLOCK;
		case smvk::Format::BC4_UNorm:           return VK_FORMAT_BC4_UNORM_BLOCK;
		case smvk::Format::BC4_SNorm:           return VK_FORMAT_BC4_SNORM_BLOCK;
		case smvk::Format::BC5_UNorm:           return VK_FORMAT_BC5_UNORM_BLOCK;
		case smvk::Format::BC5_SNorm:           return VK_FORMAT_BC5_SNORM_BLOCK;
		case smvk::Format::BC6H_UFloat:         return VK_FORMAT_BC6H_UFLOAT_BLOCK;
		case smvk::Format::BC6H_SFloat:         return VK_FORMAT_BC6H_SFLOAT_BLOCK;
		case smvk::Format::BC7_UNorm:           return VK_FORMAT_BC7_UNORM_BLOCK;
		case smvk::Format::BC7_SRGB:            return VK_FORMAT_BC7_SRGB_BLOCK;
		}

		return VK_FORMAT_UNDEFINED;
	}

	// BlendAttachmentState factors/ops -> Vk blend factors/ops
	inline VkBlendFactor toVkBlendFactor(smvk::BlendAttachmentState::SrcColorBlendFactor factor)
	{
		switch (factor)
		{
		case smvk::BlendAttachmentState::SrcColorBlendFactor::Zero:       return VK_BLEND_FACTOR_ZERO;
		case smvk::BlendAttachmentState::SrcColorBlendFactor::One:        return VK_BLEND_FACTOR_ONE;
		case smvk::BlendAttachmentState::SrcColorBlendFactor::SrcAlpha:   return VK_BLEND_FACTOR_SRC_ALPHA;
		case smvk::BlendAttachmentState::SrcColorBlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		default: return VK_BLEND_FACTOR_ONE;
		}
	}

	inline VkBlendFactor toVkBlendFactor(smvk::BlendAttachmentState::DstColorBlendFactor factor)
	{
		switch (factor)
		{
		case smvk::BlendAttachmentState::DstColorBlendFactor::Zero:       return VK_BLEND_FACTOR_ZERO;
		case smvk::BlendAttachmentState::DstColorBlendFactor::One:        return VK_BLEND_FACTOR_ONE;
		case smvk::BlendAttachmentState::DstColorBlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		default: return VK_BLEND_FACTOR_ONE;
		}
	}

	inline VkBlendOp toVkBlendOp(smvk::BlendAttachmentState::ColorBlendOp op)
	{
		switch (op)
		{
		case smvk::BlendAttachmentState::ColorBlendOp::Add:             return VK_BLEND_OP_ADD;
		case smvk::BlendAttachmentState::ColorBlendOp::Subtract:        return VK_BLEND_OP_SUBTRACT;
		case smvk::BlendAttachmentState::ColorBlendOp::ReverseSubtract:  return VK_BLEND_OP_REVERSE_SUBTRACT;
		case smvk::BlendAttachmentState::ColorBlendOp::Min:             return VK_BLEND_OP_MIN;
		case smvk::BlendAttachmentState::ColorBlendOp::Max:             return VK_BLEND_OP_MAX;
		default: return VK_BLEND_OP_ADD;
		}
	}

	inline VkBlendOp toVkBlendOp(smvk::BlendAttachmentState::AlphaBlendOp op)
	{
		switch (op)
		{
		case smvk::BlendAttachmentState::AlphaBlendOp::Add:             return VK_BLEND_OP_ADD;
		case smvk::BlendAttachmentState::AlphaBlendOp::Subtract:        return VK_BLEND_OP_SUBTRACT;
		case smvk::BlendAttachmentState::AlphaBlendOp::ReverseSubtract:  return VK_BLEND_OP_REVERSE_SUBTRACT;
		case smvk::BlendAttachmentState::AlphaBlendOp::Min:             return VK_BLEND_OP_MIN;
		case smvk::BlendAttachmentState::AlphaBlendOp::Max:             return VK_BLEND_OP_MAX;
		default: return VK_BLEND_OP_ADD;
		}
	}

	// VkColorComponentFlags -> BlendAttachmentState color write mask bits
	inline uint8_t toRhiColorWriteMask(VkColorComponentFlags flags)
	{
		uint8_t mask = 0;
		if (flags & VK_COLOR_COMPONENT_R_BIT) mask |= static_cast<uint8_t>(smvk::BlendAttachmentState::ColorComponentFlag::R);
		if (flags & VK_COLOR_COMPONENT_G_BIT) mask |= static_cast<uint8_t>(smvk::BlendAttachmentState::ColorComponentFlag::G);
		if (flags & VK_COLOR_COMPONENT_B_BIT) mask |= static_cast<uint8_t>(smvk::BlendAttachmentState::ColorComponentFlag::B);
		if (flags & VK_COLOR_COMPONENT_A_BIT) mask |= static_cast<uint8_t>(smvk::BlendAttachmentState::ColorComponentFlag::A);
		return mask;
	}

	// VkBlendFactor/Op/ColorComponentFlags -> BlendAttachmentState
	inline smvk::BlendAttachmentState::SrcColorBlendFactor toRhiSrcColorBlendFactor(VkBlendFactor factor)
	{
		switch (factor)
		{
		case VK_BLEND_FACTOR_ZERO:          return smvk::BlendAttachmentState::SrcColorBlendFactor::Zero;
		case VK_BLEND_FACTOR_ONE:         return smvk::BlendAttachmentState::SrcColorBlendFactor::One;
		case VK_BLEND_FACTOR_SRC_ALPHA:   return smvk::BlendAttachmentState::SrcColorBlendFactor::SrcAlpha;
		case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return smvk::BlendAttachmentState::SrcColorBlendFactor::OneMinusSrcAlpha;
		default: return smvk::BlendAttachmentState::SrcColorBlendFactor::One;
		}
	}

	inline smvk::BlendAttachmentState::DstColorBlendFactor toRhiDstColorBlendFactor(VkBlendFactor factor)
	{
		switch (factor)
		{
		case VK_BLEND_FACTOR_ZERO:          return smvk::BlendAttachmentState::DstColorBlendFactor::Zero;
		case VK_BLEND_FACTOR_ONE:         return smvk::BlendAttachmentState::DstColorBlendFactor::One;
		case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return smvk::BlendAttachmentState::DstColorBlendFactor::OneMinusSrcAlpha;
		default: return smvk::BlendAttachmentState::DstColorBlendFactor::One;
		}
	}

	// Just VK_1_3 for the moment, should be more than ok
	// https://asawicki.info/articles/state_of_gpu_hardware_2025.php 
	enum VulkanVersion {
		VK_1_3
		//, VK_1_4
	};

	struct VulkanCore {
		//static VulkanCore& Get()
		//{
		//	static VulkanCore instance;
		//	return instance;
		//}

		struct CoreConfiguration {
			VulkanVersion version_ = VK_1_3;
			const std::vector<const char*> validationLayers = { "VK_LAYER_KHRONOS_validation", };
			const std::vector<const char*> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
			const std::vector<const char*> instanceExtensions = {};
		} config_;

		VkPhysicalDeviceDriverProperties vkPhysicalDeviceDriverProperties_ = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
		nullptr
		};

		VkPhysicalDeviceDepthStencilResolveProperties vkPhysicalDeviceDepthStencilResolveProperties_ = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES,
			&vkPhysicalDeviceDriverProperties_
		};

		//VkPhysicalDeviceVulkan14Properties vkPhysicalDeviceVulkan14Properties_ = {
		//VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_PROPERTIES,
		//&vkPhysicalDeviceDepthStencilResolveProperties_
		//};

		VkPhysicalDeviceVulkan13Properties vkPhysicalDeviceVulkan13Properties_ = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
			&vkPhysicalDeviceDepthStencilResolveProperties_
		};

		VkPhysicalDeviceVulkan12Properties vkPhysicalDeviceVulkan12Properties_ = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
			&vkPhysicalDeviceVulkan13Properties_
		};

		VkPhysicalDeviceVulkan11Properties vkPhysicalDeviceVulkan11Properties_ = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
			&vkPhysicalDeviceVulkan12Properties_
		};

		VkPhysicalDeviceProperties2 vkPhysicalDeviceProperties2_ = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			&vkPhysicalDeviceVulkan11Properties_,
			{}
		};
		VkPhysicalDeviceProperties2& GetPhysicalDeviceProperties() { return this->vkPhysicalDeviceProperties2_; };

		//VkPhysicalDeviceVulkan14Features vkFeatures14_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES };
		VkPhysicalDeviceVulkan13Features vkFeatures13_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
		VkPhysicalDeviceVulkan12Features vkFeatures12_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &vkFeatures13_ };
		VkPhysicalDeviceVulkan11Features vkFeatures11_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &vkFeatures12_ };
		VkPhysicalDeviceFeatures2 vkFeatures10_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &vkFeatures11_ };

		// dont like this data definition for queue exposure.
		QueueFamilyIndices indices;

	public:

		VulkanCore(CoreConfiguration config = {}) :config_(config) {};
		~VulkanCore();
		VulkanCore(const VulkanCore&) = delete;
		VulkanCore& operator=(const VulkanCore&) = delete;
		VulkanCore(VulkanCore&&) = delete;
		VulkanCore& operator=(VulkanCore&&) = delete;

		int CreateInstance();
		bool Init();

		//VkInstance GetInstance();
		VkDevice GetDevice();
		//VkPhysicalDevice GetPhysicalDevice();
		//VkQueue GetGraphicsQueue();

		void CreateSampler(const SamplerCreateInfo& samplerCreateInfo, VkSampler& sampler);
		void CreateImageView(ImageViewCreateInfo imageViewCreateInfo, VkImageView& view);

		void CreateGraphicsPipeline(VkGraphicsPipelineCreateInfo createInfo, VkPipeline& pipeline);
		void CreateComputePipeline(VkComputePipelineCreateInfo createInfo, VkPipeline& pipeline);

		void DestroyPipeline(VkPipeline pipeline);
	};

	std::unique_ptr<smvk::VulkanCore> CreateVulkanCoreWithWindow(smvk::VulkanCore::CoreConfiguration config, GLFWwindow* window);
	//std::unique_ptr<smvk::VulkanCore> CreateVulkanCoreHeadless(smvk::VulkanCore::CoreConfiguration config);


	//
	struct CommandSubmitHandle {
		uint32_t index_ = 0;
		uint32_t gen_ = 0; // gen 0 means no slot. alas empty. 
		bool empty() const { return gen_ == 0; };
	};
	struct Submitter {
		struct SemaphoreState {
			VkSemaphore semaphore_ = VK_NULL_HANDLE;
			uint64_t initialValue_ = 0;
			uint64_t goal_ = 1; //defautl to 1
		};
		//
		struct CommandBuffer { // duplicated from rendergraph. commandBuffer
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			SemaphoreState finishedSignalSemaphore{};
			bool isEncoding_ = false;
		};
		struct WrapperSlot {
			CommandBuffer wrapper_{};
			VkCommandBuffer allocatedCmd_ = VK_NULL_HANDLE;
			uint32_t gen_ = 1;
			//uint32_t index = UINT32_MAX;  // load balancing gens sorting and using this
		};
		Submitter(VulkanCore& core, uint32_t queueIndex);
		~Submitter();
		CommandSubmitHandle acquire(uint32_t goal = 1);
		CommandBuffer& GetCommandBuffer(CommandSubmitHandle handle);
		void submit(CommandSubmitHandle handle);
		bool validHandle(CommandSubmitHandle handle) const;
		bool isReady(CommandSubmitHandle handle);
		void wait(CommandSubmitHandle handle, uint64_t timeout = UINT64_MAX);
		void waitAll();
		void purge();
	private:
		void reset(CommandSubmitHandle handle);
		VulkanCore& core_;
		VkCommandPool pool_;
		VkQueue m_queue = VK_NULL_HANDLE;
		static const uint32_t kMax = 12;
		uint32_t availableSlots = kMax;
		WrapperSlot commandBuffers_[kMax];
		CommandSubmitHandle m_lastSubmit{}; // not in use right now. used for creating linkedList for a faux dependency chain

		VkSemaphore m_timelineSemaphore = VK_NULL_HANDLE;
		uint64_t m_timelineValue = 1;
	};
	struct DeferredTask {
		DeferredTask(std::packaged_task<void()>&& task, CommandSubmitHandle handle) : task_(std::move(task)), handle_(handle) {}
		std::packaged_task<void()> task_;
		CommandSubmitHandle handle_;
	};


	struct VulkanResourceManager {
		VulkanResourceManager(VulkanCore& core);
		~VulkanResourceManager();
		VulkanResourceManager(const VulkanResourceManager&) = delete;
		VulkanResourceManager& operator=(const VulkanResourceManager&) = delete;
		VulkanResourceManager(VulkanResourceManager&&) = delete;
		VulkanResourceManager& operator=(VulkanResourceManager&&) = delete;

		int Initialize();
		// 	
		void AddDeferredTask(std::packaged_task<void()>&& task, CommandSubmitHandle handle = CommandSubmitHandle{});
		void ProcessDeferrredTasks();
		//
		Holder<ImageHandle> CreateImage(ImageCreateInfo imageCreateInfo, const void* data = nullptr, const char* debugName = nullptr);
		Holder<TextureHandle> CreateTexture(ImageCreateInfo imageCreateInfo, const void* data = nullptr, const char* debugName = nullptr); // specialization for textures instead of .isTexture property
		// dont like ther hardcode dep to ktx2
		Holder<TextureHandle> CreateTexture(ktxTexture2& texture, const char* debugName = nullptr);
		Holder<BufferHandle> CreateBuffer(VkDeviceSize size, BufferType bufferType, const char* debugName = nullptr);
		Holder<ShaderModuleHandle> CreateShaderModule(std::vector<char> code, const char* debugName = nullptr);
		Holder<DescriptorPoolHandle> CreateDescriptorPool(std::vector<VkDescriptorPoolSize> poolSizes, const char* debugName = nullptr);

		void* MapBuffer(smvk::Handle<smvk::Buffer> handle);
		void UnmapBuffer(smvk::Handle<smvk::Buffer> handle);

		void UploadDataToBuffer(BufferHandle buffer, const void* data, size_t dataSize, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess, size_t src_offset = 0, size_t dest_offset = 0);
		void UploadDataToImage2D(Image img, const void* data, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess, size_t src_offset = 0, size_t dest_offset = 0);


		void UpdateBindings(uint32_t frameIndex = 0);

		// VulkanImpl
		VulkanCore& core_;
		VmaAllocator m_allocator;
		Submitter* m_submitter = nullptr;
		std::vector<DeferredTask> m_deferredTasks;

		//  Binding Model
		VkDescriptorSetLayout defaultSetLayout{};
		VkPipelineLayout defaultPipeLayout{};
		VkDescriptorSet defaultDescriptorSets_[MAX_FRAMES_IN_FLIGHT];

		// Resources
		Pool<Image, Image> images_; // VkSwapchainImages are not represented with Image type and just owned by the swapchain
		Pool<Texture, Image> textures_; // specializaed textures away from images
		Pool<Buffer, Buffer> buffers_;
		Pool<Sampler, VkSampler> samplers_;

		Pool<DescriptorPool, VkDescriptorPool> descriptorPools_; // Maybe we should do away with this as we only manage one pool, and one for Imgui
		Pool<ShaderModule, VkShaderModule> shaderModules_; // this should be removed when moving to vk1.4

		//StaticVertexFmt
		BufferHandle m_staticVertexPosBuffer{};
		BufferHandle m_staticVertexShadingBuffer{};
		BufferHandle m_staticIndexBuffer{};
		VmaVirtualBlock m_staticVertexBlock = VK_NULL_HANDLE;
		VmaVirtualBlock m_staticIndexBlock = VK_NULL_HANDLE;

		// DEFAULTS
		//TextureHandle m_nilTexture{};

		VkSampler m_defaultSampler{};
		VkSampler m_defaultShadowSampler{};

		Holder<DescriptorPoolHandle> defaultDescriptorPool;
		//Holder<DescriptorPoolHandle> imguiDescriptorPool;

		mutable bool needToUpdateDescriptors = true;
		//mutable bool needToUpdateTextures = true; //maybe disambiguation is better here?


	private:
		Image CreateImageImpl(const smvk::ImageCreateInfo& imageCreateInfo, const void* data, const char* debugName);

	};

	void destroy(VulkanResourceManager* ctx, ImageHandle handle);
	void destroy(VulkanResourceManager* ctx, TextureHandle handle);
	void destroy(VulkanResourceManager* ctx, BufferHandle handle);
	void destroy(VulkanResourceManager* ctx, SamplerHandle handle);

	void destroy(VulkanResourceManager* ctx, DescriptorPoolHandle handle);
	void destroy(VulkanResourceManager* ctx, ShaderModuleHandle handle); // will be removed in 1.4

};


namespace smvk {
	class VulkanSwapchain;
	struct FrameBuffer {
		uint32_t frameIndex = 0;
		uint32_t imageIndex = 0;
		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		VulkanSwapchain& swapchain;
	};

	class VulkanSwapchain {

	public:

		VulkanSwapchain(
			VulkanResourceManager& context
			, GLFWwindow* window
		);
		VulkanSwapchain(const VulkanSwapchain&) = delete;
		VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;
		VulkanSwapchain(VulkanSwapchain&&) = delete;
		VulkanSwapchain& operator=(VulkanSwapchain&&) = delete;
		~VulkanSwapchain();

		void Create() {
			CreateSwapchain();
			CreateImageViews();
			CreateDepthResources();
			CreateSyncObjects();
		}
		void Recreate();

		VkExtent2D GetExtent() const { return extent_; }
		uint32_t GetImageCount() const { return static_cast<uint32_t>(images_.size()); }
		uint32_t GetMinImageCount() const { return minImageCount_; };
		VkImage GetImage(uint32_t index) const { return images_.at(index); }
		VkImageView GetImageView(uint32_t index) const { return imageViews_.at(index); }
		VkImage GetCurrentImage() const { return images_.at(currentImageIndex_); }
		VkImageView GetCurrentImageView() const { return imageViews_.at(currentImageIndex_); }
		uint32_t GetCurrentImageIndex() const { return currentImageIndex_; }
		uint32_t GetCurrentFrameIndex() const { return currentFrame_; }
		VkSemaphore GetImageAvailableSemaphore() const { return imageAvailableSemaphores_.at(currentFrame_); }
		VkFence GetInFlightFence() const { return inFlightFences_.at(currentFrame_); }
		VkSemaphore GetRenderFinishedSemaphore(uint32_t imageIndex) const { return renderFinishedSemaphores_.at(imageIndex); }
		// 

		void AdvanceFrame() { currentFrame_ = (currentFrame_ + 1) % framesInFlight_; }
		void SetCurrentImageIndex(uint32_t imageIndex) { currentImageIndex_ = imageIndex; }

		//private:
		VulkanResourceManager& ctx_;
		GLFWwindow* window_ = nullptr;
		VkSwapchainKHR vk_swapchain_ = VK_NULL_HANDLE;
		VkFormat vk_surfaceFormat_ = VK_FORMAT_UNDEFINED;
		VkFormat vk_depthFormat_ = VK_FORMAT_UNDEFINED;

#pragma region ===[ Frame Commands List ]===
		VkCommandPool perFrameCommandPools[MAX_FRAMES_IN_FLIGHT]{};
		VkCommandBuffer commandBuffers[MAX_FRAMES_IN_FLIGHT]{};
#pragma endregion

		bool framebufferResized = false;
		uint32_t minImageCount_;

		Holder<ImageHandle> depthImage_;
		std::vector<VkImage> images_;
		std::vector<VkImageView> imageViews_;
		std::vector<VkSemaphore> renderFinishedSemaphores_;
		std::vector<VkSemaphore> imageAvailableSemaphores_;
		std::vector<VkFence> inFlightFences_;
		std::vector<VkFence> imagesInFlight_;

		VkExtent2D extent_{};
		int windowWidth = 0;
		int windowHeight = 0;
		uint32_t framesInFlight_ = 0;
		uint32_t desiredImageCount_ = DESIRED_SWAPCHAIN_IMAGE_COUNT;
		uint32_t currentImageIndex_ = 0;
		uint32_t currentFrame_ = 0;

		void CreateSwapchain();
		void CreateImageViews();
		void CreateDepthResources();
		void CreateSyncObjects();
		void DestroySyncObjects();
		void CleanupSwapchain();

		FrameBuffer AcquireFramebuffer();
		void EndAndPresentFramebuffer(FrameBuffer& cb);
	};

	struct PipelineBundle {
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkPipelineLayout layout = VK_NULL_HANDLE;
	};

	struct PipelineKey {

		ShaderProgram& program;
		std::vector<VkVertexInputBindingDescription> bindingDescriptions{};
		std::vector<VkVertexInputAttributeDescription> attributeDescriptions{};
		std::vector<VkFormat> colorFormats;
		VkFormat depthFormat = VK_FORMAT_UNDEFINED;
		VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
		VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
		VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
		VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		bool depthTestEnable = VK_TRUE;
		bool depthWriteEnable = VK_TRUE;
		VkCompareOp depthCompareOp = VK_COMPARE_OP_GREATER;
		bool stencilTestEnable = VK_FALSE;
		bool stencilWriteEnable = VK_FALSE;
		VkCompareOp frontStencilCompareOp = VK_COMPARE_OP_ALWAYS;
		VkCompareOp backStencilCompareOp = VK_COMPARE_OP_ALWAYS;
		uint32_t frontStencilFailOp = VK_STENCIL_OP_KEEP;
		uint32_t backStencilFailOp = VK_STENCIL_OP_KEEP;
		uint32_t frontStencilPassOp = VK_STENCIL_OP_KEEP;
		uint32_t backStencilPassOp = VK_STENCIL_OP_KEEP;
		VkStencilFaceFlags frontStencilFace = VK_STENCIL_FACE_FRONT_BIT;
		VkStencilFaceFlags backStencilFace = VK_STENCIL_FACE_BACK_BIT;
		bool operator==(const PipelineKey& other) const noexcept
		{
			return program == other.program &&
				colorFormats == other.colorFormats &&
				depthFormat == other.depthFormat &&
				stencilFormat == other.stencilFormat &&
				polygonMode == other.polygonMode &&
				cullMode == other.cullMode &&
				frontFace == other.frontFace &&
				depthTestEnable == other.depthTestEnable &&
				depthWriteEnable == other.depthWriteEnable &&
				depthCompareOp == other.depthCompareOp &&
				stencilTestEnable == other.stencilTestEnable &&
				stencilWriteEnable == other.stencilWriteEnable &&
				frontStencilCompareOp == other.frontStencilCompareOp &&
				backStencilCompareOp == other.backStencilCompareOp &&
				frontStencilFailOp == other.frontStencilFailOp &&
				backStencilFailOp == other.backStencilFailOp &&
				frontStencilPassOp == other.frontStencilPassOp &&
				backStencilPassOp == other.backStencilPassOp &&
				frontStencilFace == other.frontStencilFace &&
				backStencilFace == other.backStencilFace;
		}
	};

	struct PipelineKeyHash {
		static void HashCombine(std::size_t& seed, std::size_t value) noexcept
		{
			// Standard-ish hash combine.
			seed ^= value + 0x9e3779b97f4a7c15ull +
				(seed << 6) + (seed >> 2);
		}

		template<typename T>
		static void HashValue(std::size_t& seed, const T& value) noexcept
		{
			HashCombine(seed, std::hash<T>{}(value));
		}

		std::size_t operator()(const PipelineKey& key) const noexcept
		{
			std::size_t seed = 0;

			HashValue(seed, key.program.shaderCount);

			for (size_t i = 0; i < key.program.shaderCount; ++i)
			{
				const Shader& shader = key.program.shaders[i];

				HashValue(seed, shader.vkShaderModule.index());
				HashValue(seed, static_cast<uint32_t>(shader.stage));
				HashValue(seed, std::string_view(shader.entryPoint));
			}
			HashValue(seed, key.colorFormats.size());
			for (VkFormat format : key.colorFormats) {
				HashValue(seed, static_cast<uint32_t>(format));
			}

			HashValue(seed, static_cast<uint32_t>(key.depthFormat));
			HashValue(seed, static_cast<uint32_t>(key.stencilFormat));
			HashValue(seed, static_cast<uint32_t>(key.polygonMode));
			HashValue(seed, static_cast<uint32_t>(key.cullMode));
			HashValue(seed, static_cast<uint32_t>(key.frontFace));
			HashValue(seed, static_cast<uint32_t>(key.depthTestEnable));
			HashValue(seed, static_cast<uint32_t>(key.depthWriteEnable));
			HashValue(seed, static_cast<uint32_t>(key.depthCompareOp));
			HashValue(seed, static_cast<uint32_t>(key.stencilTestEnable));
			HashValue(seed, static_cast<uint32_t>(key.stencilWriteEnable));
			HashValue(seed, static_cast<uint32_t>(key.frontStencilCompareOp));
			HashValue(seed, static_cast<uint32_t>(key.backStencilCompareOp));
			HashValue(seed, static_cast<uint32_t>(key.frontStencilFailOp));
			HashValue(seed, static_cast<uint32_t>(key.backStencilFailOp));
			HashValue(seed, static_cast<uint32_t>(key.frontStencilPassOp));
			HashValue(seed, static_cast<uint32_t>(key.backStencilPassOp));
			HashValue(seed, static_cast<uint32_t>(key.frontStencilFace));
			HashValue(seed, static_cast<uint32_t>(key.backStencilFace));

			return seed;
		}
	};

	//std::vector<VkVertexInputBindingDescription> getVkVertexInputBindingDescriptions(const VertexFormatDescription& format);
	//std::vector<VkVertexInputAttributeDescription> getVkVertexAttributeDescriptions(const VertexFormatDescription& format);
	class PipelineCache
	{
	public:
		explicit PipelineCache(VulkanResourceManager* ctx)
			: ctx_(ctx)
		{}

		~PipelineCache()
		{
			Clear();
		}

		PipelineBundle GetRenderPipeline(
			ShaderProgram& program,
			const VertexFormatDescription& vertexFormat,
			const Format* colorFormats,
			uint32_t colorAttachmentCount,
			Format depthFormat = Format::Undefined,
			Format stencilFormat = Format::Undefined,
			PrimitiveTopology topology = PrimitiveTopology::TriangleList,
			const RasterSettings& raster = {},
			const DepthSettings& depth = DepthSettings::DepthTest(),
			const StencilSettings& stencil = StencilSettings::DefaultDisabled(),
			const BlendSettings& blend = BlendSettings::Opaque(),
			const MSAASettings& msaa = MSAASettings::Count1())
		{
			PipelineKey key{
				.program = program,
				.vertexFormat = vertexFormat,
				.colorFormats = std::vector<Format>(
					colorFormats,
					colorFormats + colorAttachmentCount),
				.depthFormat = depthFormat,
				.stencilFormat = stencilFormat,
				.topology = topology,
				.raster = raster,
				.depth = depth,
				.stencil = stencil,
				.blend = blend,
				.msaa = msaa,
			};

			if (auto it = pipelines_.find(key);
				it != pipelines_.end())
			{
				return it->second;
			}

			PipelineBundle bundle =
				BuildGraphicsPipeline(key);

			auto [it, inserted] =
				pipelines_.emplace(
					std::move(key),
					bundle);

			return it->second;
		}

		void Clear()
		{
			for (auto& [key, bundle] : pipelines_)
			{
				if (bundle.pipeline != VK_NULL_HANDLE)
				{
					ctx_->core_.DestroyPipeline(
						bundle.pipeline);
				}
			}

			pipelines_.clear();
		}

	private:
		PipelineBundle BuildGraphicsPipeline(
			const PipelineKey& key);

	private:
		VulkanResourceManager* ctx_ = nullptr;

		std::unordered_map<
			PipelineKey,
			PipelineBundle,
			PipelineKeyHash
		> pipelines_;
	};

} // namespace smvk

