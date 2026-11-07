// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// Static-only SDF reflection residency for the SSR compute trace.
// TSDF mesh-local sidecars live in Cluster bake payloads; the upload
// stage packs unique volume samples and fixed mesh instance transforms
// into GPU storage buffers addressed by BDA. No dynamic/skinned meshes.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <Engine/RHI/resource_manager.h>

namespace tour {
struct World;
}

namespace smvk {

// Matches engine/shaders/static_sdf_trace.hlsli StaticSdfGpuInstance.
// Explicit float4 inverse rows avoid cross-compiler matrix packing.
// meta: x=resolution, y=packed-word offset, z=signed, w=maxDistance bits.
struct StaticSdfGpuInstance {
    glm::vec4 inverseRow0{0.0f};
    glm::vec4 inverseRow1{0.0f};
    glm::vec4 inverseRow2{0.0f};
    glm::vec4 originVoxel{0.0f}; // xyz=voxel zero CENTER, w=voxel edge
    glm::vec4 surface{0.0f};     // rgb=base color factor, w=roughness
    glm::uvec4 meta{0u};
};
static_assert(sizeof(StaticSdfGpuInstance) == 96);
static_assert(offsetof(StaticSdfGpuInstance, surface) == 64);
static_assert(offsetof(StaticSdfGpuInstance, meta) == 80);

// Matches StaticSdfSceneHeader in static_sdf_trace.hlsli (32 B).
// Publishing the descriptor through one BDA keeps the SSR push
// constants within Vulkan's guaranteed 128-byte minimum.
struct StaticSdfSceneHeader {
    uint64_t instancesAddress = 0;
    uint64_t samplesAddress = 0;
    uint32_t instanceCount = 0;
    uint32_t maxSteps = 64;
    float epsilon = 0.02f;
    float originBias = 0.05f;
};
static_assert(sizeof(StaticSdfSceneHeader) == 32);
static_assert(offsetof(StaticSdfSceneHeader, instanceCount) == 16);

inline constexpr uint32_t kStaticSdfMaxInstances = 128;
inline constexpr uint32_t kStaticSdfMaxSampleWords = 2u * 1024u * 1024u;

// Pure CPU packer for the shader's x-fastest R16_SNORM word layout.
// Low 16 bits = even sample; high 16 bits = odd sample (zero-padded).
// Kept separately testable without a GPU.
std::vector<uint32_t> PackStaticSdfWords(std::span<const int16_t> samples);

// Instances and samples are uploaded once for a frozen world topology,
// then retained until detach/scene reload. Static meshes must not move
// after publication; editors can call Invalidate() to republish an
// explicitly edited frozen scene. Failed/missing uploads degrade to SSR.
class StaticSdfGpuScene {
  public:
    bool Update(ResourceManager& ctx, const ::tour::World& world, const glm::vec3& cameraWorldPos);
    void Invalidate() {
        m_worldRevision = std::numeric_limits<uint64_t>::max();
    }
    void Clear();

    uint32_t InstanceCount() const {
        return m_instanceCount;
    }
    uint64_t Generation() const {
        return m_generation;
    }
    uint64_t SceneAddress(ResourceManager& ctx) const;
    bool Ready() const {
        return m_instanceCount != 0 && !m_header.empty() && !m_instances.empty() &&
               !m_samples.empty();
    }

  private:
    Holder<BufferHandle> m_header;
    Holder<BufferHandle> m_instances;
    Holder<BufferHandle> m_samples;
    uint32_t m_instanceCount = 0;
    uint64_t m_generation = 0; // republished static SDF scene generation
    const ::tour::World* m_world = nullptr;
    uint64_t m_worldRevision = std::numeric_limits<uint64_t>::max();
    glm::vec3 m_cachedEye{0.0f};
    bool m_cachedEyeValid = false;
};

} // namespace smvk
