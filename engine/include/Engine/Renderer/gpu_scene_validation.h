// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Opt-in GPU Scene visibility parity path. Production draws are unchanged.
// WorldRenderer calls BeginFlight only AFTER that flight's acquire fence.
// Validation uses a per-flight buffer and a blocking diagnostic readback
// when that slot is next reused. Never share ResourceManager's async ticket.

#include <Engine/Renderer/gpu_scene_candidates.h>
#include <Engine/Renderer/renderview.h>
#include <Engine/RHI/resource_manager.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace tour::gpu_scene {

// Existing RenderView's inward planes are ABSOLUTE float32-world planes.
// This adapter rebases the SAME planes to a separate relative-space view,
// without affecting raster, HZB, or the current RenderView layout.
// This cannot restore precision lost in the float32 scene/camera.
[[nodiscard]] inline RelativeFrustum LegacyRelativeFrustum(
    const smvk::RenderView& view, glm::dvec3 origin) {
    RelativeFrustum out{};
    out.originWorld = origin;
    for (int i = 0; i < 6; ++i) {
        const glm::dvec4 p(view.m_viewPlanes[i]);
        const glm::dvec3 n(p);
        if (glm::dot(n,n) < 1e-20) {
            // Reversed-Z infinite far: disabled half-space.
            out.planes[i] = glm::dvec4(0,0,0,1e30);
        } else {
            out.planes[i] = glm::dvec4(n, p.w + glm::dot(n, origin));
        }
    }
    return out;
}

// Genuine double VP variant: caller must construct worldToClip in double
// using authoritative double CPU transforms for true large-world culling.
// Extracted half-spaces use Vulkan [0,1] depth, reversed-Z safe.
[[nodiscard]] inline RelativeFrustum DoubleRelativeFrustum(
    const glm::dmat4& vp, glm::dvec3 origin) {
    const auto row = [&](int r) {
        return glm::dvec4(vp[0][r],vp[1][r],vp[2][r],vp[3][r]);
    };
    const glm::dvec4 a=row(0), b=row(1), c=row(2), d=row(3);
    const std::array<glm::dvec4,6> raw{{
        d+a,d-a,d+b,d-b,c,d-c
    }};
    RelativeFrustum out{};
    out.originWorld = origin;
    for (size_t i=0;i<6;++i) {
        const glm::dvec3 n(raw[i]);
        const double len=glm::length(n);
        if (!(len>1e-16) || !std::isfinite(len)) {
            out.planes[i]=glm::dvec4(0,0,0,1e30);
        } else {
            const glm::dvec4 p=raw[i]/len;
            out.planes[i]=glm::dvec4(glm::dvec3(p),
                p.w+glm::dot(glm::dvec3(p),origin));
        }
    }
    return out;
}

// The future authoritative camera contract: projection and rotation are
// evaluated in double, with no large absolute translation in the matrix.
// The view origin is carried separately as dvec3 and split for the GPU.
// This avoids large-world cancellation in a conventional double VP too.
[[nodiscard]] inline RelativeFrustum AuthoritativeRelativeFrustum(
    const glm::dmat4& projection,
    const glm::dmat4& worldToViewRotation,
    const glm::dvec3& cameraWorld) {
    // Projection * rotation acts on (worldPosition - cameraWorld).
    // Pass a zero origin to the plane extractor because the matrix is
    // ALREADY relative. Only then attach the actual absolute origin.
    RelativeFrustum view = DoubleRelativeFrustum(
        projection * worldToViewRotation, glm::dvec3(0.0));
    view.originWorld = cameraWorld;
    return view;
}

[[nodiscard]] inline bool PackInstanceCullView(
    const RelativeFrustum& view, InstanceCullGpuView& out) {
    if (!view.Valid()) return false;
    out = {};
    out.origin = math::SplitDoubleFloat(view.originWorld);
    for (size_t i=0;i<6;++i)
        for (int j=0;j<4;++j) {
            const float v=float(view.planes[i][j]);
            if (!std::isfinite(v)) return false;
            out.inwardPlanes[i][j]=v;
        }
    return true;
}

// Invalid references fail closed; missing bounds pass visibility with
// bypass=1. This mirrors gpu_scene_instance_cull.cs.hlsl for debug parity.
[[nodiscard]] inline InstanceCullVerdict ReferenceCandidate(
    const RenderDatabase& db, uint32_t memberIndex,
    const RelativeFrustum& view) {
    InstanceCullVerdict result{};
    if (memberIndex >= db.leafMembers.Size()) return result;
    const auto& member=db.leafMembers.Rows()[memberIndex];
    if (member.identity.y != 1u) return result;
    const Handle<Instance> id{member.value.ref.x,member.value.ref.y};
    const Instance* inst=db.instances.Get(id);
    if (!inst) return result;
    bool bypass=true;
    if (inst->refs.x < db.meshes.Size()) {
        const auto& mesh=db.meshes.Rows()[inst->refs.x];
        bypass=mesh.identity.y != 1u || mesh.identity.x != inst->refs.y ||
            (mesh.value.flags & MeshFlagCompleteBounds)==0;
    }
    const bool visible=bypass || ReferenceInstanceVisible(db,id,view);
    result.value=glm::uvec4(id.slot,id.generation,
                              visible ? 1u : 0u,bypass ? 1u : 0u);
    return result;
}

// GPU Scene stable verdict compaction. Each stage is dispatched in order
// with an explicit compute memory barrier; BDA pointers alone do not
// establish RenderGraph dependencies.
struct CompactCountPush {
    uint64_t verdicts = 0, counts = 0;
    uint32_t candidateCount = 0, groupCount = 0;
};
struct CompactPrefixPush {
    uint64_t counts = 0, offsets = 0;
    uint32_t groupCount = 0, unused = 0;
};
struct CompactScatterPush {
    uint64_t verdicts = 0, offsets = 0, output = 0;
    uint32_t candidateCount = 0, groupCount = 0;
};
static_assert(sizeof(CompactCountPush) == 24);
static_assert(sizeof(CompactPrefixPush) == 24);
static_assert(sizeof(CompactScatterPush) == 32);

// Stable order is essential for future LOD/submesh expansion: atomics
// would return a nondeterministic permutation of visible instances.
[[nodiscard]] inline std::vector<InstanceCullVerdict> CompactVisibleVerdicts(
    std::span<const InstanceCullVerdict> verdicts) {
    std::vector<InstanceCullVerdict> out;
    out.reserve(verdicts.size());
    for (const auto& verdict : verdicts)
        if (verdict.value.x != InvalidIndex &&
            verdict.value.y != 0u && verdict.value.z != 0u)
            out.push_back(verdict);
    return out;
}

// One GPU work record per selected, registered submesh. These records
// are intermediate GPU-scene work, not Vulkan indirect draw commands.
struct alignas(16) SceneSubmeshWork {
    glm::uvec4 identity{InvalidIndex, 0u, InvalidIndex, InvalidIndex};
    glm::uvec4 detail{InvalidIndex, InvalidIndex, InvalidIndex, 0u};
};
static_assert(sizeof(SceneSubmeshWork) == 32);
struct WorkSelectPush {
    uint64_t compact = 0, compactOffsets = 0;
    uint64_t instances = 0, meshes = 0, lods = 0, submeshes = 0;
    uint64_t choices = 0, counts = 0;
    alignas(16) glm::uvec4 limits{0u};
    glm::uvec4 limits2{0u};
};
struct WorkPrefixPush {
    uint64_t counts = 0, offsets = 0;
    uint32_t candidateCount = 0, reserved = 0;
};
struct WorkScatterPush {
    uint64_t choices = 0, offsets = 0, submeshes = 0, output = 0;
    alignas(16) glm::uvec4 limits{0u};
};
static_assert(sizeof(WorkSelectPush) == 96);
static_assert(offsetof(WorkSelectPush, limits) == 64);
static_assert(sizeof(WorkPrefixPush) == 24);
static_assert(sizeof(WorkScatterPush) == 48);
static_assert(offsetof(WorkScatterPush, limits) == 32);

// CPU oracle: exactly mirrors the GPU's current finest-registered-resident
// LOD policy and preserves visible-instance/LOD-submesh order. This is not
// screen-space error LOD selection yet.
[[nodiscard]] inline std::vector<SceneSubmeshWork> ReferenceExpandWork(
    const RenderDatabase& db,
    std::span<const InstanceCullVerdict> compacted) {
    std::vector<SceneSubmeshWork> result;
    for (const auto& verdict : compacted) {
        if (verdict.value.x == InvalidIndex || verdict.value.y == 0u ||
            verdict.value.z == 0u)
            continue;
        const Instance* inst = db.instances.Get({
            verdict.value.x, verdict.value.y});
        if (!inst) continue;
        const Mesh* mesh = db.meshes.Get({
            inst->refs.x, inst->refs.y});
        if (!mesh || mesh->lodCount == 0u || mesh->lodCount > 16u ||
            mesh->firstLod >= db.lods.Size() ||
            mesh->lodCount > db.lods.Size() - mesh->firstLod)
            continue;

        for (uint32_t level = 0; level < mesh->lodCount; ++level) {
            if ((mesh->residentMask & (1u << level)) == 0u)
                continue;
            const auto& lodRow = db.lods.Rows()[mesh->firstLod + level];
            const Lod& lod = lodRow.value;
            if (lodRow.identity.y != 1u ||
                (lod.flags & (LodFlagResident | LodFlagFilterable)) !=
                    (LodFlagResident | LodFlagFilterable) ||
                lod.submeshCount == 0u || lod.submeshCount > 16u ||
                lod.firstSubmesh >= db.submeshes.Size() ||
                lod.submeshCount > db.submeshes.Size() - lod.firstSubmesh)
                continue;

            std::vector<SceneSubmeshWork> selected;
            for (uint32_t sub = 0; sub < lod.submeshCount; ++sub) {
                const uint32_t slot = lod.firstSubmesh + sub;
                const auto& subRow = db.submeshes.Rows()[slot];
                if (subRow.identity.y != 1u ||
                    subRow.value.geometryId == InvalidIndex)
                    continue;
                SceneSubmeshWork work{};
                work.identity = glm::uvec4(
                    verdict.value.x, verdict.value.y,
                    subRow.value.geometryId, subRow.value.materialId);
                work.detail = glm::uvec4(
                    level, slot, subRow.value.pipelineGroup,
                    subRow.value.passMask);
                selected.push_back(work);
            }
            if (!selected.empty()) {
                result.insert(result.end(), selected.begin(), selected.end());
                break;
            }
        }
    }
    return result;
}

// VkDispatchIndirectCommand is three consecutive uint32 words.
struct SceneIndirectDispatch {
    uint32_t x=0u, y=1u, z=1u;
};
struct SceneIndirectArgsPush {
    uint64_t offsets=0, args=0;
    uint32_t candidateCount=0, workCapacity=0;
};
struct SceneIndirectVerifyPush {
    uint64_t offsets=0, work=0, verified=0;
    uint32_t candidateCount=0, workCapacity=0;
};
static_assert(sizeof(SceneIndirectDispatch)==12);
static_assert(sizeof(SceneIndirectArgsPush)==24);
static_assert(sizeof(SceneIndirectVerifyPush)==32);

[[nodiscard]] inline SceneIndirectDispatch ReferenceIndirectDispatch(
    uint32_t workCount, uint32_t capacity) {
    if (workCount>capacity || capacity>524288u)
        return {0u,1u,1u};
    return {(workCount+63u)/64u,1u,1u};
}

class CandidateValidator {
public:
    static constexpr uint32_t Flights=smvk::limits::MaxFramesInFlight;
    static constexpr uint32_t MaxShaderRowSlots=4194304u;

    // MUST run after acquire fence on a slot, BEFORE replacing its buffers.
    void BeginFlight(smvk::ResourceManager& ctx, uint32_t flight) {
        if (flight>=Flights) return;
        auto& f=flight_[flight];
        f.prepared=false;
        if (!f.pending) return;
        f.pending=false;
        if (f.expected.empty()) return;
        std::vector<InstanceCullVerdict> readback(f.expected.size());
        if (!ctx.DownloadDataFromBuffer(f.verdicts,readback.data(),
                                readback.size()*sizeof(readback[0]))) {
            std::fprintf(stderr,"[GpuSceneValidate] readback failed\n");
            f.expected.clear();
            f.expectedCompact.clear();
            f.expectedWork.clear();
            f.compactPrepared = false;
            f.workPrepared = false;
            f.indirectPrepared = false;
            return;
        }
        uint64_t wrong=0;
        for (size_t i=0;i<readback.size();++i) {
            const auto& a=readback[i].value;
            const auto& b=f.expected[i].value;
            if (a.x!=b.x||a.y!=b.y||a.z!=b.z||a.w!=b.w) {
                ++wrong;
                if (wrong<=4)
                    std::fprintf(stderr,
                      "[GpuSceneValidate] mismatch candidate=%zu gpu=(%u,%u,%u,%u) cpu=(%u,%u,%u,%u)\n",
                      i,a.x,a.y,a.z,a.w,b.x,b.y,b.z,b.w);
            }
        }
        tested_+=readback.size();
        mismatches_+=wrong;
        std::fprintf(stderr,
          "[GpuSceneValidate] flight=%u compared=%zu mismatches=%llu total=%llu/%llu\n",
          flight,readback.size(),(unsigned long long)wrong,
          (unsigned long long)mismatches_,(unsigned long long)tested_);
        if (f.compactPrepared) {
            f.compactPrepared = false;
            const uint32_t expectedCount =
                static_cast<uint32_t>(f.expectedCompact.size());
            uint32_t gpuCount = UINT32_MAX;
            if (!ctx.DownloadDataFromBuffer(f.offsets, &gpuCount,
                    sizeof(gpuCount), size_t(f.groupCount)*sizeof(uint32_t))) {
                ++compactMismatches_;
                std::fprintf(stderr, "[GpuSceneCompact] count readback failed\n");
            } else if (gpuCount != expectedCount) {
                ++compactMismatches_;
                std::fprintf(stderr,
                    "[GpuSceneCompact] visible count gpu=%u cpu=%u\n",
                    gpuCount, expectedCount);
            } else if (gpuCount != 0u) {
                std::vector<InstanceCullVerdict> compact(gpuCount);
                if (!ctx.DownloadDataFromBuffer(f.compact, compact.data(),
                        compact.size()*sizeof(compact[0]))) {
                    ++compactMismatches_;
                    std::fprintf(stderr, "[GpuSceneCompact] data readback failed\n");
                } else {
                    for (size_t i=0; i<compact.size(); ++i) {
                        const auto& a=compact[i].value;
                        const auto& b=f.expectedCompact[i].value;
                        if (a.x!=b.x || a.y!=b.y || a.z!=b.z || a.w!=b.w) {
                            ++compactMismatches_;
                            if (compactMismatches_ <= 4)
                                std::fprintf(stderr,
                                    "[GpuSceneCompact] mismatch at %zu\n", i);
                        }
                    }
                }
            }
            std::fprintf(stderr,
                "[GpuSceneCompact] expected=%u errors=%llu\n",
                expectedCount, (unsigned long long)compactMismatches_);
            if (f.workPrepared) {
                f.workPrepared = false;
                const uint32_t expectedWork =
                    static_cast<uint32_t>(f.expectedWork.size());
                uint32_t gpuWork = UINT32_MAX;
                if (!ctx.DownloadDataFromBuffer(f.workOffsets, &gpuWork,
                    sizeof(gpuWork),
                    size_t(f.workCandidates)*sizeof(uint32_t))) {
                    ++workMismatches_;
                    std::fprintf(stderr,
                        "[GpuSceneWork] count readback failed\n");
                } else if (gpuWork != expectedWork) {
                    ++workMismatches_;
                    std::fprintf(stderr,
                        "[GpuSceneWork] count gpu=%u cpu=%u\n",
                        gpuWork, expectedWork);
                } else if (gpuWork != 0u) {
                    std::vector<SceneSubmeshWork> downloaded(gpuWork);
                    if (!ctx.DownloadDataFromBuffer(f.workOutput,
                        downloaded.data(),
                        downloaded.size()*sizeof(SceneSubmeshWork))) {
                        ++workMismatches_;
                        std::fprintf(stderr,
                            "[GpuSceneWork] records readback failed\n");
                    } else {
                        for (size_t i=0; i<downloaded.size(); ++i) {
                            const auto& a=downloaded[i];
                            const auto& b=f.expectedWork[i];
                            if (!glm::all(glm::equal(a.identity,b.identity)) ||
                                !glm::all(glm::equal(a.detail,b.detail))) {
                                ++workMismatches_;
                                if (workMismatches_ <= 4)
                                    std::fprintf(stderr,
                                        "[GpuSceneWork] mismatch at %zu\n", i);
                            }
                        }
                    }
                }
                std::fprintf(stderr,
                    "[GpuSceneWork] expected=%u mismatches=%llu\n",
                    expectedWork,
                    (unsigned long long)workMismatches_);
            }
            if (f.indirectPrepared) {
                f.indirectPrepared=false;
                SceneIndirectDispatch gpuArgs{};
                if (!ctx.DownloadDataFromBuffer(f.indirectArgs, &gpuArgs,
                        sizeof(gpuArgs))) {
                    ++indirectMismatches_;
                    std::fprintf(stderr,
                        "[GpuSceneIndirect] args readback failed\n");
                } else {
                    const auto expected=ReferenceIndirectDispatch(
                        uint32_t(f.expectedWork.size()),f.indirectCapacity);
                    if (gpuArgs.x!=expected.x || gpuArgs.y!=expected.y ||
                        gpuArgs.z!=expected.z) {
                        ++indirectMismatches_;
                        std::fprintf(stderr,
                            "[GpuSceneIndirect] groups gpu=(%u,%u,%u) cpu=(%u,%u,%u)\n",
                            gpuArgs.x,gpuArgs.y,gpuArgs.z,
                            expected.x,expected.y,expected.z);
                    }
                    if (gpuArgs.x==expected.x && gpuArgs.x>0u) {
                        std::vector<SceneSubmeshWork> verify(f.expectedWork.size());
                        if (!ctx.DownloadDataFromBuffer(f.indirectVerified,
                                verify.data(),
                                verify.size()*sizeof(SceneSubmeshWork))) {
                            ++indirectMismatches_;
                        } else {
                            for (size_t i=0;i<verify.size();++i) {
                                if (!glm::all(glm::equal(verify[i].identity,
                                        f.expectedWork[i].identity)) ||
                                    !glm::all(glm::equal(verify[i].detail,
                                        f.expectedWork[i].detail))) {
                                    ++indirectMismatches_;
                                    if (indirectMismatches_<=4)
                                        std::fprintf(stderr,
                                            "[GpuSceneIndirect] record mismatch %zu\n",i);
                                }
                            }
                        }
                    }
                }
                std::fprintf(stderr,
                    "[GpuSceneIndirect] expected=%zu errors=%llu\n",
                    f.expectedWork.size(),
                    (unsigned long long)indirectMismatches_);
            }
            f.expectedWork.clear();
            f.expectedCompact.clear();
        }
        f.expected.clear();
    }

    [[nodiscard]] bool Prepare(smvk::ResourceManager& ctx, uint32_t flight,
        const RenderDatabase& db, const Addresses& addresses,
        const CandidateSelection& selected, const RelativeFrustum& view,
        InstanceCullGpuPush& push) {
        if (flight>=Flights || !selected.Valid() ||
            selected.candidateCount==0 ||
            selected.candidateCount>MaxPassedInstanceCandidates ||
            selected.ranges.size()>MaxPassedLeafRanges ||
            db.leaves.Size()>MaxShaderRowSlots ||
            db.leafMembers.Size()>MaxShaderRowSlots ||
            db.instances.Size()>MaxShaderRowSlots ||
            db.meshes.Size()>MaxShaderRowSlots ||
            !addresses.leaves || !addresses.leafMembers ||
            !addresses.instances || !addresses.meshes)
            return false;
        auto& f=flight_[flight];
        if (f.pending) return false;
        f.prepared=false;
        InstanceCullGpuView viewData{};
        if (!PackInstanceCullView(view,viewData)) return false;
        const size_t bytes=selected.ranges.size()*sizeof(PassedInstanceRange);
        const size_t outBytes=size_t(selected.candidateCount)*sizeof(InstanceCullVerdict);
        if (!Ensure(ctx,f.ranges,f.rangesSize,bytes,"scene candidate ranges") ||
            !Ensure(ctx,f.verdicts,f.verdictSize,outBytes,"scene candidate verdicts"))
            return false;
        if (f.view.empty()) {
            f.view=ctx.CreateBuffer(sizeof(InstanceCullGpuView),
                                     smvk::BufferType_Storage,"scene cull view");
            if (f.view.empty()) return false;
        }
        push={};
        push.ranges=ctx.GetBufferDeviceAddress(f.ranges);
        push.verdicts=ctx.GetBufferDeviceAddress(f.verdicts);
        push.view=ctx.GetBufferDeviceAddress(f.view);
        push.leaves=addresses.leaves;
        push.members=addresses.leafMembers;
        push.instances=addresses.instances;
        push.meshes=addresses.meshes;
        if (!push.ranges || !push.verdicts || !push.view) return false;
        push.counts=glm::uvec4(uint32_t(selected.ranges.size()),
                                 selected.candidateCount,db.leafMembers.Size(),
                                 db.instances.Size());
        push.counts2=glm::uvec4(db.leaves.Size(),db.meshes.Size(),1,0);

        std::vector<InstanceCullVerdict> expected;
        expected.reserve(selected.candidateCount);
        for (const auto& range:selected.ranges) {
            const Leaf* leaf=db.leaves.Get({range.leaf.x,range.leaf.y});
            if (!leaf || leaf->range.x!=range.members.x ||
                leaf->range.y!=range.members.y) return false;
            for (uint32_t i=0;i<range.members.y;++i)
                expected.push_back(ReferenceCandidate(db,range.members.x+i,view));
        }
        if (expected.size()!=selected.candidateCount) return false;
        ctx.UploadDataToBuffer(f.ranges,selected.ranges.data(),bytes,
                               smvk::PipelineStageBit_Compute,smvk::AccessFlags::ShaderRead);
        ctx.UploadDataToBuffer(f.view,&viewData,sizeof(viewData),
                               smvk::PipelineStageBit_Compute,smvk::AccessFlags::ShaderRead);
        f.expected=std::move(expected);
        f.prepared=true;
        return true;
    }

    // Allocate scratch and produce BDA push blocks for the three-pass
    // compaction. No resources survive a flight reuse without its fence.
    [[nodiscard]] bool PrepareCompaction(smvk::ResourceManager& ctx,
        uint32_t flight, uint32_t candidateCount,
        CompactCountPush& count, CompactPrefixPush& prefix,
        CompactScatterPush& scatter) {
        if (flight >= Flights || candidateCount == 0u ||
            candidateCount > MaxPassedInstanceCandidates)
            return false;
        auto& f = flight_[flight];
        if (!f.prepared || f.expected.size() != candidateCount)
            return false;
        const uint32_t groups = (candidateCount + 63u) / 64u;
        if (!Ensure(ctx, f.counts, f.countsSize,
                    size_t(groups)*sizeof(uint32_t), "scene compact counts") ||
            !Ensure(ctx, f.offsets, f.offsetsSize,
                    size_t(groups+1u)*sizeof(uint32_t), "scene compact offsets") ||
            !Ensure(ctx, f.compact, f.compactSize,
                    size_t(candidateCount)*sizeof(InstanceCullVerdict),
                    "scene compact output"))
            return false;
        const uint64_t verdicts = ctx.GetBufferDeviceAddress(f.verdicts);
        const uint64_t counts = ctx.GetBufferDeviceAddress(f.counts);
        const uint64_t offsets = ctx.GetBufferDeviceAddress(f.offsets);
        const uint64_t output = ctx.GetBufferDeviceAddress(f.compact);
        if (!verdicts || !counts || !offsets || !output)
            return false;
        count = {verdicts, counts, candidateCount, groups};
        prefix = {counts, offsets, groups, 0u};
        scatter = {verdicts, offsets, output, candidateCount, groups};
        f.groupCount = groups;
        f.expectedCompact = CompactVisibleVerdicts(f.expected);
        f.compactPrepared = true;
        return true;
    }

    // The three work-generation stages consume the compacted output of
    // the same flight. Their input count remains entirely GPU-resident.
    [[nodiscard]] bool PrepareWorkExpansion(smvk::ResourceManager& ctx,
        uint32_t flight, const RenderDatabase& db,
        const Addresses& addresses, uint32_t candidateCount,
        WorkSelectPush& select, WorkPrefixPush& prefix,
        WorkScatterPush& scatter) {
        // This pipeline is validation-only and must not silently allocate
        // hundreds of megabytes at the production million-candidate cap.
        if (flight>=Flights || candidateCount==0u ||
            candidateCount>32768u ||
            db.instances.Size()>MaxShaderRowSlots ||
            db.meshes.Size()>MaxShaderRowSlots ||
            db.lods.Size()>MaxShaderRowSlots ||
            db.submeshes.Size()>MaxShaderRowSlots ||
            !addresses.instances || !addresses.meshes ||
            !addresses.lods || !addresses.submeshes)
            return false;
        auto& f=flight_[flight];
        if (!f.compactPrepared || f.expected.size()!=candidateCount)
            return false;
        constexpr uint32_t MaxSubmeshesPerLod=16u;
        const size_t maxWork=size_t(candidateCount)*MaxSubmeshesPerLod;
        if (!Ensure(ctx,f.workChoices,f.workChoicesSize,
                    size_t(candidateCount)*32u,"scene work choices") ||
            !Ensure(ctx,f.workCounts,f.workCountsSize,
                    size_t(candidateCount)*sizeof(uint32_t),
                    "scene work counts") ||
            !Ensure(ctx,f.workOffsets,f.workOffsetsSize,
                    size_t(candidateCount+1u)*sizeof(uint32_t),
                    "scene work offsets") ||
            !Ensure(ctx,f.workOutput,f.workOutputSize,
                    maxWork*sizeof(SceneSubmeshWork),"scene submesh work"))
            return false;
        const uint64_t compact=ctx.GetBufferDeviceAddress(f.compact);
        const uint64_t compactOffsets=ctx.GetBufferDeviceAddress(f.offsets);
        const uint64_t choices=ctx.GetBufferDeviceAddress(f.workChoices);
        const uint64_t counts=ctx.GetBufferDeviceAddress(f.workCounts);
        const uint64_t offsets=ctx.GetBufferDeviceAddress(f.workOffsets);
        const uint64_t output=ctx.GetBufferDeviceAddress(f.workOutput);
        if (!compact || !compactOffsets || !choices || !counts ||
            !offsets || !output)
            return false;

        f.expectedWork=ReferenceExpandWork(db,f.expectedCompact);
        if (f.expectedWork.size()>maxWork) {
            f.expectedWork.clear();
            return false;
        }
        select={};
        select.compact=compact;
        select.compactOffsets=compactOffsets;
        select.instances=addresses.instances;
        select.meshes=addresses.meshes;
        select.lods=addresses.lods;
        select.submeshes=addresses.submeshes;
        select.choices=choices;
        select.counts=counts;
        select.limits=glm::uvec4(candidateCount,f.groupCount,
                                 db.instances.Size(),db.meshes.Size());
        select.limits2=glm::uvec4(db.lods.Size(),db.submeshes.Size(),0u,0u);
        prefix={counts,offsets,candidateCount,0u};
        scatter={};
        scatter.choices=choices;
        scatter.offsets=offsets;
        scatter.submeshes=addresses.submeshes;
        scatter.output=output;
        scatter.limits=glm::uvec4(candidateCount,db.submeshes.Size(),
                                  uint32_t(maxWork),0u);
        f.workCandidates=candidateCount;
        f.workPrepared=true;
        return true;
    }

    // Prepare two fully GPU-driven passes: arg generation (one thread)
    // and an indirect compute consumer of the expanded work records.
    // The ResourceManager creates Storage buffers with INDIRECT usage too
    // (the same convention used by GfxFilter's indirect buffers).
    [[nodiscard]] bool PrepareIndirect(smvk::ResourceManager& ctx,
        uint32_t flight, uint32_t candidateCount,
        SceneIndirectArgsPush& args, SceneIndirectVerifyPush& verify,
        smvk::BufferHandle& indirect) {
        if (flight>=Flights || candidateCount==0u ||
            candidateCount>32768u) return false;
        auto& f=flight_[flight];
        if (!f.workPrepared || f.workCandidates!=candidateCount)
            return false;
        const size_t maxWork=size_t(candidateCount)*16u;
        if (f.indirectArgs.empty()) {
            f.indirectArgs=ctx.CreateBuffer(
                sizeof(SceneIndirectDispatch),smvk::BufferType_Storage,
                "scene indirect compute arguments");
            if (f.indirectArgs.empty()) return false;
        }
        if (!Ensure(ctx,f.indirectVerified,f.indirectVerifiedSize,
                maxWork*sizeof(SceneSubmeshWork),"scene indirect verification"))
            return false;
        const uint64_t offsets=ctx.GetBufferDeviceAddress(f.workOffsets);
        const uint64_t work=ctx.GetBufferDeviceAddress(f.workOutput);
        const uint64_t argAddress=ctx.GetBufferDeviceAddress(f.indirectArgs);
        const uint64_t checked=ctx.GetBufferDeviceAddress(f.indirectVerified);
        if (!offsets || !work || !argAddress || !checked) return false;
        args={offsets,argAddress,candidateCount,uint32_t(maxWork)};
        verify={offsets,work,checked,candidateCount,uint32_t(maxWork)};
        indirect=f.indirectArgs;
        f.indirectCapacity=uint32_t(maxWork);
        f.indirectPrepared=true;
        return true;
    }

    void MarkDispatched(uint32_t flight) {
        if (flight>=Flights) return;
        auto& f=flight_[flight];
        if (f.prepared) { f.pending=true; f.prepared=false; }
    }

    [[nodiscard]] uint64_t Compared() const { return tested_; }
    [[nodiscard]] uint64_t Mismatches() const { return mismatches_; }
    [[nodiscard]] uint64_t CompactMismatches() const { return compactMismatches_; }
    [[nodiscard]] uint64_t WorkMismatches() const { return workMismatches_; }
    [[nodiscard]] uint64_t IndirectMismatches() const { return indirectMismatches_; }

private:
    struct Flight {
        smvk::Holder<smvk::BufferHandle> ranges{},view{},verdicts{};
        smvk::Holder<smvk::BufferHandle> counts{},offsets{},compact{};
        size_t countsSize=0,offsetsSize=0,compactSize=0;
        uint32_t groupCount=0;
        std::vector<InstanceCullVerdict> expectedCompact{};
        bool compactPrepared=false;
        smvk::Holder<smvk::BufferHandle> workChoices{},workCounts{},
            workOffsets{},workOutput{};
        size_t workChoicesSize=0,workCountsSize=0,
            workOffsetsSize=0,workOutputSize=0;
        std::vector<SceneSubmeshWork> expectedWork{};
        uint32_t workCandidates=0;
        bool workPrepared=false;
        smvk::Holder<smvk::BufferHandle> indirectArgs{},indirectVerified{};
        size_t indirectVerifiedSize=0;
        uint32_t indirectCapacity=0;
        bool indirectPrepared=false;
        size_t rangesSize=0,verdictSize=0;
        std::vector<InstanceCullVerdict> expected{};
        bool pending=false,prepared=false;
    };
    static bool Ensure(smvk::ResourceManager& ctx,
        smvk::Holder<smvk::BufferHandle>& holder,size_t& capacity,
        size_t minimum,const char* label) {
        if (!holder.empty() && capacity>=minimum) return true;
        size_t n=std::max<size_t>(256,capacity);
        while (n<minimum) {
            if (n>std::numeric_limits<size_t>::max()/2) return false;
            n*=2;
        }
        auto fresh=ctx.CreateBuffer(n,smvk::BufferType_Storage,label);
        if (fresh.empty()) return false;
        holder=std::move(fresh);
        capacity=n;
        return true;
    }
    std::array<Flight,Flights> flight_{};
    uint64_t tested_=0,mismatches_=0,compactMismatches_=0,workMismatches_=0,indirectMismatches_=0;
};

} // namespace tour::gpu_scene
