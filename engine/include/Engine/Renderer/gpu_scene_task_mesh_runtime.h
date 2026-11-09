// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
// Flight-safe GPU Scene task/mesh resources. No GPU readback and no
// 1024-meshlet-per-submesh gate; bounded by the actual 1M task group limit.
#include <Engine/Renderer/gpu_scene_task_mesh_abi.h>
#include <Engine/Renderer/gpu_scene_validation.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace tour::gpu_scene {

struct TaskMeshGroupCountPush {
    uint64_t work = 0, sceneOffsets = 0, meshes = 0;
    uint64_t aliases = 0, geometry = 0, counts = 0;
    uint32_t sceneCandidateCount = 0, workCapacity = 0, meshCount = 0;
    uint32_t aliasCount = 0, geometryCount = 0, clusterCount = 0;
};
struct TaskMeshGroupScatterPush {
    uint64_t work = 0, taskOffsets = 0, sceneOffsets = 0;
    uint64_t meshes = 0, groups = 0;
    uint32_t sceneCandidateCount = 0, workCapacity = 0;
    uint32_t taskGroupCapacity = 0, meshCount = 0;
};
struct TaskMeshDispatchPush {
    uint64_t taskOffsets = 0, arguments = 0;
    uint32_t workCapacity = 0, taskGroupCapacity = 0;
    uint32_t maxX = 0, maxY = 0;
};
static_assert(sizeof(TaskMeshGroupCountPush) == 72);
static_assert(sizeof(TaskMeshGroupScatterPush) == 56);
static_assert(sizeof(TaskMeshDispatchPush) == 32);

struct TaskMeshInputs {
    uint64_t work = 0, sceneOffsets = 0, meshes = 0, aliases = 0;
    uint64_t geometry = 0, clusters = 0, instances = 0;
    uint64_t cullView = 0, rasterView = 0;
    uint64_t indices = 0, positions = 0, shading = 0;
    uint32_t sceneCandidateCount = 0, workCapacity = 0;
    uint32_t meshCount = 0, aliasCount = 0, geometryCount = 0;
    uint32_t clusterCount = 0, instanceCount = 0, indexBase = 0;
    uint32_t indexWordCapacity = 0, vertexCapacity = 0;
    uint32_t worstMeshlets = 0;
    uint32_t coneEnabled = 1;
};
struct TaskMeshDispatchPlan {
    TaskMeshGroupCountPush count{};
    ParallelWorkBlockPush scanBlock{};
    ParallelWorkGroupPush scanGroups{};
    ParallelWorkAddPush scanAdd{};
    TaskMeshGroupScatterPush scatter{};
    TaskMeshDispatchPush indirect{};
    TaskMeshGpuPush graphics{};
    uint32_t workCapacity = 0;
    uint32_t taskGroupCapacity = 0;
    uint32_t scanBlocks = 0;
    smvk::BufferHandle indirectHandle{};
    smvk::LogicalBufferHandle indirectLogical{};
    smvk::LogicalBufferHandle groupLogical{};
    smvk::LogicalBufferHandle offsetLogical{};
};

class TaskMeshRuntime {
public:
    static constexpr uint32_t Flights = smvk::limits::MaxFramesInFlight;
    static constexpr uint32_t MaxTaskGroups = 1048576u;
    static constexpr uint32_t TaskGridX = 1024u;
    struct Flight {
        smvk::Holder<smvk::BufferHandle> counts, offsets, blockSums, blockBases;
        smvk::Holder<smvk::BufferHandle> groups, indirect, params;
        size_t countsBytes = 0, offsetsBytes = 0, sumsBytes = 0;
        size_t basesBytes = 0, groupsBytes = 0, indirectBytes = 0, paramsBytes = 0;
        smvk::LogicalBufferHandle groupsLogical{}, indirectLogical{}, offsetsLogical{};
        smvk::BufferHandle groupsBacking{}, indirectBacking{}, offsetsBacking{};
    };
    std::array<Flight, Flights> flights{};

    void Destroy(smvk::RenderGraph& rg) {
        for (auto& f : flights) {
            if (!f.groupsLogical.empty()) rg.FreeBuffer(f.groupsLogical);
            if (!f.indirectLogical.empty()) rg.FreeBuffer(f.indirectLogical);
            if (!f.offsetsLogical.empty()) rg.FreeBuffer(f.offsetsLogical);
            f = Flight{};
        }
    }

    [[nodiscard]] bool Prepare(smvk::ResourceManager& ctx,
                               smvk::RenderGraph& rg,
                               uint32_t flight,
                               const TaskMeshInputs& in,
                               TaskMeshDispatchPlan& out) {
        if (flight >= Flights || !in.work || !in.sceneOffsets || !in.meshes ||
            !in.aliases || !in.geometry || !in.clusters || !in.instances ||
            !in.cullView || !in.rasterView || !in.indices || !in.positions ||
            !in.shading || !in.workCapacity || !in.worstMeshlets ||
            !in.meshCount || !in.aliasCount || !in.geometryCount ||
            !in.instanceCount || !in.indexWordCapacity || !in.vertexCapacity ||
            in.workCapacity > 1048576u)
            return false;
        const uint64_t groupsPerWork =
            (uint64_t(in.worstMeshlets) + 31u) / 32u;
        const uint64_t upperBound = uint64_t(in.workCapacity) * groupsPerWork;
        if (upperBound == 0 || upperBound > MaxTaskGroups)
            return false;
        const uint32_t groupCapacity = uint32_t(upperBound);
        const uint32_t blocks = (in.workCapacity + 255u) / 256u;
        Flight& f = flights[flight];
        const auto ensure = [&](smvk::Holder<smvk::BufferHandle>& h,
                                size_t& capacity, size_t bytes,
                                const char* name) -> bool {
            if (!h.empty() && capacity >= bytes)
                return true;
            auto next = ctx.CreateBuffer(bytes, smvk::BufferType_Storage, name);
            if (next.empty())
                return false;
            h = std::move(next);
            capacity = bytes;
            return true;
        };
        if (!ensure(f.counts, f.countsBytes, size_t(in.workCapacity) * 4u, "Task mesh counts") ||
            !ensure(f.offsets, f.offsetsBytes, size_t(in.workCapacity + 1u) * 4u, "Task mesh offsets") ||
            !ensure(f.blockSums, f.sumsBytes, size_t(blocks) * 4u, "Task mesh block sums") ||
            !ensure(f.blockBases, f.basesBytes, size_t(blocks) * 4u, "Task mesh block bases") ||
            !ensure(f.groups, f.groupsBytes, size_t(groupCapacity) * sizeof(TaskMeshGpuGroup),
                    "Task mesh groups") ||
            !ensure(f.indirect, f.indirectBytes, 4u * sizeof(TaskMeshIndirectCommand),
                    "Task mesh indirect") ||
            !ensure(f.params, f.paramsBytes, sizeof(TaskMeshGpuParams),
                    "Task mesh parameters"))
            return false;

        const uint64_t counts = ctx.GetBufferDeviceAddress(f.counts);
        const uint64_t offsets = ctx.GetBufferDeviceAddress(f.offsets);
        const uint64_t sums = ctx.GetBufferDeviceAddress(f.blockSums);
        const uint64_t bases = ctx.GetBufferDeviceAddress(f.blockBases);
        const uint64_t groups = ctx.GetBufferDeviceAddress(f.groups);
        const uint64_t indirect = ctx.GetBufferDeviceAddress(f.indirect);
        const uint64_t params = ctx.GetBufferDeviceAddress(f.params);
        if (!counts || !offsets || !sums || !bases || !groups || !indirect || !params)
            return false;

        if (f.groupsBacking != smvk::BufferHandle(f.groups)) {
            if (!f.groupsLogical.empty())
                rg.FreeBuffer(f.groupsLogical);
            f.groupsLogical = rg.AddExternalBuffer(f.groups, "GpuSceneTaskMeshGroups");
            f.groupsBacking = smvk::BufferHandle(f.groups);
        }
        if (f.indirectBacking != smvk::BufferHandle(f.indirect)) {
            if (!f.indirectLogical.empty())
                rg.FreeBuffer(f.indirectLogical);
            f.indirectLogical = rg.AddExternalBuffer(f.indirect, "GpuSceneTaskMeshIndirect");
            f.indirectBacking = smvk::BufferHandle(f.indirect);
        }
        if (f.offsetsBacking != smvk::BufferHandle(f.offsets)) {
            if (!f.offsetsLogical.empty())
                rg.FreeBuffer(f.offsetsLogical);
            f.offsetsLogical = rg.AddExternalBuffer(f.offsets, "GpuSceneTaskMeshOffsets");
            f.offsetsBacking = smvk::BufferHandle(f.offsets);
        }
        if (f.groupsLogical.empty() || f.indirectLogical.empty() ||
            f.offsetsLogical.empty())
            return false;

        TaskMeshGpuParams p{
            groups, in.work, in.clusters, in.instances, in.cullView,
            in.rasterView, in.geometry, in.aliases, in.indices,
            in.positions, in.shading, offsets,
            in.workCapacity, in.instanceCount, in.clusterCount, in.geometryCount,
            in.aliasCount, in.indexBase, in.indexWordCapacity, in.vertexCapacity,
            1u, in.coneEnabled, in.workCapacity, 0u
        };
        ctx.UploadDataToBuffer(f.params, &p, sizeof(p),
            smvk::PipelineStage(smvk::PipelineStageBit_Task |
                                smvk::PipelineStageBit_Mesh),
            smvk::AccessFlags::ShaderRead);

        TaskMeshDispatchPlan next{};
        next.count = {in.work, in.sceneOffsets, in.meshes, in.aliases, in.geometry,
                      counts, in.sceneCandidateCount, in.workCapacity,
                      in.meshCount, in.aliasCount, in.geometryCount, in.clusterCount};
        next.scanBlock = {counts, offsets, sums, in.workCapacity, blocks};
        next.scanGroups = {sums, bases, blocks, 0u};
        next.scanAdd = {offsets, sums, bases, in.workCapacity, blocks};
        next.scatter = {in.work, offsets, in.sceneOffsets, in.meshes, groups,
                        in.sceneCandidateCount, in.workCapacity, groupCapacity,
                        in.meshCount};
        next.indirect = {offsets, indirect, in.workCapacity, groupCapacity,
                         TaskGridX, 1024u};
        next.graphics = {params, 0u, groupCapacity, 0u, TaskGridX};
        next.workCapacity = in.workCapacity;
        next.taskGroupCapacity = groupCapacity;
        next.scanBlocks = blocks;
        next.indirectHandle = f.indirect;
        next.indirectLogical = f.indirectLogical;
        next.groupLogical = f.groupsLogical;
        next.offsetLogical = f.offsetsLogical;
        out = next;
        return true;
    }
};
} // namespace tour::gpu_scene
