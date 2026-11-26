// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Renderer/static_sdf_gpu.h>

#include <Engine/App/world.h>
#include <Engine/Asset/sdf.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace smvk {
namespace {

bool ValidTransform(const glm::mat4& m) {
    for (uint32_t column = 0; column < 4; ++column)
        for (uint32_t row = 0; row < 4; ++row)
            if (!std::isfinite(m[column][row]))
                return false;
    if (std::abs(m[0][3]) > 1e-5f || std::abs(m[1][3]) > 1e-5f || std::abs(m[2][3]) > 1e-5f ||
        std::abs(m[3][3] - 1.0f) > 1e-5f)
        return false;
    const float det = glm::determinant(glm::mat3(m));
    return std::isfinite(det) && std::abs(det) > 1e-10f;
}

bool ValidVolume(const ::tour::asset::SdfVolume& v) {
    const size_t r = v.resolution;
    return r >= 8 && r <= 128 && v.samples.size() == r * r * r && std::isfinite(v.voxelSize) &&
           v.voxelSize > 0.0f && std::isfinite(v.maxDistance) && v.maxDistance > 0.0f &&
           std::isfinite(v.origin.x) && std::isfinite(v.origin.y) && std::isfinite(v.origin.z);
}

} // namespace

std::vector<uint32_t> PackStaticSdfWords(std::span<const int16_t> samples) {
    std::vector<uint32_t> packed;
    packed.reserve((samples.size() + 1) / 2);
    for (size_t i = 0; i < samples.size(); i += 2) {
        const uint32_t lo = uint16_t(samples[i]);
        const uint32_t hi =
            (i + 1 < samples.size()) ? uint32_t(uint16_t(samples[i + 1])) << 16 : 0u;
        packed.push_back(lo | hi);
    }
    return packed;
}

void StaticSdfGpuScene::Clear() {
    ++m_generation; // old temporal reflection history is invalid
    m_header = {};
    m_instances = {};
    m_nodes = {};
    m_children = {};
    m_leaves = {};
    m_materials = {};
    m_instanceCount = 0;
    m_world = nullptr;
    m_worldRevision = std::numeric_limits<uint64_t>::max();
    m_cachedEyeValid = false;
}

uint64_t StaticSdfGpuScene::SceneAddress(ResourceManager& ctx) const {
    return Ready() ? ctx.GetBufferDeviceAddress(m_header) : 0;
}

bool StaticSdfGpuScene::Update(ResourceManager& ctx,
                               const ::tour::World& world,
                               const glm::vec3& cameraWorldPos) {
    // Camera-centric mode keeps a bounded set near the camera (8 m republish
    // cell); whole-scene mode (DDGI) republishes on world topology only.
    const bool eyeFinite = std::isfinite(cameraWorldPos.x) && std::isfinite(cameraWorldPos.y) &&
                           std::isfinite(cameraWorldPos.z);
    if (m_world == &world && m_worldRevision == world.gpuSceneTopologyRevision) {
        if (m_wholeScene ||
            (m_cachedEyeValid && eyeFinite && glm::distance(m_cachedEye, cameraWorldPos) < 8.0f))
            return Ready();
    }

    // De-duplicate SDF grids across instanced static meshes. Each sidecar
    // groups all base primitives in one .geometry family; fragment names
    // match the suffix of the cooked mesh URI (before optional /lodN).
    std::unordered_map<std::string, ::tour::asset::SdfAsset> families;
    struct OctreeOffsets {
        uint32_t node = 0, child = 0, leaf = 0;
    };
    std::unordered_map<std::string, OctreeOffsets> octreeOffsets;
    std::vector<StaticSdfGpuInstance> instances;
    std::vector<uint32_t> nodes;
    std::vector<int32_t> children;
    std::vector<uint32_t> leaves;
    std::vector<uint32_t> materials; // concatenated packed-material palettes
    std::unordered_map<std::string, uint32_t> familyMaterialBase;
    instances.reserve(std::min(world.drawItems.size(), size_t(kStaticSdfMaxInstances)));

    // Deterministic closest-first upload order (camera-centric). Whole-scene
    // mode keeps drawItems order (every static opaque instance, not the
    // camera-closest set) so DDGI probes far from the camera trace the real
    // scene. Selecting from a scene's material-sorted draw list would fill
    // the camera-centric cap with invisible distant geometry before any
    // nearby reflective object is reached.
    std::vector<std::pair<float, const ::tour::World::RenderItem*>> candidates;
    for (const auto& item : world.drawItems) {
        if ((item.flags & (::tour::IteratorFlagBit_Static | ::tour::IteratorFlagBit_Opaque |
                           ::tour::IteratorFlagBit_StaticVertexFmt)) !=
                (::tour::IteratorFlagBit_Static | ::tour::IteratorFlagBit_Opaque |
                 ::tour::IteratorFlagBit_StaticVertexFmt) ||
            (item.flags & ::tour::IteratorFlagBit_Masked) != 0)
            continue;
        if (item.node >= world.scene.globalTransform.size())
            continue;
        const StaticMesh* mesh = world.GetMesh(item.mesh);
        if (!mesh || mesh->skinned)
            continue;
        float distance2 = 0.0f;
        if (!m_wholeScene) {
            const glm::vec3 localCenter = (mesh->geometry.aabbMin + mesh->geometry.aabbMax) * 0.5f;
            const glm::vec3 worldCenter =
                glm::vec3(world.scene.globalTransform[item.node] * glm::vec4(localCenter, 1.0f));
            distance2 = glm::dot(worldCenter - cameraWorldPos, worldCenter - cameraWorldPos);
            if (!std::isfinite(distance2))
                distance2 = std::numeric_limits<float>::max();
        }
        candidates.emplace_back(distance2, &item);
    }
    if (!m_wholeScene)
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first)
                return a.first < b.first;
            return a.second->node < b.second->node;
        });
    for (const auto& [distance2, itemPtr] : candidates) {
        if (instances.size() >= kStaticSdfMaxInstances)
            break;
        const auto& item = *itemPtr;
        if (item.node >= world.scene.globalTransform.size() ||
            item.mesh >= world.meshLocations.size())
            continue;
        const StaticMesh* mesh = world.GetMesh(item.mesh);
        if (!mesh || mesh->skinned)
            continue;

        const auto& loc = world.meshLocations[item.mesh];
        if (!loc.cluster)
            continue;
        const std::string uri = loc.cluster->GetMeshUri(loc.handle).Str();
        const size_t hash = uri.find('#');
        if (hash == std::string::npos || hash + 1 >= uri.size())
            continue;
        const std::string family = uri.substr(0, hash);
        std::string fragment = uri.substr(hash + 1);
        // LOD siblings may use a /lodN URI suffix, but the v1 TSDF
        // sidecar is keyed by the base primitive fragment only.
        const size_t lodSuffix = fragment.rfind("/lod");
        if (lodSuffix != std::string::npos && lodSuffix + 4 < fragment.size() &&
            std::all_of(fragment.begin() + lodSuffix + 4, fragment.end(), [](char c) {
                return c >= '0' && c <= '9';
            }))
            fragment.resize(lodSuffix);
        auto familyIt = families.find(family);
        if (familyIt == families.end()) {
            ::tour::asset::SdfAsset decoded;
            std::string error;
            const auto bytes = world.ReadBakeBytes(family + ".sdf");
            if (!bytes.empty() &&
                !::tour::asset::ParseSdfAsset(bytes.data(), bytes.size(), decoded, error))
                std::fprintf(stderr,
                             "[StaticSdf] invalid sidecar '%s.sdf': %s\n",
                             family.c_str(),
                             error.c_str());
            familyIt = families.emplace(family, std::move(decoded)).first;
            // Merge the family's packed-material palette into the global
            // palette and remember its base offset. A v1 sidecar (no
            // palette) contributes a single neutral entry.
            const uint32_t base = uint32_t(materials.size());
            familyMaterialBase.emplace(family, base);
            if (familyIt->second.materials.empty())
                materials.push_back(
                    ::tour::asset::PackSdfMaterial(glm::vec3(0.7f), 0.5f, 0.0f, 0.0f));
            else
                materials.insert(materials.end(),
                                 familyIt->second.materials.begin(),
                                 familyIt->second.materials.end());
        }
        const auto it =
            std::find_if(familyIt->second.meshes.begin(),
                         familyIt->second.meshes.end(),
                         [&](const ::tour::asset::SdfMesh& m) { return m.fragment == fragment; });
        if (it == familyIt->second.meshes.end() || it->octree.nodes.empty())
            continue;
        const glm::mat4& model = world.scene.globalTransform[item.node];
        if (!ValidTransform(model))
            continue;

        // Pack the octree (nodes/children/leaves) once per fragment.
        OctreeOffsets off{};
        if (const auto found = octreeOffsets.find(uri); found != octreeOffsets.end()) {
            off = found->second;
        } else {
            const auto& o = it->octree;
            if (o.nodes.size() > kStaticSdfMaxNodeWords - nodes.size() ||
                o.children.size() > kStaticSdfMaxChildWords - children.size() ||
                o.leaves.size() > kStaticSdfMaxLeafWords - leaves.size())
                continue;
            off.node = uint32_t(nodes.size());
            off.child = uint32_t(children.size());
            off.leaf = uint32_t(leaves.size());
            nodes.insert(nodes.end(), o.nodes.begin(), o.nodes.end());
            children.insert(children.end(), o.children.begin(), o.children.end());
            leaves.insert(leaves.end(), o.leaves.begin(), o.leaves.end());
            octreeOffsets.emplace(uri, off);
        }

        const glm::mat4 inverse = glm::inverse(model);
        StaticSdfGpuInstance gpu{};
        gpu.inverseRow0 = {inverse[0][0], inverse[1][0], inverse[2][0], inverse[3][0]};
        gpu.inverseRow1 = {inverse[0][1], inverse[1][1], inverse[2][1], inverse[3][1]};
        gpu.inverseRow2 = {inverse[0][2], inverse[1][2], inverse[2][2], inverse[3][2]};
        const auto& o = it->octree;
        gpu.originSize = glm::vec4(o.origin, o.size);
        const uint32_t matBase = familyMaterialBase[family] + it->materialIndex;
        gpu.material = glm::uvec4(matBase, o.maxDepth, 0u, 0u);
        gpu.meta = {off.node, off.child, off.leaf, std::bit_cast<uint32_t>(o.maxDistance)};
        instances.push_back(gpu);
    }

    // A failed build must not leave stale buffers from a previous world.
    Holder<BufferHandle> headerBuffer, instanceBuffer, nodeBuffer, childBuffer, leafBuffer,
        materialBuffer;
    if (!instances.empty()) {
        instanceBuffer = ctx.CreateBuffer(instances.size() * sizeof(StaticSdfGpuInstance),
                                          BufferType_Storage,
                                          "Static SDF reflection instances");
        nodeBuffer = ctx.CreateBuffer(std::max(nodes.size(), size_t(1)) * sizeof(uint32_t),
                                      BufferType_Storage,
                                      "Static SDF reflection nodes");
        childBuffer = ctx.CreateBuffer(std::max(children.size(), size_t(1)) * sizeof(uint32_t),
                                       BufferType_Storage,
                                       "Static SDF reflection children");
        leafBuffer = ctx.CreateBuffer(std::max(leaves.size(), size_t(1)) * sizeof(uint32_t),
                                      BufferType_Storage,
                                      "Static SDF reflection leaves");
        const uint32_t matCount = uint32_t(materials.size());
        materialBuffer = ctx.CreateBuffer(std::max(matCount, 1u) * sizeof(uint32_t),
                                          BufferType_Storage,
                                          "Static SDF reflection materials");
        headerBuffer = ctx.CreateBuffer(
            sizeof(StaticSdfSceneHeader), BufferType_Storage, "Static SDF reflection descriptor");
        if (instanceBuffer.empty() || nodeBuffer.empty() || childBuffer.empty() ||
            leafBuffer.empty() || materialBuffer.empty() || headerBuffer.empty()) {
            Clear();
            m_world = &world;
            m_worldRevision = world.gpuSceneTopologyRevision;
            m_cachedEye = cameraWorldPos;
            m_cachedEyeValid = eyeFinite;
            return false;
        }
        ctx.UploadDataToBuffer(instanceBuffer,
                               instances.data(),
                               instances.size() * sizeof(StaticSdfGpuInstance),
                               PipelineStageBit_Compute,
                               AccessFlags::ShaderRead);
        const uint32_t zero = 0u;
        const uint32_t* nodeData = nodes.empty() ? &zero : nodes.data();
        ctx.UploadDataToBuffer(nodeBuffer,
                               nodeData,
                               std::max(nodes.size(), size_t(1)) * sizeof(uint32_t),
                               PipelineStageBit_Compute,
                               AccessFlags::ShaderRead);
        const int32_t zeroChild = -1;
        const int32_t* childData = children.empty() ? &zeroChild : children.data();
        ctx.UploadDataToBuffer(childBuffer,
                               childData,
                               std::max(children.size(), size_t(1)) * sizeof(uint32_t),
                               PipelineStageBit_Compute,
                               AccessFlags::ShaderRead);
        const uint32_t* leafData = leaves.empty() ? &zero : leaves.data();
        ctx.UploadDataToBuffer(leafBuffer,
                               leafData,
                               std::max(leaves.size(), size_t(1)) * sizeof(uint32_t),
                               PipelineStageBit_Compute,
                               AccessFlags::ShaderRead);
        const uint32_t neutral = ::tour::asset::PackSdfMaterial(glm::vec3(0.7f), 0.5f, 0.0f, 0.0f);
        ctx.UploadDataToBuffer(materialBuffer,
                               matCount ? materials.data() : &neutral,
                               std::max(matCount, 1u) * sizeof(uint32_t),
                               PipelineStageBit_Compute,
                               AccessFlags::ShaderRead);
        StaticSdfSceneHeader gpuHeader{};
        gpuHeader.instancesAddress = ctx.GetBufferDeviceAddress(instanceBuffer);
        gpuHeader.nodesAddress = ctx.GetBufferDeviceAddress(nodeBuffer);
        gpuHeader.childrenAddress = ctx.GetBufferDeviceAddress(childBuffer);
        gpuHeader.leavesAddress = ctx.GetBufferDeviceAddress(leafBuffer);
        gpuHeader.materialsAddress = ctx.GetBufferDeviceAddress(materialBuffer);
        if (gpuHeader.instancesAddress == 0 || gpuHeader.nodesAddress == 0 ||
            gpuHeader.childrenAddress == 0 || gpuHeader.leavesAddress == 0 ||
            gpuHeader.materialsAddress == 0) {
            Clear();
            m_world = &world;
            m_worldRevision = world.gpuSceneTopologyRevision;
            m_cachedEye = cameraWorldPos;
            m_cachedEyeValid = eyeFinite;
            return false;
        }
        gpuHeader.instanceCount = uint32_t(instances.size());
        gpuHeader.materialCount = matCount;
        ctx.UploadDataToBuffer(headerBuffer,
                               &gpuHeader,
                               sizeof(gpuHeader),
                               PipelineStageBit_Compute,
                               AccessFlags::ShaderRead);
    }
    m_header = std::move(headerBuffer);
    m_instances = std::move(instanceBuffer);
    m_nodes = std::move(nodeBuffer);
    m_children = std::move(childBuffer);
    m_leaves = std::move(leafBuffer);
    m_materials = std::move(materialBuffer);
    m_instanceCount = uint32_t(instances.size());
    ++m_generation;
    m_world = &world;
    m_worldRevision = world.gpuSceneTopologyRevision;
    m_cachedEye = cameraWorldPos;
    m_cachedEyeValid = eyeFinite;
    if (m_instanceCount)
        std::fprintf(stderr,
                     "[StaticSdf] GPU resident: %u frozen opaque instances, %zu octree nodes, "
                     "%zu leaves (%.2f MiB)\n",
                     m_instanceCount,
                     nodes.size(),
                     leaves.size(),
                     double((nodes.size() + children.size() + leaves.size()) * sizeof(uint32_t)) /
                         (1024.0 * 1024.0));
    return Ready();
}

} // namespace smvk
