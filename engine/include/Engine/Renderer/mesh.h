#pragma once

#include <span>
#include <vector>
#include <optional>

#include <Engine/Shared/common.h>
#include "volk.h"
#include "vk_mem_alloc.h"

namespace smvk {

    struct Material {
        struct Values {
            glm::vec4 ambient = glm::vec4(0.06f);
            glm::vec4 diffuse = glm::vec4(1.0f);

            // diffuse, ambient, alpha, normals
            int tex[4] = { -1, -1, -1, -1 };
        } values;

        static_assert(sizeof(Values) % 16 == 0);
    };


    enum class PrimitiveTopology {
        PointList,
        LineList,
        LineStrip,
        TriangleList,
        TriangleStrip,
        TriangleFan,
    };

    enum class VertexSemantic {
        Position,
        Normal,
        Tangent,
        Color,
        TexCoord0,
        TexCoord1,
        JointIndices,
        JointWeights,
    };

    struct VertexAttribute {
        VertexSemantic semantic;
        VkFormat format;
        uint32_t offset;
    };

    struct VertexBinding {
        uint32_t binding;
        uint32_t stride;
        VkVertexInputRate inputRate;

        std::vector<VertexAttribute> attributes;
    };

    struct VertexFormat {
        std::vector<VertexBinding> bindings;
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


    inline const VertexFormat StaticVertexFormat{
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
            VmaVirtualAllocation vertex = VK_NULL_HANDLE;
            VmaVirtualAllocation index = VK_NULL_HANDLE;
        } allocation;
    };


    // -----------------------------------------------------------------------------
    // Upload structure
    // -----------------------------------------------------------------------------

    struct StaticMeshUpload {
        std::vector<StaticVertex> vertices;
        std::vector<uint32_t> indices;

        glm::vec3 aabbMin{};
        glm::vec3 aabbMax{};
    };

    std::vector<VkVertexInputBindingDescription> getVkVertexInputBindingDescriptions(const VertexFormat& format);
    std::vector<VkVertexInputAttributeDescription> getVkVertexAttributeDescriptions(const VertexFormat& format);

} // namespace smvk