#pragma once

#include <cstddef>
#include <span>
#include <vector>
#include <optional>

#include <glm/glm.hpp>
#include <Engine/smvk.h>
#include <Engine/Asset/meshlet.h>
#include <Engine/Renderer/vertex_pack.h>

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

	// ---- Shader-LOD policy (per material) ---------------------------
	// Replaces the old hardcoded LodShader enum: each material declares
	// how its farthest cooked LOD level shades. Two mechanisms, both
	// data-driven per material:
	//   1. Whole-program substitution: the farthest level may use the
	//      cheap vertex-lit program (per-vertex sun + ambient, no
	//      textures/shadows), so far instances trade shading quality
	//      for fragment cost; materials the cheap program cannot shade
	//      stay fully lit at every level.
	//   2. Quality specialization: when the farthest level stays on the
	//      lit program, it binds with the specialization constants below
	//      (simplified BRDF, no normal-map fetch, ...), so the far
	//      program pays for only what the material needs at distance.
	struct ShaderLodQuality {
		// GGX specular lobe (false = Lambert-only "simplified BRDF": no
		// specular math, no specular env fetch, no per-light specular).
		bool specular = true;
		// Tangent-space normal-map fetch (false = geometric normal: no
		// normal-texture sample, no TBN work).
		bool normalMap = true;
		bool operator==(const ShaderLodQuality&) const = default;
	};
	struct ShaderLodPolicy {
		// Farthest cooked level may use the cheap vertex-lit program.
		// Disable for materials that need the full program at distance
		// (alpha-tested cutouts with alpha textures, emissives you want
		// to keep glowing, view-dependent effects...).
		bool allowFarVertexLit = true;
		// Quality spec applied to the lit program when the farthest
		// level stays lit (allowFarVertexLit == false). Default =
		// full quality (indistinguishable from the base program).
		ShaderLodQuality farQuality{};
	};

	// Specialization constant IDs for the far-level quality knobs above.
	// Must match common.hlsli (SPEC_LIT_*).
	inline constexpr uint32_t kSpecLitSpecular = 20;
	inline constexpr uint32_t kSpecLitNormalMap = 21;

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
		// Shadow participation (SMVK_materials_shadows.castShadow). Decal
		// overlay materials opt out at cook/import time; the World flag
		// build gates IteratorFlagBit_CastShadow on it.
		bool castsShadows = true;
		// Far-LOD shading program (see ShaderLodPolicy): the farthest
		// cooked level may use the cheap vertex-lit program. Author
		// false for materials the cheap program cannot shade (the glTF
		// importer derives this from alphaMode: masked/blend stay lit).
		bool allowFarVertexLit = true;
		// Far-LOD quality spec for materials that stay on the lit
		// program (see ShaderLodQuality): specular/normal-map switches
		// for the farthest cooked level. Tour attributes farSpecular /
		// farNormalMap (1 = full quality, 0 = simplified).
		ShaderLodQuality farQuality{};
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
		bool castsShadows = true;
		ShaderLodPolicy shaderLod{};

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

	// GPU shading stream (24 bytes): color is packed via
	// vertexpack::PackColorUnorm8 and fetched as Format::RGBA8_UNorm, uv
	// via vertexpack::PackUvHalf fetched as Format::RG16_Float, chartUv
	// (the CHART-LOCAL lightmap UV in the chart's inner rect, [0,1]) via
	// vertexpack::PackChartUv fetched as Format::RG16_UNorm — UNORM
	// because chart-space UVs need 65536-step precision (half-float is
	// 1-texel-at-2048, 4-texel-at-8192), normal/tangent via
	// PackNormalOct/PackTangentOct fetched as Format::R32_UInt and
	// unpacked by hand in the vertex shader (see vertex_pack.hlsli).
	// The bitangent is never stored (B = cross(N, T) * w in the fragment).
	// chart (R32_UInt) is the owning lightmap chart index (0xFFFFFFFF =
	// uncharted); the vertex shader resolves the final atlas UV as
	// chartUv * xform.scale + xform.offset with the per-draw chart offset
	// (see GfxDraw::lmChartOffset). The ATLAS-space uv2 no longer rides
	// the vertex stream — it is unpacked in the shader.
	struct VertexShading {
		uint32_t color;      // RGBA8_UNORM
		uint32_t uv;         // RG16_FLOAT (packHalf2x16)
		uint32_t chartUv;    // RG16_UNORM (packUnorm2x16) — chart-local UV
		uint32_t normal;     // custom oct encoding (16+16)
		uint32_t tangent;    // custom oct encoding (16+15) + handedness bit
		uint32_t chart;      // R32_UINT — lightmap chart index
	};
	static_assert(sizeof(VertexShading) == 24);

	// GPU shading stream for SKINNED meshes (16 bytes, no lightmaps):
	// color, uv, normal, tangent only. Skinned meshes are dynamic and
	// never carry lightmap charts, so chartUv/chartId are omitted
	// entirely (no TexCoord1/ChartId attributes, tighter stride).
	struct VertexSkinnedShading {
		uint32_t color;      // RGBA8_UNORM
		uint32_t uv;         // RG16_FLOAT (packHalf2x16)
		uint32_t normal;     // custom oct encoding (16+16)
		uint32_t tangent;    // custom oct encoding (16+15) + handedness bit
	};
	static_assert(sizeof(VertexSkinnedShading) == 16);

	// GPU skin stream (8 bytes, binding 2): joint ids RGBA8_UInt + blend
	// weights RGBA8_UNorm, 4 influences per vertex. Joint ids index the
	// mesh's skin palette (the Skeleton joint table, glTF JOINTS_0 order),
	// which caps skeletons at 256 joints; weights are 255-based and
	// renormalized in the vertex shader (w /= w0+w1+w2+w3, see
	// touranim.h for the palette contract). Skinned pipelines fetch
	// bindings 0 (position) + 2 (skin) for depth/shadow passes and
	// 0+1+2 for shading, so every static pipeline stays untouched;
	// JointIndices/JointWeights append as VSIn locations 5/6 (binding 1
	// carries no TexCoord1/ChartId). VS math: skinMat = sum(w_i * pal[j_i])
	// / sum(w_i); pos = skinMat * pos; normal/tangent = mat3(skinMat) * n,
	// renormalized.
	struct VertexSkin {
		uint32_t joints;   // RGBA8_UINT — palette indices x4
		uint32_t weights;  // RGBA8_UNORM — blend weights x4 (renormalize in VS)
	};
	static_assert(sizeof(VertexSkin) == 8);

	// GPU TERRAIN vertex (32 bytes, ONE interleaved binding): the tiled
	// heightfield's own stream — never the glTF static streams (generated
	// geometry, no UVs/charts/skin; the tangent carries its bitangent-flip
	// handedness in w so no side channel is needed). Layout (offsets are
	// load-bearing, see TerrainVertexFormat):
	//   POSITION      R32G32B32_FLOAT  @ 0   (pos.pos)
	//   NORMAL        R16G16B16A16_FLOAT @ 12  (normal[4], xyz + pad)
	//   TANGENT_BFLIP R16G16B16A16_FLOAT @ 20  (tangent[4], xyz + flip w)
	//   COLOR         R8G8B8A8_UNORM    @ 28  (color[4], albedo tint)
	// The LOWER-DETAIL format (TerrainLowVertexFormat) binds POSITION +
	// COLOR only, at the SAME offsets in the SAME 32-byte stride — one
	// vertex buffer serves both pipelines; the far/low pipeline just
	// fetches two attributes instead of four (the terrain LOD's cheap
	// shading path skips normal/tangent entirely).
	struct TerrainVertex {
		glm::vec3 pos;         // @ 0  (12 B)
		uint16_t normal[4];     // @ 12 (8 B)  half xyz (w pad)
		uint16_t tangent[4];   // @ 20 (8 B)  half xyz + bitangent flip w
		uint8_t color[4];      // @ 28 (4 B)  RGBA8_UNORM albedo tint
	};
	static_assert(sizeof(TerrainVertex) == 32);
	static_assert(offsetof(TerrainVertex, pos) == 0);
	static_assert(offsetof(TerrainVertex, normal) == 12);
	static_assert(offsetof(TerrainVertex, tangent) == 20);
	static_assert(offsetof(TerrainVertex, color) == 28);

	// CPU-side terrain vertex authoring form (floats in, packed at build
	// time): terrain.tile builders fill these and PackTerrainVertex
	// writes the GPU struct. colorAlpha rides the color.w channel (the
	// layered-blend weight future path; 1 by default).
	struct TerrainVertexSrc {
		glm::vec3 pos;
		glm::vec3 normal{ 0.0f, 0.0f, 1.0f };
		glm::vec3 tangent{ 1.0f, 0.0f, 0.0f };
		float tangentFlip = 1.0f;
		glm::vec4 color{ 1.0f };
	};

	inline void PackTerrainVertex(const TerrainVertexSrc& src,
		TerrainVertex& out) noexcept {
		out.pos = src.pos;
		vertexpack::PackTerrainNormalHalf(src.normal, out.normal);
		vertexpack::PackTerrainTangentHalf(src.tangent, src.tangentFlip,
			out.tangent);
		out.color[0] = uint8_t(glm::clamp(
			int(src.color.r * 255.0f + 0.5f), 0, 255));
		out.color[1] = uint8_t(glm::clamp(
			int(src.color.g * 255.0f + 0.5f), 0, 255));
		out.color[2] = uint8_t(glm::clamp(
			int(src.color.b * 255.0f + 0.5f), 0, 255));
		out.color[3] = uint8_t(glm::clamp(
			int(src.color.a * 255.0f + 0.5f), 0, 255));
	}


	// CPU-side import vertex. texCoord2 is the LIGHTMAP chart-UV channel
	// (glTF TEXCOORD_1): filled by the importer when the attribute exists
	// and overwritten with the CHART-LOCAL uv by the lightmap cook at bake
	// time (the atlas-space transform lives per chart in
	// StaticMesh::lmChartXforms and is applied in the vertex shader).
	// Zero when a scene carries no lightmaps (shader gates on the
	// per-draw flag, never on the value). chartId is the owning lightmap
	// chart index (0xFFFFFFFF = uncharted), stamped by the cook alongside
	// texCoord2 (seam corners are duplicated per chart).
		struct StaticVertex {
		glm::vec3 pos;
		glm::vec4 color;
		glm::vec2 texCoord;
		glm::vec2 texCoord2;
		glm::vec3 normal;
		glm::vec4 tangent;
		uint32_t chartId = 0xFFFFFFFFu;

		bool operator==(const StaticVertex& other) const {
			return pos == other.pos &&
				color == other.color &&
				texCoord == other.texCoord &&
				texCoord2 == other.texCoord2 &&
				normal == other.normal &&
				tangent == other.tangent &&
				chartId == other.chartId;
		}
	};

	// One AoS import vertex -> one VertexPos + one VertexShading (the
	// exact GPU heap-stream layout). Shared by Cluster::UploadStaticMesh(es)
	// (runtime upload) and the pak cooker's packed-SoA mesh blobs (v4;
	// cooked geometry round-trips the heap layout verbatim) — the two must
	// never diverge.
	inline void PackStaticVertex(const StaticVertex& src,
		VertexPos& pos, VertexShading& shade) noexcept {
		pos.pos = src.pos;
		shade.color = vertexpack::PackColorUnorm8(src.color);
		shade.uv = vertexpack::PackUvHalf(src.texCoord);
		shade.chartUv = vertexpack::PackChartUv(src.texCoord2);
		shade.chart = src.chartId;
		shade.normal = vertexpack::PackNormalOct(src.normal);
		shade.tangent = vertexpack::PackTangentOct(
			glm::vec3(src.tangent), src.tangent.w);
	}

	// Inverse pack (the .mesh-asset cook path rebuilds AoS uploads from
	// the packed SoA streams; lossy exactly like the GPU fetch, so the
	// round trip is byte-stable).
	inline void UnpackStaticVertex(const VertexPos& pos,
		const VertexShading& shade, StaticVertex& out) noexcept {
		out.pos = pos.pos;
		const glm::vec4 c = glm::unpackUnorm4x8(shade.color);
		out.color = c;
		out.texCoord = glm::unpackHalf2x16(shade.uv);
		out.texCoord2 = glm::unpackUnorm2x16(shade.chartUv);
		out.chartId = shade.chart;
		out.normal = vertexpack::UnpackNormalOct(shade.normal);
		out.tangent = vertexpack::UnpackTangentOct(shade.tangent);
	}

	// CPU-side import vertex (skinned): position/color/uv/normal/tangent
	// plus the glTF JOINTS_0/WEIGHTS_0 pair. The importer keeps the top 4
	// influences, renormalizes the weights, and packs at upload (joints ->
	// RGBA8_UInt, weights -> RGBA8_UNorm) into the binding-2 skin stream.
	// No texCoord2/charts, no chartId: skinned meshes are dynamic and
	// never lightmapped.
	struct SkinnedVertex {
		glm::vec3 pos;
		glm::vec4 color;
		glm::vec2 texCoord;
		glm::vec3 normal;
		glm::vec4 tangent;
		glm::u8vec4 joints;  // palette indices x4 (Skeleton joint table)
		glm::vec4 weights;   // normalized, sums to 1

		bool operator==(const SkinnedVertex& other) const {
			return pos == other.pos &&
				color == other.color &&
				texCoord == other.texCoord &&
				normal == other.normal &&
				tangent == other.tangent &&
				joints == other.joints &&
				weights == other.weights;
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
				// Location order is load-bearing: shader VSIn locations
				// 0..4 are Position, Color, TexCoord0, Normal, Tangent.
				// TexCoord1 (chart-local lightmap UV) appends as location
				// 5 and ChartId as location 6 so existing vertex shaders
				// keep their locations; shaders that don't declare them
				// simply ignore the attributes.
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
					},
				{
					.semantic = VertexSemantic::TexCoord1,
					.format = Format::RG16_UNorm,
					.offset = offsetof(VertexShading, chartUv)
				},
				{
					.semantic = VertexSemantic::ChartId,
					.format = Format::R32_UInt,
					.offset = offsetof(VertexShading, chart)
				}
			}
		}
	}
};

	// Skinned vertex format: bindings 0 (position) + 1 (chart-free
	// VertexSkinnedShading: color/uv/normal/tangent only) plus the
	// binding-2 skin stream. No TexCoord1/ChartId — dynamic meshes never
	// sample lightmaps — so JointIndices/JointWeights follow as locations
	// 5/6. Depth/shadow passes bind only bindings 0+2 (position + skin) —
	// skinning must move the position.
	inline const VertexFormatDescription SkinnedVertexFormat{
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
				.stride = sizeof(VertexSkinnedShading),
				.inputRate = VertexInputRate::Vertex,
				.attributes = {
					{
						.semantic = VertexSemantic::Color,
						.format = Format::RGBA8_UNorm,
						.offset = offsetof(VertexSkinnedShading, color)
					},
					{
						.semantic = VertexSemantic::TexCoord0,
						.format = Format::RG16_Float,
						.offset = offsetof(VertexSkinnedShading, uv)
					},
					{
						.semantic = VertexSemantic::Normal,
						.format = Format::R32_UInt,
						.offset = offsetof(VertexSkinnedShading, normal)
					},
					{
						.semantic = VertexSemantic::Tangent,
						.format = Format::R32_UInt,
						.offset = offsetof(VertexSkinnedShading, tangent)
					}
				}
			},
			{
				.binding = 2,
				.stride = sizeof(VertexSkin),
				.inputRate = VertexInputRate::Vertex,
				.attributes = {
					{
						.semantic = VertexSemantic::JointIndices,
						.format = Format::RGBA8_UInt,
						.offset = offsetof(VertexSkin, joints)
					},
					{
						.semantic = VertexSemantic::JointWeights,
						.format = Format::RGBA8_UNorm,
						.offset = offsetof(VertexSkin, weights)
					}
				}
			}
		}
	};


	// Terrain FULL-detail format: ONE interleaved binding, 32-byte
	// stride, four attributes (near tiles; the shading path reads the
	// normal + tangent + bitangent flip). Attribute order is
	// load-bearing: terrain VSIn locations are 0=Position, 1=Normal,
	// 2=TangentBFlip, 3=Color (locations are assigned in list order,
	// see getVkVertexAttributeDescriptions).
	inline const VertexFormatDescription TerrainVertexFormat{
		.bindings = {
			{
				.binding = 0,
				.stride = sizeof(TerrainVertex),
				.inputRate = VertexInputRate::Vertex,
				.attributes = {
					{
						.semantic = VertexSemantic::Position,
						.format = Format::RGB32_Float,
						.offset = offsetof(TerrainVertex, pos)
					},
					{
						.semantic = VertexSemantic::Normal,
						.format = Format::RGBA16_Float,
						.offset = offsetof(TerrainVertex, normal)
					},
					{
						.semantic = VertexSemantic::TangentBFlip,
						.format = Format::RGBA16_Float,
						.offset = offsetof(TerrainVertex, tangent)
					},
					{
						.semantic = VertexSemantic::Color,
						.format = Format::RGBA8_UNorm,
						.offset = offsetof(TerrainVertex, color)
					}
				}
			}
		}
	};

	// Terrain LOWER-detail format: the SAME binding + stride + offsets,
	// two attributes only (far tiles; the cheap shading path skips the
	// normal/tangent fetch entirely — POSITION @0 and COLOR @28). One
	// vertex buffer serves BOTH formats: the far pipeline simply binds
	// fewer attributes out of the same 32-byte records. VSIn locations
	// are 0=Position, 1=Color (the far terrain shader declares only
	// these two).
	inline const VertexFormatDescription TerrainLowVertexFormat{
		.bindings = {
			{
				.binding = 0,
				.stride = sizeof(TerrainVertex),
				.inputRate = VertexInputRate::Vertex,
				.attributes = {
					{
						.semantic = VertexSemantic::Position,
						.format = Format::RGB32_Float,
						.offset = offsetof(TerrainVertex, pos)
					},
					{
						.semantic = VertexSemantic::Color,
						.format = Format::RGBA8_UNorm,
						.offset = offsetof(TerrainVertex, color)
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
		// Lightmap: atlas chunk index (cooked with --lightmap; the lobe
		// count rides the gfx draw ring (GfxDraw.misc.w) to the lit
		// fragment). 0xFFFFFFFF = none.
		uint32_t lmChunk = 0xFFFFFFFFu;
		// Lightmap chart atlas transforms, parallel to the cook's chart
		// indices (one float4 per chart: xy = scale, zw = offset, so
		// atlasUv = chartUv * xy + zw). Empty when the mesh carries no
		// lightmap. The runtime concatenates every mesh's transforms into
		// one GPU buffer (sorted by mesh, then chart) and writes each
		// draw's base offset into the draw ring (GfxDraw chart offset);
		// the vertex shader fetches xform[chartOffset + chartId].
		std::vector<glm::vec4> lmChartXforms;
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
		, SkinnedVertexFormat
		, DebugLine
		, TerrainVertexFormat
		, TerrainLowVertexFormat
	};

	// Returns the static description for the given format (no per-call
	// copies: BindRenderPipeline runs this on every bind).
	const VertexFormatDescription& GetVertexFormatDescription(VertexFormat format);



} // namespace smvk