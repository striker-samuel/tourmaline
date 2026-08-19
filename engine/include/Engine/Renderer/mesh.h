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
				.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
				.attributes = {
					{
						.semantic = VertexSemantic::Position,
						.format = VK_FORMAT_R32G32B32_SFLOAT,
						.offset = offsetof(VertexPos, pos)
					}
				}
			},
			{
				.binding = 1,
				.stride = sizeof(VertexShading),
				.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
				.attributes = {
					{
						.semantic = VertexSemantic::Color,
						.format = VK_FORMAT_R32G32B32_SFLOAT,
						.offset = offsetof(VertexShading, color)
					},
					{
						.semantic = VertexSemantic::TexCoord0,
						.format = VK_FORMAT_R32G32_SFLOAT,
						.offset = offsetof(VertexShading, texCoord)
					},
					{
						.semantic = VertexSemantic::Normal,
						.format = VK_FORMAT_R32G32B32_SFLOAT,
						.offset = offsetof(VertexShading, normal)
					},
					{
						.semantic = VertexSemantic::Tangent,
						.format = VK_FORMAT_R32G32B32A32_SFLOAT,
						.offset = offsetof(VertexShading, tangent)
					}
				}
			}
		}
	};

	enum struct VertexFormat {
		Empty
		, StaticVertexFormat
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
		struct Allocation {
			VmaVirtualAllocation vertexAllocation = VK_NULL_HANDLE;
			VmaVirtualAllocation indexAllocation = VK_NULL_HANDLE;
		} allocation;
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
	void DeallocateStaticVertexFmt(smvk::VulkanResourceManager& ctx);
	void BindStaticVertexFmtBuffers(VulkanResourceManager& ctx, VkCommandBuffer cmd);

} // namespace smvk