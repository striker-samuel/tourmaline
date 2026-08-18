//#pragma once
//
//#include <cstdint>
//#include <cstddef>
//#include <vector>
//#include <optional>
//#include <tuple>
//#include <cstring>
//#include <type_traits>
//
//#include "Engine/Shared/common.h"
//#include <Engine/Memory/pool.h>
//
//#define ENGINE_NAME "smvk"
//
//namespace smvk {
//
//	// ============================================================================
//	// Common helpers
//	// ============================================================================
//
//	template<typename Enum>
//	constexpr auto to_underlying(Enum value) noexcept
//	{
//		return static_cast<std::underlying_type_t<Enum>>(value);
//	}
//
//	template<typename Enum>
//	constexpr Enum operator|(Enum lhs, Enum rhs) noexcept
//		requires std::is_enum_v<Enum>
//	{
//		return static_cast<Enum>(
//			to_underlying(lhs) | to_underlying(rhs)
//			);
//	}
//
//	template<typename Enum>
//	constexpr Enum operator&(Enum lhs, Enum rhs) noexcept
//		requires std::is_enum_v<Enum>
//	{
//		return static_cast<Enum>(
//			to_underlying(lhs) & to_underlying(rhs)
//			);
//	}
//
//	template<typename Enum>
//	constexpr Enum& operator|=(Enum& lhs, Enum rhs) noexcept
//		requires std::is_enum_v<Enum>
//	{
//		lhs = lhs | rhs;
//		return lhs;
//	}
//
//	template<typename Enum>
//	constexpr bool has_flag(Enum value, Enum flag) noexcept
//	{
//		return (to_underlying(value) & to_underlying(flag)) != 0;
//	}
//
//
//	// ============================================================================
//	// Basic geometry / resource dimensions
//	// ============================================================================
//
//	struct Extent2D
//	{
//		uint32_t width = 1;
//		uint32_t height = 1;
//	};
//
//	struct Extent3D
//	{
//		uint32_t width = 1;
//		uint32_t height = 1;
//		uint32_t depth = 1;
//	};
//
//	struct Offset3D
//	{
//		int32_t x = 0;
//		int32_t y = 0;
//		int32_t z = 0;
//	};
//
//
//	// ============================================================================
//	// Formats
//	// ============================================================================
//
//	enum class Format : uint16_t
//	{
//		Undefined,
//
//		// 8-bit
//		R8_UNorm,
//		R8_SNorm,
//		R8_UInt,
//		R8_SInt,
//
//		RG8_UNorm,
//		RG8_SNorm,
//		RG8_UInt,
//		RG8_SInt,
//
//		RGBA8_UNorm,
//		RGBA8_SNorm,
//		RGBA8_UInt,
//		RGBA8_SInt,
//		RGBA8_SRGB,
//
//		BGRA8_UNorm,
//		BGRA8_SRGB,
//
//		// 16-bit
//		R16_UNorm,
//		R16_SNorm,
//		R16_UInt,
//		R16_SInt,
//		R16_Float,
//
//		RG16_UNorm,
//		RG16_SNorm,
//		RG16_UInt,
//		RG16_SInt,
//		RG16_Float,
//
//		RGBA16_UNorm,
//		RGBA16_SNorm,
//		RGBA16_UInt,
//		RGBA16_SInt,
//		RGBA16_Float,
//
//		// 32-bit
//		R32_UInt,
//		R32_SInt,
//		R32_Float,
//
//		RG32_UInt,
//		RG32_SInt,
//		RG32_Float,
//
//		RGB32_UInt,
//		RGB32_SInt,
//		RGB32_Float,
//
//		RGBA32_UInt,
//		RGBA32_SInt,
//		RGBA32_Float,
//
//		// packed
//		RGB10A2_UNorm,
//		RGB10A2_UInt,
//
//		R11G11B10_Float,
//
//		// depth / stencil
//		D16_UNorm,
//		D24_UNorm_S8_UInt,
//		D32_Float,
//		D32_Float_S8_UInt,
//
//		// compressed
//		BC1_RGBA_UNorm,
//		BC1_RGBA_SRGB,
//		BC2_UNorm,
//		BC2_SRGB,
//		BC3_UNorm,
//		BC3_SRGB,
//		BC4_UNorm,
//		BC4_SNorm,
//		BC5_UNorm,
//		BC5_SNorm,
//		BC6H_UFloat,
//		BC6H_SFloat,
//		BC7_UNorm,
//		BC7_SRGB,
//	};
//
//
//	// ============================================================================
//	// Primitive / vertex input
//	// ============================================================================
//
//	enum class PrimitiveTopology : uint8_t
//	{
//		PointList,
//
//		LineList,
//		LineStrip,
//
//		TriangleList,
//		TriangleStrip,
//		TriangleFan,
//	};
//
//	enum class VertexInputRate : uint8_t
//	{
//		Vertex,
//		Instance,
//	};
//
//	enum class VertexSemantic : uint8_t
//	{
//		Position,
//		Normal,
//		Tangent,
//		Color,
//
//		TexCoord0,
//		TexCoord1,
//
//		JointIndices,
//		JointWeights,
//	};
//
//	struct VertexAttribute
//	{
//		VertexSemantic semantic = VertexSemantic::Position;
//		Format format = Format::Undefined;
//		uint32_t offset = 0;
//	};
//
//	struct VertexBinding
//	{
//		uint32_t binding = 0;
//		uint32_t stride = 0;
//
//		VertexInputRate inputRate = VertexInputRate::Vertex;
//
//		std::vector<VertexAttribute> attributes;
//	};
//
//	struct VertexFormat
//	{
//		std::vector<VertexBinding> bindings;
//	};
//
//
//	// ============================================================================
//	// Sample counts
//	// ============================================================================
//
//	enum class SampleCount : uint8_t
//	{
//		Count1 = 1,
//		Count2 = 2,
//		Count4 = 4,
//		Count8 = 8,
//		Count16 = 16,
//		Count32 = 32,
//		Count64 = 64,
//	};
//
//
//	// ============================================================================
//	// Buffer
//	// ============================================================================
//
//	enum class BufferUsage : uint32_t
//	{
//		None = 0,
//
//		TransferSrc = 1u << 0,
//		TransferDst = 1u << 1,
//
//		Index = 1u << 2,
//		Vertex = 1u << 3,
//		Uniform = 1u << 4,
//		Storage = 1u << 5,
//		Indirect = 1u << 6,
//
//		// Keep room here for future RT support:
//		// ShaderBindingTable
//		// AccelerationStructureInput
//		// AccelerationStructureStorage
//	};
//
//	enum class MemoryUsage : uint8_t
//	{
//		Auto,
//
//		DeviceLocal,
//		Upload,
//		Readback,
//	};
//
//	struct BufferCreateInfo
//	{
//		size_t size = 0;
//
//		BufferUsage usage = BufferUsage::None;
//		MemoryUsage memoryUsage = MemoryUsage::Auto;
//
//		const char* name = nullptr;
//	};
//
//
//	// ============================================================================
//	// Image
//	// ============================================================================
//
//	enum class ImageType : uint8_t
//	{
//		Image1D,
//		Image2D,
//		Image3D,
//	};
//
//	enum class ImageUsage : uint32_t
//	{
//		None = 0,
//
//		TransferSrc = 1u << 0,
//		TransferDst = 1u << 1,
//
//		Sampled = 1u << 2,
//		Storage = 1u << 3,
//
//		ColorAttachment = 1u << 4,
//		DepthStencilAttachment = 1u << 5,
//
//		InputAttachment = 1u << 6,
//	};
//
//	enum class ImageViewType : uint8_t
//	{
//		View1D,
//		View2D,
//		View3D,
//
//		ViewCube,
//
//		View1DArray,
//		View2DArray,
//		ViewCubeArray,
//	};
//
//	enum class ImageAspect : uint8_t
//	{
//		None = 0,
//		Color = 1u << 0,
//		Depth = 1u << 1,
//		Stencil = 1u << 2,
//	};
//
//	struct ImageSubresourceRange
//	{
//		ImageAspect aspect = ImageAspect::Color;
//
//		uint32_t baseMipLevel = 0;
//		uint32_t mipLevelCount = 1;
//
//		uint32_t baseArrayLayer = 0;
//		uint32_t arrayLayerCount = 1;
//	};
//
//	struct ImageCreateInfo
//	{
//		ImageType type = ImageType::Image2D;
//
//		Extent3D extent{};
//
//		Format format = Format::Undefined;
//
//		ImageUsage usage =
//			ImageUsage::Sampled |
//			ImageUsage::TransferDst;
//
//		SampleCount samples = SampleCount::Count1;
//
//		uint32_t mipLevels = 1;
//		uint32_t arrayLayers = 1;
//
//		const char* name = nullptr;
//	};
//
//	using TextureCreateInfo = ImageCreateInfo;
//
//	struct ImageViewCreateInfo
//	{
//		ImageViewType viewType = ImageViewType::View2D;
//
//		// Undefined = inherit underlying image format.
//		Format format = Format::Undefined;
//
//		ImageSubresourceRange subresourceRange{};
//	};
//
//
//	// ============================================================================
//	// Samplers
//	// ============================================================================
//
//	enum class Filter : uint8_t
//	{
//		Nearest,
//		Linear,
//	};
//
//	enum class MipmapMode : uint8_t
//	{
//		Nearest,
//		Linear,
//	};
//
//	enum class AddressMode : uint8_t
//	{
//		Repeat,
//		MirroredRepeat,
//		ClampToEdge,
//		ClampToBorder,
//		MirrorClampToEdge,
//	};
//
//	enum class CompareOp : uint8_t
//	{
//		Never,
//		Less,
//		Equal,
//		LessEqual,
//		Greater,
//		NotEqual,
//		GreaterEqual,
//		Always,
//	};
//
//	struct SamplerCreateInfo
//	{
//		Filter magFilter = Filter::Linear;
//		Filter minFilter = Filter::Linear;
//
//		MipmapMode mipmapMode = MipmapMode::Linear;
//
//		AddressMode addressU = AddressMode::Repeat;
//		AddressMode addressV = AddressMode::Repeat;
//		AddressMode addressW = AddressMode::Repeat;
//
//		float mipLodBias = 0.0f;
//
//		float minLod = 0.0f;
//		float maxLod = 1000.0f;
//
//		float maxAnisotropy = 1.0f;
//
//		CompareOp compareOp = CompareOp::Always;
//		bool compareEnable = false;
//
//		bool operator<(const SamplerCreateInfo& other) const noexcept
//		{
//			return std::tie(
//				magFilter,
//				minFilter,
//				mipmapMode,
//
//				addressU,
//				addressV,
//				addressW,
//
//				mipLodBias,
//				minLod,
//				maxLod,
//				maxAnisotropy,
//
//				compareOp,
//				compareEnable
//			) <
//				std::tie(
//					other.magFilter,
//					other.minFilter,
//					other.mipmapMode,
//
//					other.addressU,
//					other.addressV,
//					other.addressW,
//
//					other.mipLodBias,
//					other.minLod,
//					other.maxLod,
//					other.maxAnisotropy,
//
//					other.compareOp,
//					other.compareEnable
//				);
//		}
//	};
//
//
//	// ============================================================================
//	// Shader
//	// ============================================================================
//
//	enum class ShaderStage : uint32_t
//	{
//		None = 0,
//
//		Vertex = 1u << 0,
//		TessControl = 1u << 1,
//		TessEvaluation = 1u << 2,
//		Geometry = 1u << 3,
//		Fragment = 1u << 4,
//		Compute = 1u << 5,
//
//		// Future:
//		// Task
//		// Mesh
//		// RayGen
//		// AnyHit
//		// ClosestHit
//		// Miss
//		// Intersection
//		// Callable
//	};
//
//	using ShaderStageFlags = ShaderStage;
//
//
//	// ============================================================================
//	// Resource handles
//	// ============================================================================
//
//	template<typename T>
//	struct Handle;
//
//	using ImageHandle = Handle<struct Image>;
//	using TextureHandle = Handle<struct Texture>;
//	using SamplerHandle = Handle<struct Sampler>;
//	using BufferHandle = Handle<struct Buffer>;
//	using ShaderModuleHandle = Handle<struct ShaderModule>;
//	using DescriptorPoolHandle = Handle<struct DescriptorPool>;
//
//
//	// ============================================================================
//	// Shader program
//	// ============================================================================
//
//	struct Shader
//	{
//		ShaderModuleHandle module;
//
//		const char* entryPoint = "main";
//		ShaderStage stage = ShaderStage::None;
//
//		bool operator==(const Shader& other) const noexcept
//		{
//			return std::strcmp(entryPoint, other.entryPoint) == 0 &&
//				stage == other.stage &&
//				module == other.module;
//		}
//	};
//
//	struct ShaderProgram
//	{
//		Shader shaders[8]{};
//		size_t shaderCount = 0;
//
//		bool operator==(const ShaderProgram& other) const noexcept
//		{
//			if (shaderCount != other.shaderCount)
//				return false;
//
//			for (size_t i = 0; i < shaderCount; ++i)
//			{
//				if (!(shaders[i] == other.shaders[i]))
//					return false;
//			}
//
//			return true;
//		}
//	};
//
//
//	// ============================================================================
//	// Descriptor/bindless slots
//	// ============================================================================
//
//	enum class BindingType : uint8_t
//	{
//		Textures,
//		Samplers,
//		StorageImages,
//
//		Count
//	};
//
//
//	// ============================================================================
//	// Queue information
//	//
//	// I'd keep this generic rather than calling them "Vk queue families".
//	// ============================================================================
//
//	struct QueueFamilyIndices
//	{
//		std::optional<uint32_t> graphics;
//		std::optional<uint32_t> present;
//		std::optional<uint32_t> transfer;
//		std::optional<uint32_t> compute;
//
//		[[nodiscard]]
//		bool isComplete() const noexcept
//		{
//			return graphics.has_value() &&
//				present.has_value();
//		}
//	};
//
//} // namespace rhi