#pragma once

#include <span>
#include <Engine/Shared/common.h>
#include "volk.h"

namespace smvk {

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


    // -------------------------------------------------------------------------
    // CPU/GPU vertex stream types
    // -------------------------------------------------------------------------

    // Vertex buffer binding 0
    struct VertexPos {
        glm::vec3 pos;
    };

    // Vertex buffer binding 1
    struct VertexShading {
        glm::vec3 color;
        glm::vec2 texCoord;
        glm::vec3 normal;
        glm::vec4 tangent;
    };


    // The actual format used by a normal static mesh.
    //
    // Binding 0:
    //     position
    //
    // Binding 1:
    //     color
    //     texCoord
    //     normal
    //     tangent
    //
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


    // -------------------------------------------------------------------------
    // Geometry
    // -------------------------------------------------------------------------

    struct VertexBuffer {
        Buffer buffer;
        uint32_t binding;
    };
    struct IndexBuffer {
        Buffer buffer;

        VkDeviceSize offset = 0;
        uint32_t indexCount = 0;
        VkIndexType indexType = VK_INDEX_TYPE_UINT32;
    };

    struct Surface {
        uint32_t firstIndex;
        uint32_t indexCount;
        int32_t baseVertex;
    };

    struct Geometry {
        VertexFormat format;

        std::vector<VertexBuffer> vertexBuffers;
        std::optional<IndexBuffer> indexBuffer;

        PrimitiveTopology topology;

        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;

        std::vector<Surface> surfaces;

        glm::vec3 aabbMin{};
        glm::vec3 aabbMax{};
        float sphereRadius = 0.0f;
    };


    // -------------------------------------------------------------------------
    // Mesh
    // -------------------------------------------------------------------------

    class Mesh {
    public:
        virtual ~Mesh() = default;

        virtual const VertexFormat& vertexFormat() const = 0;

        virtual PrimitiveTopology topology() const = 0;

        virtual std::span<const VertexBuffer> vertexBuffers() const = 0;

        virtual const Buffer* indexBuffer() const = 0;

        virtual uint32_t vertexCount() const = 0;
        virtual uint32_t indexCount() const = 0;
    };


    class StaticMesh : public Mesh {
    public:
        const VertexFormat& vertexFormat() const override {
            return m_geometry.format;
        }

        PrimitiveTopology topology() const override {
            return m_geometry.topology;
        }

        std::span<const VertexBuffer> vertexBuffers() const override {
            return m_geometry.vertexBuffers;
        }

        const Buffer* indexBuffer() const override {
            if (!m_geometry.indexBuffer)
                return nullptr;

            return &m_geometry.indexBuffer->buffer;
        }

        uint32_t vertexCount() const override {
            return m_geometry.vertexCount;
        }

        uint32_t indexCount() const override {
            return m_geometry.indexCount;
        }

        const Geometry& geometry() const {
            return m_geometry;
        }

    private:
        Geometry m_geometry;
    };
    //
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

    struct VertexPos {
        glm::vec3 pos;
    };

    struct VertexShading {
        glm::vec3 color;
        glm::vec2 texCoord;
        glm::vec3 normal;
        glm::vec4 tangent;
    };
};