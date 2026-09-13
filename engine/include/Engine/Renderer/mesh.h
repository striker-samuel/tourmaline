#pragma once

#include <span>
#include <vector>
#include <optional>

#include <glm/glm.hpp>
#include <Engine/smvk.h>
#include <Engine/Asset/meshlet.h>

namespace smvk {

	// GPU material buffer layout – must match HLSL common.hlsli::MaterialValues.
	// Keep 16-byte alignment for StructuredBuffer / buffer_device_address.
	struct alignas(16) MaterialValues {

		// Legacy sky-ambient factor (albedo multiplier): 0 = off by default,
		// ambient comes from the env/IBL block. Paks round-trip this verbatim,
		// so paks cooked with an old default keep their baked value.
		glm::vec4 ambient{ 0.f };                       // 0 – see cluster.cpp
		glm::vec4 diffuse{ 1.0f };                       // 16 – baseColorFactor (alias: diffuse)
		glm::ivec4 textures{ -1 };                       // 32 – x=baseColor, y=metallicRoughness, z=normal, w=occlusion
		glm::ivec4 textures2{ -1 };                      // 48 – x=emissive, yzw unused (-1)
		glm::vec4 params{ 0.5f, 1.0f, 1.0f, 0.0f };       // 64 – x=alphaCutoff, y=metallic, z=roughness, w=shadingModel
		glm::vec4 params2{ 1.0f, 1.0f, 1.0f, 0.0f };      // 80 – x=occlusionStrength, y=normalScale, z=emissiveStrength, w=alphaMode (0=Opaque,1=Mask,2=Blend)
		glm::vec4 emissiveFactor{ 0.0f, 0.0f, 0.0f, 0.0f }; // 96 – xyz=emissiveFactor, w=pad
		// Layered materials (vertex-color blending): x/y/z hold the
		// cluster-local material pool indices of layers 1..3 (the material
		// itself is layer 0; -1 = no layer). w selects the mode: 0 = legacy
		// (vertex color is a plain albedo tint), 1..4 = layered with that
		// many active layers (weights from the vertex color RGBA channels,
		// normalized in the shader). Cluster-local ids are remapped to
		// world material ids by World::ResolveAndFixup_clusters, which also
		// appends slots for layer materials no node references directly.
		glm::ivec4 layerMaterials{ -1, -1, -1, 0 };      // 112
	};
	static_assert(sizeof(MaterialValues) % 16 == 0);
	static_assert(sizeof(MaterialValues) == 128);

	// -------------------------------------------------------------------------
	// Shading / raster abstraction
	// -------------------------------------------------------------------------

	enum class ShadingModel : uint8_t {
		DefaultLit = 0, // PBR metallic-roughness, single forward pass
		Unlit,          // baseColor only, no lighting
		Masked,         // alpha-tested (cutout), uses alphaCutoff
	};

	enum class AlphaMode : uint8_t {
		Opaque = 0,
		Mask,
		Blend,
	};

	// Runtime transparency technique for AlphaMode::Blend materials.
	// Dithered (default): screen-door discard in shader, opaque pipeline
	// state (depth write on, blending off) – no sorting, correct depth/MSAA.
	// Blended: classic back-to-front alpha blending (no depth write).
	enum class TransparentMode : uint8_t {
		Dithered = 0,
		Blended,
	};

	struct MaterialPipelineState {
		RasterSettings raster{};
		DepthSettings depth{};
		BlendSettings blend{};
	};

	struct MaterialCreateInfo {
		// Primary PBR textures – baseColorTexture == diffuse for backward compat.
		TextureHandle baseColorTexture{};
		TextureHandle metallicRoughnessTexture{};
		TextureHandle normalTexture{};
		TextureHandle occlusionTexture{};
		TextureHandle emissiveTexture{};
		// Legacy alias: alias for baseColorTexture
		TextureHandle diffuse{};

		glm::vec4 baseColorFactor{ 1.0f };
		glm::vec3 emissiveFactor{ 0.0f };
		float emissiveStrength = 1.0f;

		float metallicFactor = 1.0f;
		float roughnessFactor = 1.0f;
		float normalScale = 1.0f;
		float occlusionStrength = 1.0f;

		ShadingModel shadingModel = ShadingModel::DefaultLit;
		AlphaMode alphaMode = AlphaMode::Opaque;
		float alphaCutoff = 0.5f;
		bool doubleSided = false;
	};

	MaterialPipelineState MakePipelineStateForMaterial(
		AlphaMode alphaMode,
		bool doubleSided,
		ShadingModel shadingModel = ShadingModel::DefaultLit);

	struct Material {
		MaterialValues values{};
		ShadingModel shadingModel = ShadingModel::DefaultLit;
		AlphaMode alphaMode = AlphaMode::Opaque;
		float alphaCutoff = 0.5f;
		bool doubleSided = false;

		// Derived / cached pipeline state. Authoritative for PSO selection
		// and for World iterator flags. Mutated only via helpers so values
		// and pipeline stay coherent.
		MaterialPipelineState pipeline{};

		bool isTransparent() const noexcept { return alphaMode == AlphaMode::Blend; }
		bool isCutout() const noexcept { return alphaMode == AlphaMode::Mask; }
		bool isDoubleSided() const noexcept { return doubleSided; }

		void rebuildPipeline() { pipeline = MakePipelineStateForMaterial(alphaMode, doubleSided, shadingModel); }
	};


	// -----------------------------------------------------------------------------
	// GPU vertex streams
	// -----------------------------------------------------------------------------

	struct VertexPos {
		glm::vec3 pos;
	};

	// GPU shading stream (16 bytes): color is packed via
	// vertexpack::PackColorUnorm8 and fetched as Format::RGBA8_UNorm, uv via
	// vertexpack::PackUvHalf fetched as Format::RG16_Float, normal/tangent
	// via PackNormalOct/PackTangentOct fetched as Format::R32_UInt and
	// unpacked by hand in the vertex shader (see vertex_pack.hlsli).
	// The bitangent is never stored (B = cross(N, T) * w in the fragment).
	struct VertexShading {
		uint32_t color;      // RGBA8_UNORM
		uint32_t uv;         // RG16_FLOAT (packHalf2x16)
		uint32_t normal;     // custom oct encoding (16+16)
		uint32_t tangent;    // custom oct encoding (16+15) + handedness bit
	};
	static_assert(sizeof(VertexShading) == 16);


	// CPU-side import vertex
	struct StaticVertex {
		glm::vec3 pos;
		glm::vec4 color;
		glm::vec2 texCoord;
		glm::vec3 normal;
		glm::vec4 tangent;

		bool operator==(const StaticVertex& other) const {
			return pos == other.pos &&
				color == other.color &&
				texCoord == other.texCoord &&
				normal == other.normal &&
				tangent == other.tangent;
		}
	};


	// Debug line stream (pos-only, binding 0): powers the meshlet/cone
	// debug views without touching the static heap (see BindVertexBuffer).
	struct DebugLineVertex {
		glm::vec3 pos;
	};
	static_assert(sizeof(DebugLineVertex) == 12);

	inline const VertexFormatDescription DebugLineFormat{
		.bindings = {
			{
				.binding = 0,
				.stride = sizeof(DebugLineVertex),
				.inputRate = VertexInputRate::Vertex,
				.attributes = {
					{
						.semantic = VertexSemantic::Position,
						.format = Format::RGB32_Float,
						.offset = offsetof(DebugLineVertex, pos)
					}
				}
			}
		}
	};

	inline const VertexFormatDescription StaticVertexFormat{
		.bindings = {
			{
				.binding = 0,
				.stride = sizeof(VertexPos),
				.inputRate = VertexInputRate::Vertex,
				.attributes = {
					{
						.semantic = VertexSemantic::Position,
						.format = Format::RGB32_Float,
						.offset = offsetof(VertexPos, pos)
					}
				}
			},
			{
				.binding = 1,
				.stride = sizeof(VertexShading),
				.inputRate = VertexInputRate::Vertex,
			.attributes = {
				{
					.semantic = VertexSemantic::Color,
						.format = Format::RGBA8_UNorm,
						.offset = offsetof(VertexShading, color)
					},
					{
						.semantic = VertexSemantic::TexCoord0,
						.format = Format::RG16_Float,
						.offset = offsetof(VertexShading, uv)
					},
					{
						.semantic = VertexSemantic::Normal,
						.format = Format::R32_UInt,
						.offset = offsetof(VertexShading, normal)
					},
					{
						.semantic = VertexSemantic::Tangent,
						.format = Format::R32_UInt,
						.offset = offsetof(VertexShading, tangent)
					}
				}
			}
		}
	};


	// -----------------------------------------------------------------------------
	// Draw information
	// -----------------------------------------------------------------------------

	struct Surface {
		uint32_t firstIndex = 0;
		uint32_t indexCount = 0;
		int32_t baseVertex = 0;
	};

	struct Geometry {
		PrimitiveTopology topology = PrimitiveTopology::TriangleList;

		uint32_t vertexCount = 0;
		uint32_t indexCount = 0;

		std::vector<Surface> surfaces;

		glm::vec3 aabbMin{};
		glm::vec3 aabbMax{};
		float sphereRadius = 0.0f;
	};


	// -----------------------------------------------------------------------------
	// Static mesh
	// -----------------------------------------------------------------------------

	struct StaticMesh {
		Geometry geometry;
		// Cook-time meshlet data for the future indirect/mesh-shader path.
		// Empty when the mesh was cooked without meshlets. Never consulted
		// by the current direct-draw renderer.
		std::optional<CookedMeshlets> meshlets;
	};

	// -----------------------------------------------------------------------------
	// Upload structure
	// -----------------------------------------------------------------------------

	struct StaticMeshUpload {
		std::vector<StaticVertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<Surface> surfaces;
		glm::vec3 aabbMin{};
		glm::vec3 aabbMax{};
	};

	void AllocateStaticVertexFmt(smvk::ResourceManager& ctx);

	enum struct VertexFormat {
		Empty
		, StaticVertexFormat
		, DebugLine
	};

	// Returns the static description for the given format (no per-call
	// copies: BindRenderPipeline runs this on every bind).
	const VertexFormatDescription& GetVertexFormatDescription(VertexFormat format);



} // namespace smvk