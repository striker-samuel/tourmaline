#pragma once

#include <Engine/Memory/handle.h>

#include <cassert>
#include <array>
#include <tuple>
#include <map>
#include <future>
#include <algorithm>
#include <optional>
#include <vector>
#include <set>

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

namespace smvk {

	struct Image;

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
	enum class ColorSpace : uint8_t
	{
		Undefined,

		SRGB_Nonlinear,

		DisplayP3_Nonlinear,
		DisplayP3_Linear,

		ExtendedSRGB_Linear,
		ExtendedSRGB_Nonlinear,

		DCI_P3_Nonlinear,

		BT709_Linear,
		BT709_Nonlinear,

		BT2020_Linear,

		HDR10_ST2084,
		HDR10_HLG,

		DolbyVision,

		AdobeRGB_Linear,
		AdobeRGB_Nonlinear,

		PassThrough,
	};

	// ============================================================================
	// Resource access
	// ============================================================================

	enum class AccessFlags : uint32_t
	{
		None = 0,

		IndirectCommandRead = 1u << 0,
		IndexRead = 1u << 1,
		VertexAttributeRead = 1u << 2,
		UniformRead = 1u << 3,
		InputAttachmentRead = 1u << 4,
		ShaderRead = 1u << 5,
		ShaderWrite = 1u << 6,
		ColorAttachmentRead = 1u << 7,
		ColorAttachmentWrite = 1u << 8,
		DepthStencilAttachmentRead = 1u << 9,
		DepthStencilAttachmentWrite = 1u << 10,
		TransferRead = 1u << 11,
		TransferWrite = 1u << 12,
		HostRead = 1u << 13,
		HostWrite = 1u << 14,
		MemoryRead = 1u << 15,
		MemoryWrite = 1u << 16,

		CommandPreprocessRead = 1u << 17,
		CommandPreprocessWrite = 1u << 18,
		ColorAttachmentReadNoncoherent = 1u << 19,
		ConditionalRenderingRead = 1u << 20,
		AccelerationStructureRead = 1u << 21,
		AccelerationStructureWrite = 1u << 22,
		FragmentShadingRateAttachmentRead = 1u << 23,
		FragmentDensityMapRead = 1u << 24,
		TransformFeedbackWrite = 1u << 25,
		TransformFeedbackCounterRead = 1u << 26,
		TransformFeedbackCounterWrite = 1u << 27,
	};

	constexpr AccessFlags operator|(AccessFlags a, AccessFlags b)
	{
		return static_cast<AccessFlags>(
			static_cast<uint32_t>(a) |
			static_cast<uint32_t>(b));
	}

	constexpr AccessFlags operator&(AccessFlags a, AccessFlags b)
	{
		return static_cast<AccessFlags>(
			static_cast<uint32_t>(a) &
			static_cast<uint32_t>(b));
	}

	constexpr AccessFlags& operator|=(AccessFlags& a, AccessFlags b)
	{
		a = a | b;
		return a;
	}

	constexpr bool HasAnyFlag(AccessFlags value, AccessFlags flags)
	{
		return (value & flags) != AccessFlags::None;
	}
	constexpr AccessFlags operator~(AccessFlags a) {
		return static_cast<AccessFlags>(~static_cast<uint32_t>(a));
	}
	constexpr AccessFlags operator^(AccessFlags a, AccessFlags b) {
		return static_cast<AccessFlags>(static_cast<uint32_t>(a) ^ static_cast<uint32_t>(b));
	}
	constexpr AccessFlags& operator^=(AccessFlags& a, AccessFlags b) { a = a ^ b; return a; }
	constexpr AccessFlags& operator&=(AccessFlags& a, AccessFlags b) { a = a & b; return a; }

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

		bool operator==(const VertexAttribute&) const = default;
	};

	struct VertexBinding
	{
		uint32_t binding = 0;
		uint32_t stride = 0;

		VertexInputRate inputRate = VertexInputRate::Vertex;

		std::vector<VertexAttribute> attributes;

		bool operator==(const VertexBinding&) const = default;
	};

	struct VertexFormatDescription
	{
		std::vector<VertexBinding> bindings;

		bool operator==(const VertexFormatDescription&) const = default;
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


	// BufferType encapsulates mmeoryUsage, locality and more.
	// This is a higher level abstraction than the rest of the rhi but reality is
	// we need to cover X uses for 99% of apps. No need to provideo MemoryUsage or Locality and expose them to user

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
		size_t size = 0;
		BufferType bufferType;
		const char* name = nullptr;
	};

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

	constexpr ImageUsage operator|(ImageUsage a, ImageUsage b)
	{
		return static_cast<ImageUsage>(
			static_cast<uint32_t>(a) |
			static_cast<uint32_t>(b)
			);
	}
	constexpr ImageUsage& operator|=(ImageUsage& a, ImageUsage b)
	{
		a = a | b;
		return a;
	}

	constexpr ImageUsage operator&(ImageUsage a, ImageUsage b)
	{
		return static_cast<ImageUsage>(
			static_cast<uint32_t>(a) &
			static_cast<uint32_t>(b)
			);
	}

	constexpr ImageUsage& operator&=(ImageUsage& a, ImageUsage b)
	{
		a = a & b;
		return a;
	}

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

	constexpr ImageAspect operator&(
		ImageAspect a,
		ImageAspect b)
	{
		return static_cast<ImageAspect>(
			static_cast<uint32_t>(a) &
			static_cast<uint32_t>(b)
			);
	}

	constexpr ImageAspect operator|(
		ImageAspect a,
		ImageAspect b)
	{
		return static_cast<ImageAspect>(
			static_cast<uint32_t>(a) |
			static_cast<uint32_t>(b)
			);
	}

	static constexpr uint32_t RemainingSubresources =
		UINT32_MAX;

	struct ImageSubresourceRange
	{
		ImageAspect aspect = ImageAspect::Color;

		uint32_t baseMipLevel = 0;
		uint32_t mipLevelCount = RemainingSubresources;

		uint32_t baseArrayLayer = 0;
		uint32_t arrayLayerCount = RemainingSubresources;
	};

	struct ClearColor
	{
		float r = 0.0f;
		float g = 0.0f;
		float b = 0.0f;
		float a = 0.0f;
	};

	struct ClearDepthStencil
	{
		float depth = 1.0f;
		uint32_t stencil = 0;
	};

	enum class AttachmentLoadOp : uint8_t
	{
		Load,
		Clear,
		DontCare
	};

	enum class AttachmentStoreOp : uint8_t
	{
		Store,
		DontCare
	};

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

	struct SamplerCreateInfo
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
		bool operator==(const SamplerCreateInfo& other) const noexcept
		{
			return !(*this < other) && !(other < *this);
		}
	};

	struct SamplerCreateInfoHash {
		static void hashCombine(size_t& seed, size_t v) noexcept {
			seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
		}
		size_t operator()(SamplerCreateInfo const& k) const noexcept {
			size_t h = 0;
			hashCombine(h, std::hash<int>{}(static_cast<int>(k.magFilter)));
			hashCombine(h, std::hash<int>{}(static_cast<int>(k.minFilter)));
			hashCombine(h, std::hash<int>{}(static_cast<int>(k.mipmapMode)));
			hashCombine(h, std::hash<int>{}(static_cast<int>(k.addressU)));
			hashCombine(h, std::hash<int>{}(static_cast<int>(k.addressV)));
			hashCombine(h, std::hash<int>{}(static_cast<int>(k.addressW)));
			hashCombine(h, std::hash<float>{}(k.mipLodBias));
			hashCombine(h, std::hash<float>{}(k.minLod));
			hashCombine(h, std::hash<float>{}(k.maxLod));
			hashCombine(h, std::hash<float>{}(k.maxAnisotropy));
			hashCombine(h, std::hash<int>{}(static_cast<int>(k.compareOp)));
			hashCombine(h, std::hash<bool>{}(k.compareEnable));
			return h;
		}
	};

	// ============================================================================
	// Shader
	// ============================================================================

	enum class ShaderStage : uint32_t
	{
		None = 0,

		Vertex = 1u << 0, // 0x01
		TessControl = 1u << 1, // 0x02
		TessEvaluation = 1u << 2, // 0x04
		Geometry = 1u << 3, // 0x08
		Fragment = 1u << 4, // 0x10
		Compute = 1u << 5, // 0x20

		AllGraphics = (1u << 5) - 1u, // 0x1F
		All = 0x7FFFFFFF,
		// Future:
		// Task
		// Mesh
		// RayGen
		// AnyHit
		// ClosestHit
		// Miss
		// Intersection
		// Callable
	};

	using ShaderStageFlags = ShaderStage;

	constexpr ShaderStage operator|(ShaderStage lhs, ShaderStage rhs)
	{
		return static_cast<ShaderStage>(
			static_cast<uint32_t>(lhs) |
			static_cast<uint32_t>(rhs)
			);
	}

	constexpr ShaderStage operator&(ShaderStage lhs, ShaderStage rhs)
	{
		return static_cast<ShaderStage>(
			static_cast<uint32_t>(lhs) &
			static_cast<uint32_t>(rhs)
			);
	}

	constexpr ShaderStage& operator|=(ShaderStage& lhs, ShaderStage rhs)
	{
		lhs = lhs | rhs;
		return lhs;
	}

	constexpr ShaderStage& operator&=(ShaderStage& lhs, ShaderStage rhs)
	{
		lhs = lhs & rhs;
		return lhs;
	}

	// ============================================================================
	// Shader program
	// ============================================================================

	struct ShaderModuleTag;
	using ShaderModuleHandle = Handle<ShaderModuleTag>;
	struct Shader
	{
		ShaderModuleHandle module;
		const char* entryPoint = "main";
		ShaderStage stage = ShaderStage::None;
		bool operator==(const Shader& other) const noexcept
		{
			return std::strcmp(entryPoint, other.entryPoint) == 0 &&
				stage == other.stage &&
				module == other.module;
		}
	};

	struct ShaderProgram
	{
		Shader shaders[8]{};
		size_t shaderCount = 0;

		bool operator==(const ShaderProgram& other) const noexcept
		{
			if (shaderCount != other.shaderCount)
				return false;

			for (size_t i = 0; i < shaderCount; ++i)
			{
				if (!(shaders[i] == other.shaders[i]))
					return false;
			}

			return true;
		}
	};

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

	enum class StencilFace : uint8_t
	{
		Front,
		Back,
		FrontAndBack,
	};

	struct StencilSettings {
		CompareOp frontCompareOp = CompareOp::Always;
		CompareOp backCompareOp = CompareOp::Always;
		uint8_t frontFailOp = STENCIL_OP_KEEP;
		uint8_t backFailOp = STENCIL_OP_KEEP;
		uint8_t frontPassOp = STENCIL_OP_KEEP;
		uint8_t backPassOp = STENCIL_OP_KEEP;
		StencilFace frontFace = StencilFace::Front;
		StencilFace backFace = StencilFace::Back;
		bool frontTestEnable = false;
		bool backTestEnable = false;
		bool frontWriteEnable = false;
		bool backWriteEnable = false;

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

		PipelineStageBit_Transfer = 1u << 14,

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
		kBinding_Samplers = 1, // I thought better to alias samplers with comparisonSamplers HLSLside.
		kBinding_StorageImages = 2,
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

	enum class PolygonMode : uint8_t
	{
		Fill,
		Line,
		Point,
	};

	enum class CullMode : uint8_t
	{
		None,
		Front,
		Back,
		FrontAndBack,
	};

	enum class FrontFace : uint8_t
	{
		CounterClockwise,
		Clockwise,
	};

	struct RasterSettings
	{
		PolygonMode polygonMode = PolygonMode::Fill;
		CullMode cullMode = CullMode::Back;
		FrontFace frontFace = FrontFace::CounterClockwise;

		bool depthClampEnable = false;
		bool rasterizerDiscardEnable = false;

		float lineWidth = 1.0f;

		bool operator<(const RasterSettings& rhs) const noexcept
		{
			return std::tie(
				polygonMode,
				cullMode,
				frontFace,
				depthClampEnable,
				rasterizerDiscardEnable,
				lineWidth
			) <
				std::tie(
					rhs.polygonMode,
					rhs.cullMode,
					rhs.frontFace,
					rhs.depthClampEnable,
					rhs.rasterizerDiscardEnable,
					rhs.lineWidth
				);
		}
	};



	struct PipelineKey
	{
		ShaderProgram program{};
		VertexFormatDescription vertexFormat{};
		std::vector<Format> colorFormats;
		Format depthFormat = Format::Undefined;
		Format stencilFormat = Format::Undefined;
		PrimitiveTopology topology = PrimitiveTopology::TriangleList;
		RasterSettings raster{};
		DepthSettings depth{};
		StencilSettings stencil{};
		BlendSettings blend{};
		MSAASettings msaa{};

		bool operator==(const PipelineKey& rhs) const noexcept
		{
			return
				program == rhs.program &&
				vertexFormat == rhs.vertexFormat &&
				colorFormats == rhs.colorFormats &&
				depthFormat == rhs.depthFormat &&
				stencilFormat == rhs.stencilFormat &&
				topology == rhs.topology &&

				!(raster < rhs.raster) &&
				!(rhs.raster < raster) &&

				!(depth < rhs.depth) &&
				!(rhs.depth < depth) &&

				!(stencil < rhs.stencil) &&
				!(rhs.stencil < stencil) &&

				!(blend < rhs.blend) &&
				!(rhs.blend < blend) &&

				msaa.samples == rhs.msaa.samples;
		}
	};

	struct ImageCreateInfo
	{
		ImageType type = ImageType::Image2D;
		Extent3D extent{};
		Format format = Format::Undefined;
		ImageUsage usage =
			ImageUsage::Sampled |
			ImageUsage::TransferDst;
		SampleCount samples = SampleCount::Count1;
		uint32_t mipLevels = 1;
		uint32_t arrayLayers = 1;
		//ImageCreateFlags flags = ImageCreateFlags::None;
		const char* name = nullptr;
	};

	enum class ResourceOwnership : uint8_t
	{
		Owned,
		External
	};

	struct ImageViewCreateInfo
	{
		ImageViewType viewType = ImageViewType::View2D;
		Format format = Format::Undefined; // Undefined = inherit from underlying Image.
		ImageSubresourceRange subresourceRange{};
		const char* name = nullptr;
	};

	struct ImageTag;
	struct ImageViewTag;
	struct BufferTag;
	struct TextureTag;
	struct SamplerTag;
	struct BindlessImageTag;
	struct DescriptorPoolTag;

	using ImageHandle = Handle<ImageTag>;
	using ImageViewHandle = Handle<ImageViewTag>;
	using BufferHandle = Handle<BufferTag>;
	using TextureHandle = Handle<TextureTag>;
	using SamplerHandle = Handle<SamplerTag>;
	using BindlessImageHandle = Handle<BindlessImageTag>;
	using DescriptorPoolHandle = Handle<DescriptorPoolTag>;

	// Forward declaration for PIMPL firewall – concrete type lives in RHI/resource_manager.h
	class ResourceManager;

	// RHI barrier / layout enums (opaque, no Vk leakage)
	enum class ImageLayout : uint8_t {
		Undefined = 0,
		General,
		ColorAttachmentOptimal,
		DepthStencilAttachmentOptimal,
		DepthStencilReadOnlyOptimal,
		DepthReadOnlyOptimal,
		ShaderReadOnlyOptimal,
		TransferSrcOptimal,
		TransferDstOptimal,
		PresentSrc
	};

	void destroy(ResourceManager* ctx, ImageHandle handle);
	void destroy(ResourceManager* ctx, TextureHandle handle);
	void destroy(ResourceManager* ctx, BufferHandle handle);
	void destroy(ResourceManager* ctx, SamplerHandle handle);
	void destroy(ResourceManager* ctx, BindlessImageHandle handle);
	void destroy(ResourceManager* ctx, ImageViewHandle handle);
	void destroy(ResourceManager* ctx, DescriptorPoolHandle handle);
	void destroy(ResourceManager* ctx, ShaderModuleHandle handle); // will be removed in 1.4. transition

	 // Sync types translated from nicegraf. RHI-opaque, no Vk leakage.
	struct SyncBarrierMasks
	{
		AccessFlags accessMask = AccessFlags::None;
		PipelineStage stageMask = PipelineStageBit_None;
		bool operator==(const SyncBarrierMasks&) const = default;
	};
	struct ImageSyncRequest {
		SyncBarrierMasks barrierMasks{};
		ImageLayout layout = ImageLayout::Undefined;
		bool operator==(const ImageSyncRequest&) const = default;
	};
	struct BufferSyncRequest {
		SyncBarrierMasks barrierMasks{};
		bool operator==(const BufferSyncRequest&) const = default;
	};
	using SyncRequest = ImageSyncRequest;

	constexpr bool IsWriteAccess(AccessFlags a) {
		constexpr AccessFlags writeMask =
			AccessFlags::ShaderWrite |
			AccessFlags::ColorAttachmentWrite |
			AccessFlags::DepthStencilAttachmentWrite |
			AccessFlags::TransferWrite |
			AccessFlags::HostWrite |
			AccessFlags::MemoryWrite |
			AccessFlags::AccelerationStructureWrite;
		return (a & writeMask) != AccessFlags::None;
	}
	constexpr bool IsReadAccess(AccessFlags a) {
		return a != AccessFlags::None && !IsWriteAccess(a);
	}

	inline PipelineStage DerivePipelineStage(AccessFlags access, PipelineStage hint = PipelineStageBit_None) {
		if (hint != PipelineStageBit_None) return hint;
		if ((access & AccessFlags::ColorAttachmentWrite) != AccessFlags::None ||
			(access & AccessFlags::ColorAttachmentRead) != AccessFlags::None)
			return PipelineStageBit_Fragment;
		if ((access & AccessFlags::DepthStencilAttachmentWrite) != AccessFlags::None ||
			(access & AccessFlags::DepthStencilAttachmentRead) != AccessFlags::None)
			return PipelineStageBit_Fragment;
		if ((access & AccessFlags::ShaderRead) != AccessFlags::None ||
			(access & AccessFlags::ShaderWrite) != AccessFlags::None ||
			(access & AccessFlags::UniformRead) != AccessFlags::None)
			return PipelineStageBit_Fragment | PipelineStageBit_Vertex | PipelineStageBit_Compute;
		if ((access & AccessFlags::TransferRead) != AccessFlags::None ||
			(access & AccessFlags::TransferWrite) != AccessFlags::None)
			return PipelineStageBit_Transfer;
		return PipelineStageBit_Fragment;
	}


	[[nodiscard]] constexpr ImageAspect DefaultViewAspect(Format format)
	{
		switch (format)
		{
		case Format::D16_UNorm:
		case Format::D32_Float:
			return ImageAspect::Depth;

		case Format::D24_UNorm_S8_UInt:
		case Format::D32_Float_S8_UInt:
			// Policy decision: default DS views to depth.
			return ImageAspect::Depth;

		case Format::Undefined:
			return ImageAspect::None;

		default:
			return ImageAspect::Color;
		}
	}
	inline  StencilSettings StencilSettings::DefaultDisabled()
	{
		StencilSettings s{};
		s.frontTestEnable = false;
		s.backTestEnable = false;
		s.frontWriteEnable = false;
		s.backWriteEnable = false;
		return s;
	}
	inline  StencilSettings StencilSettings::DefaultEnabled()
	{
		StencilSettings s{};
		s.frontTestEnable = true;
		s.backTestEnable = true;
		s.frontWriteEnable = true;
		s.backWriteEnable = true;
		return s;
	}

} // namespace smvk
