// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// Vulkan KHR ray-query acceleration world. Included by Vulkan/impl.cpp:
// shares its Volk/VMA context without exposing Vulkan in the RHI API.
// Prototype uses a blocking build on world/transform changes. Static worlds
// are cached by WorldRenderer; the per-frame ray pass itself is asynchronous.
#include <array>
#include <bit>
#include <map>

bool smvk::VulkanResourceManager::SupportsRayQueries() const {
    return g_rayQueriesSupported
        && vkCmdBuildAccelerationStructuresKHR
        && vkGetAccelerationStructureBuildSizesKHR
        && vkCreateAccelerationStructureKHR
        && vkDestroyAccelerationStructureKHR
        && vkGetAccelerationStructureDeviceAddressKHR;
}

void smvk::VulkanResourceManager::ClearRayTracingWorld() {
    // Caller must have drained in-flight graphics before releasing a TLAS
    // referenced by an already-submitted ray-query dispatch.
    auto release = [this](RtAcceleration& as) {
        if (as.handle != VK_NULL_HANDLE)
            vkDestroyAccelerationStructureKHR(device_, as.handle, nullptr);
        if (as.buffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(m_allocator, as.buffer, as.allocation);
        as = {};
    };
    release(rtTlas_);
    for (RtAcceleration& as : rtBlas_)
        release(as);
    rtBlas_.clear();
    for (RtOpacityMicromap& mm : rtOpacityMicromaps_) {
        if (mm.handle)
            vkDestroyMicromapEXT(device_, mm.handle, nullptr);
        if (mm.buffer)
            vmaDestroyBuffer(m_allocator, mm.buffer, mm.allocation);
        mm = {};
    }
    rtOpacityMicromaps_.clear();
    rtWorldReady_ = false;
}

bool smvk::VulkanResourceManager::BuildRayTracingWorld(
    std::span<const RayTracingInstance> input)
{
    if (!SupportsRayQueries() || input.empty())
        return false;

    // Geometry is the shared static position/index heap, not a separate
    // CPU mesh copy. The façade waits for async uploads before this call.
    const Buffer* positions = buffers_.get(staticVertexFmtHeap.m_staticVertexPosBuffer);
    const Buffer* indices = buffers_.get(staticVertexFmtHeap.m_staticIndexBuffer);
    if (!positions || !indices || !positions->deviceAddress || !indices->deviceAddress)
        return false;

    // Each unique indexed surface needs one BLAS. Transform-only copies
    // share that BLAS and become distinct TLAS instances.
    std::map<std::array<uint32_t, 8>, uint32_t> geometryIndex;
    std::vector<RayTracingInstance> uniqueGeometry, accepted;
    std::vector<uint32_t> blasForInstance;
    for (const RayTracingInstance& inst : input) {
        if (inst.baseVertex < 0 || inst.vertexCount < 3 ||
            inst.indexCount < 3 || (inst.indexCount % 3) != 0)
            continue;
        const uint64_t vertexEnd = (uint64_t(inst.baseVertex) + inst.vertexCount) * 12ull;
        const uint64_t indexEnd = (uint64_t(inst.firstIndex) + inst.indexCount) * 4ull;
        if (vertexEnd > positions->allocBytes || indexEnd > indices->allocBytes)
            continue;
        // A mesh shared by two materials has different opacity patterns.
        const std::array<uint32_t, 8> key{ inst.firstIndex, inst.indexCount,
            uint32_t(inst.baseVertex), inst.vertexCount, inst.opacityMode,
            inst.opacityTextureIndex, std::bit_cast<uint32_t>(inst.opacityFactor),
            std::bit_cast<uint32_t>(inst.opacityCutoff) };
        auto [it, added] = geometryIndex.emplace(key, uint32_t(uniqueGeometry.size()));
        if (added) uniqueGeometry.push_back(inst);
        accepted.push_back(inst);
        blasForInstance.push_back(it->second);
    }
    if (accepted.empty())
        return false;
    if (accepted.size() > 0x00ffffffu)
        return false;

    // Conservative synchronous update, necessary before rewriting the
    // single TLAS binding shared by all frame-in-flight descriptor sets.
    if (vkDeviceWaitIdle(device_) != VK_SUCCESS)
        return false;
    ClearRayTracingWorld();

    // Micromaps must exist BEFORE any BLAS size query or build; the
    // optional builder runs compute alpha classification on device and
    // waits for VK_EXT_opacity_micromap construction to complete.
    if (!BuildOpacityMicromaps(uniqueGeometry)) {
        ClearRayTracingWorld();
        return false;
    }

    struct TempBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkDeviceAddress address = 0;
        void* mapped = nullptr;
    };
    auto drop = [this](TempBuffer& b) {
        if (b.buffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(m_allocator, b.buffer, b.allocation);
        b = {};
    };
    auto alloc = [this](VkDeviceSize bytes, VkBufferUsageFlags usage,
                      bool hostVisible, TempBuffer& out) {
        VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        ci.size = bytes;
        ci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo aci{};
        aci.usage = hostVisible ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST
                                : VMA_MEMORY_USAGE_GPU_ONLY;
        if (hostVisible) {
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        }
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(m_allocator, &ci, &aci, &out.buffer,
                &out.allocation, &info) != VK_SUCCESS)
            return false;
        out.mapped = info.pMappedData;
        VkBufferDeviceAddressInfo ai{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        ai.buffer = out.buffer;
        out.address = vkGetBufferDeviceAddress(device_, &ai);
        if (!out.address) {
            vmaDestroyBuffer(m_allocator, out.buffer, out.allocation);
            out = {};
            return false;
        }
        return true;
    };
    auto makeAs = [this, &alloc](VkDeviceSize bytes,
                            VkAccelerationStructureTypeKHR type, RtAcceleration& out) {
        TempBuffer b{};
        if (!alloc(bytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                   false, b))
            return false;
        VkAccelerationStructureCreateInfoKHR ci{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        ci.buffer = b.buffer;
        ci.size = bytes;
        ci.type = type;
        if (vkCreateAccelerationStructureKHR(device_, &ci, nullptr,
                &out.handle) != VK_SUCCESS) {
            vmaDestroyBuffer(m_allocator, b.buffer, b.allocation);
            return false;
        }
        out.buffer = b.buffer;
        out.allocation = b.allocation;
        return true;
    };
    auto getGeometry = [&](const RayTracingInstance& inst) {
        VkAccelerationStructureGeometryKHR geo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geo.geometry.triangles.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        geo.geometry.triangles.vertexData.deviceAddress =
            positions->deviceAddress + uint64_t(inst.baseVertex) * 12ull;
        geo.geometry.triangles.vertexStride = 12;
        geo.geometry.triangles.maxVertex = inst.vertexCount - 1;
        geo.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        geo.geometry.triangles.indexData.deviceAddress =
            indices->deviceAddress + uint64_t(inst.firstIndex) * 4ull;
        return geo;
    };
    auto getInfo = [](VkAccelerationStructureTypeKHR type,
                      const VkAccelerationStructureGeometryKHR* geo) {
        VkAccelerationStructureBuildGeometryInfoKHR build{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        build.type = type;
        build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build.geometryCount = 1;
        build.pGeometries = geo;
        return build;
    };

    // The scratch buffer is shared between builds; barriers serialize
    // scratch reuse. A single submission builds all BLAS and the TLAS.
    std::vector<VkAccelerationStructureBuildSizesInfoKHR> blasSizes(uniqueGeometry.size());
    // Keep pNext attachment/usage structures stable through BOTH the
    // BLAS size query and the recorded build command.
    std::vector<VkMicromapUsageEXT> ommUsages(uniqueGeometry.size());
    std::vector<VkAccelerationStructureTrianglesOpacityMicromapEXT>
        ommAttachments(uniqueGeometry.size());
    for (size_t i = 0; i < uniqueGeometry.size(); ++i) {
        if (i >= rtOpacityMicromaps_.size() || !rtOpacityMicromaps_[i].handle)
            continue;
        auto& u = ommUsages[i];
        u.count = uniqueGeometry[i].indexCount / 3u;
        u.subdivisionLevel = 2u;
        u.format = VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT;
        auto& a = ommAttachments[i];
        a.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT;
        a.indexType = VK_INDEX_TYPE_NONE_KHR;
        a.baseTriangle = 0;
        a.usageCountsCount = 1;
        a.pUsageCounts = &u;
        a.micromap = rtOpacityMicromaps_[i].handle;
    }
    auto attachOpacity = [&](VkAccelerationStructureGeometryKHR& geo, size_t i) {
        if (i < ommAttachments.size() && ommAttachments[i].micromap)
            geo.geometry.triangles.pNext = &ommAttachments[i];
    };
    VkDeviceSize scratchMax = 0;
    rtBlas_.resize(uniqueGeometry.size());
    for (size_t i = 0; i < uniqueGeometry.size(); ++i) {
        auto geo = getGeometry(uniqueGeometry[i]);
        attachOpacity(geo, i);
        auto bi = getInfo(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &geo);
        auto& sz = blasSizes[i];
        sz.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        uint32_t primitives = uniqueGeometry[i].indexCount / 3;
        vkGetAccelerationStructureBuildSizesKHR(device_,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &primitives, &sz);
        scratchMax = std::max(scratchMax, sz.buildScratchSize);
        if (!makeAs(sz.accelerationStructureSize,
                VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, rtBlas_[i])) {
            ClearRayTracingWorld();
            return false;
        }
    }

    std::vector<VkAccelerationStructureInstanceKHR> nativeInstances;
    nativeInstances.reserve(accepted.size());
    for (size_t i = 0; i < accepted.size(); ++i) {
        VkAccelerationStructureInstanceKHR native{};
        for (uint32_t row = 0; row < 3; ++row)
            for (uint32_t col = 0; col < 4; ++col)
                native.transform.matrix[row][col] = accepted[i].transform[row * 4 + col];
        native.instanceCustomIndex = uint32_t(i) & 0x00ffffffu;
        native.mask = 0xff;
        native.instanceShaderBindingTableRecordOffset = 0;
        native.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
        addressInfo.accelerationStructure = rtBlas_[blasForInstance[i]].handle;
        native.accelerationStructureReference =
            vkGetAccelerationStructureDeviceAddressKHR(device_, &addressInfo);
        nativeInstances.push_back(native);
    }

    TempBuffer instanceBuffer{}, scratchBuffer{};
    auto fail = [&]() {
        drop(instanceBuffer);
        drop(scratchBuffer);
        ClearRayTracingWorld();
        return false;
    };
    const VkDeviceSize instancesBytes =
        VkDeviceSize(nativeInstances.size()) * sizeof(VkAccelerationStructureInstanceKHR);
    if (!alloc(instancesBytes + 16,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
            true, instanceBuffer))
        return fail();
    const VkDeviceSize instPad = (16 - (instanceBuffer.address % 16)) % 16;
    if (!instanceBuffer.mapped)
        return fail();
    std::memcpy(static_cast<char*>(instanceBuffer.mapped) + instPad,
        nativeInstances.data(), size_t(instancesBytes));
    vmaFlushAllocation(m_allocator, instanceBuffer.allocation, 0, VK_WHOLE_SIZE);

    VkAccelerationStructureGeometryKHR instancesGeo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    instancesGeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    instancesGeo.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instancesGeo.geometry.instances.arrayOfPointers = VK_FALSE;
    instancesGeo.geometry.instances.data.deviceAddress =
        instanceBuffer.address + instPad;
    auto tlasBuild = getInfo(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &instancesGeo);
    VkAccelerationStructureBuildSizesInfoKHR tlasSize{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    const uint32_t instanceCount = uint32_t(nativeInstances.size());
    vkGetAccelerationStructureBuildSizesKHR(device_,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlasBuild,
        &instanceCount, &tlasSize);
    scratchMax = std::max(scratchMax, tlasSize.buildScratchSize);
    if (!makeAs(tlasSize.accelerationStructureSize,
            VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, rtTlas_))
        return fail();

    VkPhysicalDeviceAccelerationStructurePropertiesKHR props{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
    VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &props };
    vkGetPhysicalDeviceProperties2(physicalDevice_, &props2);
    const VkDeviceSize alignment =
        std::max<VkDeviceSize>(256, props.minAccelerationStructureScratchOffsetAlignment);
    if (!alloc(scratchMax + alignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
               false, scratchBuffer))
        return fail();
    const VkDeviceAddress scratchAddress =
        (scratchBuffer.address + alignment - 1) / alignment * alignment;

    const CommandSubmitHandle job = m_submitter->acquire();
    const VkCommandBuffer cmd = m_submitter->GetCommandBuffer(job);
    auto asBarrier = [&](VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkMemoryBarrier2 mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        mb.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        mb.dstStageMask = dstStage;
        mb.dstAccessMask = dstAccess;
        VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
    };
    // The source position/index monoblocks can have just completed staged
    // uploads. Establish a graphics-queue visibility dependency for AS
    // reads even if their original upload barrier targeted vertex input.
    VkMemoryBarrier2 inputBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    inputBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    inputBarrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    inputBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    inputBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    if (SupportsOpacityMicromaps())
        inputBarrier.dstAccessMask |= VK_ACCESS_2_MICROMAP_READ_BIT_EXT;
    VkDependencyInfo inputDependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    inputDependency.memoryBarrierCount = 1;
    inputDependency.pMemoryBarriers = &inputBarrier;
    vkCmdPipelineBarrier2(cmd, &inputDependency);
    for (size_t i = 0; i < uniqueGeometry.size(); ++i) {
        auto geo = getGeometry(uniqueGeometry[i]);
        attachOpacity(geo, i);
        auto bi = getInfo(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &geo);
        bi.dstAccelerationStructure = rtBlas_[i].handle;
        bi.scratchData.deviceAddress = scratchAddress;
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = uniqueGeometry[i].indexCount / 3;
        const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = { &range };
        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &bi, ranges);
        asBarrier(VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    }
    tlasBuild.dstAccelerationStructure = rtTlas_.handle;
    tlasBuild.scratchData.deviceAddress = scratchAddress;
    VkAccelerationStructureBuildRangeInfoKHR tlasRange{};
    tlasRange.primitiveCount = instanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR* tlasRanges[] = { &tlasRange };
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tlasBuild, tlasRanges);
    asBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    m_submitter->submit(job);
    m_submitter->wait(job);
    drop(instanceBuffer);
    drop(scratchBuffer);

    // The TLAS is now complete. The device is idle and each flight's
    // descriptor set may be updated without racing a pending shader read.
    VkWriteDescriptorSetAccelerationStructureKHR asInfo{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
    asInfo.accelerationStructureCount = 1;
    asInfo.pAccelerationStructures = &rtTlas_.handle;
    for (uint32_t f = 0; f < smvk::limits::MaxFramesInFlight; ++f) {
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asInfo };
        write.dstSet = defaultDescriptorSets_[f];
        write.dstBinding = smvk::Bindings::kBinding_RayTracingScene;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }
    rtWorldReady_ = true;
    std::fprintf(stderr, "[RayShadows] built BLAS=%zu TLAS instances=%u\n",
        rtBlas_.size(), instanceCount);
    return true;
}
