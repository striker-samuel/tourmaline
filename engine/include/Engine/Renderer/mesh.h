#pragma once

#include <span>
#include <vector>
#include <optional>

#include <Engine/Shared/common.h> //glm
#include <Engine/smvk.h>

namespace smvk {

	struct alignas(16) MaterialValues {
		glm::vec4 ambient{ 0.06f };
		glm::vec4 diffuse{ 1.0f };
		glm::ivec4 textures{ -1 };
	};
	static_assert(sizeof(MaterialValues) % 16 == 0);

	struct Material {
		MaterialValues values;
		//ShaderProgram // RasterState
	};


	// -----------------------------------------------------------------------------
	// GPU vertex streams
	// -----------------------------------------------------------------------------

	struct VertexPos {
		glm::vec3 pos;
	};

	struct VertexShading {
		glm::vec3 color;
		glm::vec2 texCoord;
		glm::vec3 normal;
		glm::vec4 tangent;
	};


	// CPU-side import vertex
	struct StaticVertex {
		glm::vec3 pos;
		glm::vec3 color;
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
						.format = Format::RG32_Float,
						.offset = offsetof(VertexShading, color)
					},
					{
						.semantic = VertexSemantic::TexCoord0,
						.format = Format::RG32_Float,
						.offset = offsetof(VertexShading, texCoord)
					},
					{
						.semantic = VertexSemantic::Normal,
						.format = Format::RGB32_Float,
						.offset = offsetof(VertexShading, normal)
					},
					{
						.semantic = VertexSemantic::Tangent,
						.format = Format::RGBA32_Float,
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

	void AllocateStaticVertexFmt(smvk::VulkanResourceManager& ctx);

	enum struct VertexFormat {
		Empty
		, StaticVertexFormat
	};

	VertexFormatDescription GetVertexFormatDescription(VertexFormat format);



} // namespace smvk