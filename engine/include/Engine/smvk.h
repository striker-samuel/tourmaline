#pragma once

#include <cassert>
#include <array>
#include <tuple>
#include <map>
#include <future>
#include <algorithm>
#include <optional>
#include <vector>
#include <set>
//
#include "volk.h"
#include "vk_mem_alloc.h"
//
#include "Engine/Shared/common.h"
#include <Engine/Memory/pool.h>

#define ENGINE_NAME "smvk"

#include <ktx.h>
#include <ktxVulkan.h>
//
static constexpr uint32_t DEFAULT_WIDTH = 1280;
static constexpr uint32_t DEFAULT_HEIGHT = 720;
static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
static constexpr uint32_t DESIRED_SWAPCHAIN_IMAGE_COUNT = 2;
static constexpr uint32_t MAX_BUFFERS = 1024;
static constexpr uint32_t MAX_LIGHTS = 64;
static constexpr uint32_t MAX_TEXTURES = 4096;
static constexpr uint32_t MAX_SAMPLERS = 8;
static constexpr uint32_t MAX_MATERIALS = 1024;
static constexpr uint32_t MAX_SHADOW_SAMPLERS = 8;
static constexpr uint32_t MAX_COLOR_ATTACHEMNTS = 4096;

inline const char* vkResultToString(VkResult result);

#define VK_ASSERT_RETURN(func)                                     \
  {                                                                \
    const VkResult vk_assert_result = func;                        \
    if (vk_assert_result != VK_SUCCESS) {                          \
      LLOGW("Vulkan API call failed: %s:%i\n  %s\n  %s\n", \
                    __FILE__,                                      \
                    __LINE__,                                      \
                    #func,                                         \
                    lvk::getVulkanResultString(vk_assert_result)); \
      assert(false);                                               \
      return getResultFromVkResult(vk_assert_result);              \
    }                                                              \
  }

#define VK_CHECK(x)                                                     \
    do {                                                                \
        VkResult err = (x);                                             \
        if (err != VK_SUCCESS) {                                        \
            fprintf(stderr, "[Vulkan Error] %s:%d: %s\n",               \
                    __FILE__, __LINE__, vkResultToString(err));         \
            abort();                                                    \
        }                                                               \
    } while (0)

namespace smvk {

	/*template<typename Enum>
	constexpr auto to_underlying(Enum value) noexcept
	{
		return static_cast<std::underlying_type_t<Enum>>(value);
	}

	template<typename Enum>
	constexpr Enum operator|(Enum lhs, Enum rhs) noexcept
		requires std::is_enum_v<Enum>
	{
		return static_cast<Enum>(
			to_underlying(lhs) | to_underlying(rhs)
			);
	}

	template<typename Enum>
	constexpr Enum operator&(Enum lhs, Enum rhs) noexcept
		requires std::is_enum_v<Enum>
	{
		return static_cast<Enum>(
			to_underlying(lhs) & to_underlying(rhs)
			);
	}

	template<typename Enum>
	constexpr Enum& operator|=(Enum& lhs, Enum rhs) noexcept
		requires std::is_enum_v<Enum>
	{
		lhs = lhs | rhs;
		return lhs;
	}

	template<typename Enum>
	constexpr bool has_flag(Enum value, Enum flag) noexcept
	{
		return (to_underlying(value) & to_underlying(flag)) != 0;
	}*/

	// ============================================================================
	// Basic geometry / resource dimensions
	// ============================================================================

	struct Extent2D
	{
		uint32_t width = 1;
		uint32_t height = 1;
	};

	struct Extent3D
	{
		uint32_t width = 1;
		uint32_t height = 1;
		uint32_t depth = 1;
	};

	struct Offset3D
	{
		int32_t x = 0;
		int32_t y = 0;
		int32_t z = 0;
	};

	// ============================================================================
	// Formats
	// ============================================================================

	enum class Format : uint16_t
	{
		Undefined,

		// 8-bit
		R8_UNorm,
		R8_SNorm,
		R8_UInt,
		R8_SInt,

		RG8_UNorm,
		RG8_SNorm,
		RG8_UInt,
		RG8_SInt,

		RGBA8_UNorm,
		RGBA8_SNorm,
		RGBA8_UInt,
		RGBA8_SInt,
		RGBA8_SRGB,

		BGRA8_UNorm,
		BGRA8_SRGB,

		// 16-bit
		R16_UNorm,
		R16_SNorm,
		R16_UInt,
		R16_SInt,
		R16_Float,

		RG16_UNorm,
		RG16_SNorm,
		RG16_UInt,
		RG16_SInt,
		RG16_Float,

		RGBA16_UNorm,
		RGBA16_SNorm,
		RGBA16_UInt,
		RGBA16_SInt,
		RGBA16_Float,

		// 32-bit
		R32_UInt,
		R32_SInt,
		R32_Float,

		RG32_UInt,
		RG32_SInt,
		RG32_Float,

		RGB32_UInt,
		RGB32_SInt,
		RGB32_Float,

		RGBA32_UInt,
		RGBA32_SInt,
		RGBA32_Float,

		// packed
		RGB10A2_UNorm,
		RGB10A2_UInt,

		R11G11B10_Float,

		// depth / stencil
		D16_UNorm,
		D24_UNorm_S8_UInt,
		D32_Float,
		D32_Float_S8_UInt,

		// compressed
		BC1_RGBA_UNorm,
		BC1_RGBA_SRGB,
		BC2_UNorm,
		BC2_SRGB,
		BC3_UNorm,
		BC3_SRGB,
		BC4_UNorm,
		BC4_SNorm,
		BC5_UNorm,
		BC5_SNorm,
		BC6H_UFloat,
		BC6H_SFloat,
		BC7_UNorm,
		BC7_SRGB,
	};

	// ============================================================================
	// Primitive / vertex input
	// ============================================================================

	enum class PrimitiveTopology : uint8_t
	{
		PointList,

		LineList,
		LineStrip,

		TriangleList,
		TriangleStrip,
		TriangleFan,
	};

	enum class VertexInputRate : uint8_t
	{
		Vertex,
		Instance,
	};

	enum class VertexSemantic : uint8_t
	{
		Position,
		Normal,
		Tangent,
		Color,

		TexCoord0,
		TexCoord1,

		JointIndices,
		JointWeights,
	};

	struct VertexAttribute
	{
		VertexSemantic semantic = VertexSemantic::Position;
		Format format = Format::Undefined;
		uint32_t offset = 0;
	};

	struct VertexBinding
	{
		uint32_t binding = 0;
		uint32_t stride = 0;

		VertexInputRate inputRate = VertexInputRate::Vertex;

		std::vector<VertexAttribute> attributes;
	};

	struct VertexFormatDescription
	{
		std::vector<VertexBinding> bindings;
	};

	// ============================================================================
	// Sample counts
	// ============================================================================

	enum class SampleCount : uint8_t
	{
		Count1 = 1,
		Count2 = 2,
		Count4 = 4,
		Count8 = 8,
		Count16 = 16,
		Count32 = 32,
		Count64 = 64,
	};

	struct MSAASettings {
		SampleCount samples = SampleCount::Count1;

		static MSAASettings Count1() { return { SampleCount::Count1 }; }
		static MSAASettings Count2() { return { SampleCount::Count2 }; }
		static MSAASettings Count4() { return { SampleCount::Count4 }; }
		static MSAASettings Count8() { return { SampleCount::Count8 }; }
		static MSAASettings Count16() { return { SampleCount::Count16 }; }
		static MSAASettings Count32() { return { SampleCount::Count32 }; }
		static MSAASettings Count64() { return { SampleCount::Count64 }; }
	};

	// ============================================================================
	// Buffer
	// ============================================================================

	enum class BufferUsage : uint32_t
	{
		None = 0,

		TransferSrc = 1u << 0,
		TransferDst = 1u << 1,

		Index = 1u << 2,
		Vertex = 1u << 3,
		Uniform = 1u << 4,
		Storage = 1u << 5,
		Indirect = 1u << 6,

		// Keep room here for future RT support:
		// ShaderBindingTable
		// AccelerationStructureInput
		// AccelerationStructureStorage
	};

	enum class MemoryUsage : uint8_t
	{
		Auto,

		DeviceLocal,
		Upload,
		Readback,
	};

	//struct BufferCreateInfo
	//{
	//	size_t size = 0;
	//	BufferUsage usage = BufferUsage::None;
	//	MemoryUsage memoryUsage = MemoryUsage::Auto;
	//	const char* name = nullptr;
	//};

	// ============================================================================
	// Image
	// ============================================================================

	enum class ImageType : uint8_t
	{
		Image1D,
		Image2D,
		Image3D,
	};

	enum class ImageUsage : uint32_t
	{
		None = 0,

		TransferSrc = 1u << 0,
		TransferDst = 1u << 1,

		Sampled = 1u << 2,
		Storage = 1u << 3,

		ColorAttachment = 1u << 4,
		DepthStencilAttachment = 1u << 5,

		InputAttachment = 1u << 6,
	};

	enum class ImageViewType : uint8_t
	{
		View1D,
		View2D,
		View3D,

		ViewCube,

		View1DArray,
		View2DArray,
		ViewCubeArray,
	};

	enum class ImageAspect : uint8_t
	{
		None = 0,
		Color = 1u << 0,
		Depth = 1u << 1,
		Stencil = 1u << 2,
	};

	struct ImageSubresourceRange
	{
		ImageAspect aspect = ImageAspect::Color;

		uint32_t baseMipLevel = 0;
		uint32_t mipLevelCount = 1;

		uint32_t baseArrayLayer = 0;
		uint32_t arrayLayerCount = 1;
	};

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
	enum class Filter : uint8_t
	{
		Nearest,
		Linear,
	};

	enum class MipmapMode : uint8_t
	{
		Nearest,
		Linear,
	};

	enum class AddressMode : uint8_t
	{
		Repeat,
		MirroredRepeat,
		ClampToEdge

	};

	enum class CompareOp : uint8_t
	{
		Never,
		Less,
		Equal,
		LessEqual,
		Greater,
		NotEqual,
		GreaterEqual,
		Always,
	};

	/*	struct SamplerCreateInfo
		{
			Filter magFilter = Filter::Linear;
			Filter minFilter = Filter::Linear;

			MipmapMode mipmapMode = MipmapMode::Linear;

			AddressMode addressU = AddressMode::Repeat;
			AddressMode addressV = AddressMode::Repeat;
			AddressMode addressW = AddressMode::Repeat;

			float mipLodBias = 0.0f;

			float minLod = 0.0f;
			float maxLod = 1000.0f;

			float maxAnisotropy = 1.0f;

			CompareOp compareOp = CompareOp::Always;
			bool compareEnable = false;

			bool operator<(const SamplerCreateInfo& other) const noexcept
			{
				return std::tie(
					magFilter,
					minFilter,
					mipmapMode,

					addressU,
					addressV,
					addressW,

					mipLodBias,
					minLod,
					maxLod,
					maxAnisotropy,

					compareOp,
					compareEnable
				) <
					std::tie(
						other.magFilter,
						other.minFilter,
						other.mipmapMode,

						other.addressU,
						other.addressV,
						other.addressW,

						other.mipLodBias,
						other.minLod,
						other.maxLod,
						other.maxAnisotropy,

						other.compareOp,
						other.compareEnable
					);
			}
		};*/

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


	// ============================================================================
	// Stencil
	// ============================================================================

	enum StencilOp : uint8_t
	{
		STENCIL_OP_KEEP = 0,
		STENCIL_OP_ZERO,
		STENCIL_OP_REPLACE,
		STENCIL_OP_INCREMENT_AND_CLAMP,
		STENCIL_OP_DECREMENT_AND_CLAMP,
		STENCIL_OP_INVERT,
		STENCIL_OP_INCREMENT_AND_WRAP,
		STENCIL_OP_DECREMENT_AND_WRAP,
	};

	struct StencilSettings {
		CompareOp frontCompareOp = CompareOp::Always;
		CompareOp backCompareOp = CompareOp::Always;
		uint8_t frontFailOp = STENCIL_OP_KEEP;
		uint8_t backFailOp = STENCIL_OP_KEEP;
		uint8_t frontPassOp = STENCIL_OP_KEEP;
		uint8_t backPassOp = STENCIL_OP_KEEP;
		VkStencilFaceFlags frontFace = VK_STENCIL_FACE_FRONT_BIT;
		VkStencilFaceFlags backFace = VK_STENCIL_FACE_BACK_BIT;
		bool frontTestEnable = VK_FALSE;
		bool backTestEnable = VK_FALSE;
		bool frontWriteEnable = VK_FALSE;
		bool backWriteEnable = VK_FALSE;

		// Static helpers
		static StencilSettings DefaultDisabled();
		static StencilSettings DefaultEnabled();
		static StencilSettings TwoSided(CompareOp frontCompareOp, CompareOp backCompareOp, ...);

		// Comparison operators for caching
		bool operator<(const StencilSettings& other) const noexcept;
	};


	// ============================================================================
	// Depth settings for pipeline state
	// ============================================================================

	struct DepthSettings {
		CompareOp depthFunc = CompareOp::Less;
		bool writeEnable = true;
		bool depthTestEnable = true;
		bool depthBiasEnable = false;
		float depthBiasConstantFactor = 0.0f;
		float depthBiasClamp = 0.0f;
		float depthBiasSlopeFactor = 0.0f;

		static DepthSettings DepthTest()
		{
			DepthSettings s;
			s.depthFunc = CompareOp::Less;
			s.writeEnable = true;
			s.depthTestEnable = true;
			s.depthBiasEnable = false;
			return s;
		}

		static DepthSettings Disabled()
		{
			DepthSettings s;
			s.depthFunc = CompareOp::Always;
			s.writeEnable = false;
			s.depthTestEnable = true;
			s.depthBiasEnable = false;
			return s;
		}

		static DepthSettings Always()
		{
			DepthSettings s;
			s.depthFunc = CompareOp::Always;
			s.writeEnable = true;
			s.depthTestEnable = true;
			s.depthBiasEnable = false;
			return s;
		}

		static DepthSettings WithSlopeBias(
			CompareOp depthFunc,
			bool writeEnable,
			bool depthTestEnable = true,
			float slopeFactor = 1.0f,
			float constantFactor = 0.0f,
			float clamp = 0.0f
		)
		{
			DepthSettings s;
			s.depthFunc = depthFunc;
			s.writeEnable = writeEnable;
			s.depthTestEnable = depthTestEnable;
			s.depthBiasEnable = true;
			s.depthBiasSlopeFactor = slopeFactor;
			s.depthBiasConstantFactor = constantFactor;
			s.depthBiasClamp = clamp;
			return s;
		}

		bool operator<(const DepthSettings& other) const noexcept
		{
			return std::tie(depthFunc, writeEnable, depthTestEnable,
				depthBiasEnable, depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor) <
				std::tie(other.depthFunc, other.writeEnable, other.depthTestEnable,
					other.depthBiasEnable, other.depthBiasConstantFactor, other.depthBiasClamp, other.depthBiasSlopeFactor);
		}
	};

	// ============================================================================
	// Blend settings for pipeline state
	// ============================================================================

	// Abstract blend state description - defined in RHI layer, translated to Vk types
	// in impl.h when creating pipelines.
	struct BlendAttachmentState {
		bool blendEnable = false;
		// Color blend factors
		enum class SrcColorBlendFactor : uint8_t {
			Zero = 0,
			One = 1,
			SrcAlpha = 2,
			OneMinusSrcAlpha = 3,
			// ... other factors as needed
		};
		enum class DstColorBlendFactor : uint8_t {
			Zero = 0,
			One = 1,
			OneMinusSrcAlpha = 2,
			// ... other factors as needed
		};
		// Color blend operation
		enum class ColorBlendOp : uint8_t {
			Add = 0,
			Subtract = 1,
			ReverseSubtract = 2,
			Min = 3,
			Max = 4,
		};
		// Alpha blend operation
		enum class AlphaBlendOp : uint8_t {
			Add = 0,
			Subtract = 1,
			ReverseSubtract = 2,
			Min = 3,
			Max = 4,
		};
		// Color write mask bits
		enum class ColorComponentFlag : uint8_t {
			R = 1u << 0,
			G = 1u << 1,
			B = 1u << 2,
			A = 1u << 3,
		};

		SrcColorBlendFactor srcColorBlendFactor = SrcColorBlendFactor::One;
		DstColorBlendFactor dstColorBlendFactor = DstColorBlendFactor::One;
		ColorBlendOp colorBlendOp = ColorBlendOp::Add;
		AlphaBlendOp alphaBlendOp = AlphaBlendOp::Add;
		uint8_t colorWriteMask =
			static_cast<uint8_t>(ColorComponentFlag::R) |
			static_cast<uint8_t>(ColorComponentFlag::G) |
			static_cast<uint8_t>(ColorComponentFlag::B) |
			static_cast<uint8_t>(ColorComponentFlag::A);
	};

	struct BlendState {
		bool blendEnable = false;
		BlendAttachmentState attachment[4]{}; // max 4 color attachments
	};

	struct BlendSettings {
		BlendState blendState{};

		static BlendSettings Opaque()
		{
			BlendSettings s;
			s.blendState.blendEnable = false;
			s.blendState.attachment[0].srcColorBlendFactor = BlendAttachmentState::SrcColorBlendFactor::One;
			s.blendState.attachment[0].dstColorBlendFactor = BlendAttachmentState::DstColorBlendFactor::One;
			s.blendState.attachment[0].colorBlendOp = BlendAttachmentState::ColorBlendOp::Add;
			s.blendState.attachment[0].alphaBlendOp = BlendAttachmentState::AlphaBlendOp::Add;
			s.blendState.attachment[0].colorWriteMask =
				static_cast<uint8_t>(BlendAttachmentState::ColorComponentFlag::R) |
				static_cast<uint8_t>(BlendAttachmentState::ColorComponentFlag::G) |
				static_cast<uint8_t>(BlendAttachmentState::ColorComponentFlag::B) |
				static_cast<uint8_t>(BlendAttachmentState::ColorComponentFlag::A);
			return s;
		}

		static BlendSettings Add()
		{
			BlendSettings s;
			s.blendState.blendEnable = true;
			s.blendState.attachment[0].srcColorBlendFactor = BlendAttachmentState::SrcColorBlendFactor::One;
			s.blendState.attachment[0].dstColorBlendFactor = BlendAttachmentState::DstColorBlendFactor::One;
			s.blendState.attachment[0].colorBlendOp = BlendAttachmentState::ColorBlendOp::Add;
			s.blendState.attachment[0].alphaBlendOp = BlendAttachmentState::AlphaBlendOp::Add;
			s.blendState.attachment[0].colorWriteMask =
				static_cast<uint8_t>(BlendAttachmentState::ColorComponentFlag::R) |
				static_cast<uint8_t>(BlendAttachmentState::ColorComponentFlag::G) |
				static_cast<uint8_t>(BlendAttachmentState::ColorComponentFlag::B) |
				static_cast<uint8_t>(BlendAttachmentState::ColorComponentFlag::A);
			return s;
		}

		static BlendSettings Mixed()
		{
			BlendSettings s;
			s.blendState.blendEnable = true;
			s.blendState.attachment[0].srcColorBlendFactor = BlendAttachmentState::SrcColorBlendFactor::One;
			s.blendState.attachment[0].dstColorBlendFactor = BlendAttachmentState::DstColorBlendFactor::OneMinusSrcAlpha;
			s.blendState.attachment[0].colorBlendOp = BlendAttachmentState::ColorBlendOp::Add;
			s.blendState.attachment[0].alphaBlendOp = BlendAttachmentState::AlphaBlendOp::Add;
			s.blendState.attachment[0].colorWriteMask =
				static_cast<uint8_t>(BlendAttachmentState::ColorComponentFlag::R) |
				static_cast<uint8_t>(BlendAttachmentState::ColorComponentFlag::G) |
				static_cast<uint8_t>(BlendAttachmentState::ColorComponentFlag::B) |
				static_cast<uint8_t>(BlendAttachmentState::ColorComponentFlag::A);
			return s;
		}

		static BlendSettings AlphaBlend()
		{
			BlendSettings s;
			s.blendState.blendEnable = true;
			s.blendState.attachment[0].srcColorBlendFactor = BlendAttachmentState::SrcColorBlendFactor::SrcAlpha;
			s.blendState.attachment[0].dstColorBlendFactor = BlendAttachmentState::DstColorBlendFactor::OneMinusSrcAlpha;
			s.blendState.attachment[0].colorBlendOp = BlendAttachmentState::ColorBlendOp::Add;
			s.blendState.attachment[0].alphaBlendOp = BlendAttachmentState::AlphaBlendOp::Add;
			s.blendState.attachment[0].colorWriteMask =
				static_cast<uint8_t>(BlendAttachmentState::ColorComponentFlag::R) |
				static_cast<uint8_t>(BlendAttachmentState::ColorComponentFlag::G) |
				static_cast<uint8_t>(BlendAttachmentState::ColorComponentFlag::B) |
				static_cast<uint8_t>(BlendAttachmentState::ColorComponentFlag::A);
			return s;
		}

		bool operator<(const BlendSettings& other) const noexcept
		{
			return std::tie(blendState.blendEnable,
				blendState.attachment[0].alphaBlendOp,
				blendState.attachment[0].colorBlendOp,
				blendState.attachment[0].srcColorBlendFactor,
				blendState.attachment[0].dstColorBlendFactor) <
				std::tie(other.blendState.blendEnable,
					other.blendState.attachment[0].alphaBlendOp,
					other.blendState.attachment[0].colorBlendOp,
					other.blendState.attachment[0].srcColorBlendFactor,
					other.blendState.attachment[0].dstColorBlendFactor);
		}
	};

	typedef uint64_t BufferDeviceAddress;

	typedef enum PipelineStageBits {
		PipelineStageBit_None = 0,

		PipelineStageBit_Vertex = 1u << 0,
		PipelineStageBit_Fragment = 1u << 1,
		PipelineStageBit_Compute = 1u << 2,
		PipelineStageBit_Geometry = 1u << 3,
		PipelineStageBit_TessellationControl = 1u << 4,
		PipelineStageBit_TessellationEvaluation = 1u << 5,
		PipelineStageBit_Task = 1u << 6,
		PipelineStageBit_Mesh = 1u << 7,

		// Ray tracing stages
		PipelineStageBit_RayGeneration = 1u << 8,
		PipelineStageBit_AnyHit = 1u << 9,
		PipelineStageBit_ClosestHit = 1u << 10,
		PipelineStageBit_Miss = 1u << 11,
		PipelineStageBit_Intersection = 1u << 12,
		PipelineStageBit_Callable = 1u << 13,

		// Convenience masks
		PipelineStageBit_AllGraphics =
		PipelineStageBit_Vertex |
		PipelineStageBit_Fragment |
		PipelineStageBit_Geometry |
		PipelineStageBit_TessellationControl |
		PipelineStageBit_TessellationEvaluation |
		PipelineStageBit_Task |
		PipelineStageBit_Mesh,

		PipelineStageBit_AllRayTracing =
		PipelineStageBit_RayGeneration |
		PipelineStageBit_AnyHit |
		PipelineStageBit_ClosestHit |
		PipelineStageBit_Miss |
		PipelineStageBit_Intersection |
		PipelineStageBit_Callable,

		PipelineStageBit_All =
		PipelineStageBit_AllGraphics |
		PipelineStageBit_Compute |
		PipelineStageBit_AllRayTracing,

	} PipelineStageBits;
	typedef uint32_t PipelineStage;

	enum Bindings {
		kBinding_Textures = 0, // position 0
		kBinding_Samplers = 1, // kBinding_ComparisonSamplers = 2,
		// I thought better to alias samplers with comparisonSamplers HLSLside.
		kBinding_StorageImages = 3,
		kBinding_NumBindings = 3 // this should be # of all other
	};

	struct QueueFamilyIndices {
		std::optional<uint32_t> graphicsFamily;
		std::optional<uint32_t> presentFamily;
		std::optional<uint32_t> transferFamily;

		bool isComplete() const {
			return graphicsFamily.has_value() && presentFamily.has_value();
		}
	};

	typedef enum BufferType {
		BufferType_Index = 0,
		BufferType_Vertex,
		BufferType_Uniform,
		BufferType_Storage,
		BufferType_Indirect,
		BufferType_TransferSrc,
		BufferType_TransferDst,
		BufferType_Staging,
		BufferType_Count,
	} BufferType;

	struct BufferCreateInfo {
		VkDeviceSize size;
		BufferType bufferType;
		const char* name;
	};

	struct ImageCreateInfo {
		VkSampleCountFlags vk_samples = VK_SAMPLE_COUNT_1_BIT;
		VkExtent3D vk_extent;
		VkFormat vk_format = VK_FORMAT_UNDEFINED;
		VkImageUsageFlags vk_usage = VK_IMAGE_USAGE_SAMPLED_BIT |
			VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		uint32_t levels = 1;
		uint32_t arrayLayers = 1;
	};
	using TextureCreateInfo = ImageCreateInfo;
	struct ImageViewCreateInfo {
		VkImageViewCreateFlags     flags{};
		VkImage                    vk_image{};
		VkImageViewType            viewType{};
		VkFormat                   vk_format{};
		VkImageSubresourceRange    subResourceRange{};
	};
	struct SamplerCreateInfo {
		VkFilter mag = VK_FILTER_LINEAR;
		VkFilter min = VK_FILTER_LINEAR;
		VkSamplerMipmapMode mip = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		VkSamplerAddressMode u = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		VkSamplerAddressMode v = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		VkSamplerAddressMode w = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		float maxLod = VK_LOD_CLAMP_NONE;
		float maxAniso = 1.0f;
		VkCompareOp compareOp = VK_COMPARE_OP_ALWAYS;
		bool compareEnable = false;
		bool operator<(const SamplerCreateInfo& other) const noexcept {
			return std::tie(
				mag, min, mip,
				u, v, w,
				maxLod, maxAniso,
				compareOp, compareEnable
			) < std::tie(
				other.mag, other.min, other.mip,
				other.u, other.v, other.w,
				other.maxLod, other.maxAniso,
				other.compareOp, other.compareEnable
			);
		}
	};
	struct Image {

		VkImage vk_image = VK_NULL_HANDLE;
		VmaAllocation vma_alloc = VK_NULL_HANDLE;
		VkExtent3D vk_extent = { 1,1,1 };
		VkFormat vk_format = VK_FORMAT_UNDEFINED;
		VkImageUsageFlags vk_usage = VK_IMAGE_USAGE_SAMPLED_BIT |
			VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		uint32_t levels = 1;
		uint32_t arrayLayers = 1;
		VkSampleCountFlags vk_samples = VK_SAMPLE_COUNT_1_BIT;
		VkImageView sampleView = VK_NULL_HANDLE; // you can sample multiple levels
		VkImageView storageView = VK_NULL_HANDLE; // but can only render to one. Views are created with this baked in. Thats the why to separate them
		VkImageView arrayView[6]{
			VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
			VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE
		}; // for cubemap
		VkSampler vk_sampler = VK_NULL_HANDLE; // Not owning
		bool owns_views = true;
		bool is_swapchain = false;
		ImageCreateInfo getImageCreateInfo() const {
			return {
				.vk_extent = vk_extent,
				.vk_format = vk_format,
				.vk_usage = vk_usage,
				.levels = levels,
				.arrayLayers = arrayLayers
			};
		};
	};

	// Just piggybacking on vma really
	struct Buffer {
		VkBuffer vk_buffer = VK_NULL_HANDLE;
		VmaAllocation vma_alloc{};
		VkBufferUsageFlags usage = 0;
		VkMemoryPropertyFlags memory_flags = 0;
		VkDeviceAddress deviceAddress = 0;
		//uint64_t mapped = 0; // permanent mapping may be good
		[[nodiscard]] bool is_mappable() const noexcept {
			return (memory_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
		}
	};

	using ShaderModuleHandle = Handle<struct ShaderModule>; // will be removed when updating to vk 1.4
	using ImageHandle = Handle<struct Image>;
	using TextureHandle = Handle<struct Texture>;
	using SamplerHandle = Handle<struct Sampler>;
	using BufferHandle = Handle<struct Buffer>;
	using DescriptorPoolHandle = Handle<struct DescriptorPool>;

	struct Shader {
		ShaderModuleHandle vkShaderModule; // to be refactored out when moving to 1.4
		const char* entryPoint = "main";
		VkShaderStageFlagBits stage;
		//bool usePushConstants;
		//size_t pushConstantsSize // this should be gotten with reflection

		bool operator==(const Shader& other) const noexcept {
			return std::strcmp(entryPoint, other.entryPoint) == 0 &&
				stage == other.stage &&
				vkShaderModule == other.vkShaderModule;
		}
	};

	struct ShaderProgram {
		Shader shaders[8];
		size_t shaderCount = 0;
		bool operator==(const ShaderProgram& other) const noexcept
		{
			if (shaderCount != other.shaderCount)
			{
				return false;
			}

			for (size_t i = 0; i < shaderCount; ++i)
			{
				if (!(shaders[i] == other.shaders[i]))
					return false;
			}

			return true;
		}
	};

};
//Interfaces to implement
//struct ICore {};
//struct ISwapchain {};
//struct IResourceManager {};
//struct ICommandList {};
//struct IFrameBuffer {};