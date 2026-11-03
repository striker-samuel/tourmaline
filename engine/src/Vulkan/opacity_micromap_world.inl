// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// Optional VK_EXT_opacity_micromap builder. A compute shader classifies
// 16 microtriangles per masked surface using packed UVs and the real
// sampled baseColor alpha texture. EXT micromaps attach to BLAS triangles;
// all Vulkan objects stay private to the RHI backend.
#include <Engine/platform.h>

bool smvk::VulkanResourceManager::SupportsOpacityMicromaps() const {
    return SupportsRayQueries() && g_opacityMicromapsSupported
        && vkCreateMicromapEXT && vkDestroyMicromapEXT
        && vkCmdBuildMicromapsEXT && vkGetMicromapBuildSizesEXT;
}

bool smvk::VulkanResourceManager::BuildOpacityMicromaps(
    std::span<const RayTracingInstance> uniqueGeometry)
{
    const bool hasMasked = std::any_of(uniqueGeometry.begin(), uniqueGeometry.end(),
        [](const RayTracingInstance& g) { return g.opacityMode == 1u; });
    if (!hasMasked)
        return true;
    if (!SupportsOpacityMicromaps() || !defaultPipeLayout)
        return false;
    const Buffer* indexes = buffers_.get(staticVertexFmtHeap.m_staticIndexBuffer);
    const Buffer* shading = buffers_.get(staticVertexFmtHeap.m_staticVertexShadingBuffer);
    if (!indexes || !shading || indexes->deviceAddress == 0 || shading->deviceAddress == 0)
        return false;

    // This method runs only after BuildRayTracingWorld's device-idle
    // synchronization. Bind the current material texture views into
    // flight 0 (the OMM compute submit uses that set explicitly).
    UpdateDescriptors(0);

    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    try {
        const std::vector<char> code =
            readFile("content/shaders/opacity_micromap_bake.cs.hlsl.spv");
        if (code.empty()) return false;
        CreateShaderModuleIn(code, shader);
        VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = shader;
        ci.stage.pName = "mainCS";
        ci.layout = defaultPipeLayout;
        if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci,
                nullptr, &pipeline) != VK_SUCCESS)
            pipeline = VK_NULL_HANDLE;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[OMM] compute shader load failed: %s\n", e.what());
    }
    if (!pipeline) {
        if (shader) vkDestroyShaderModule(device_, shader, nullptr);
        return false;
    }

    struct Tmp {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkDeviceAddress address = 0;
        void* mapped = nullptr;
    };
    auto destroyTmp = [this](Tmp& b) {
        if (b.buffer)
            vmaDestroyBuffer(m_allocator, b.buffer, b.allocation);
        b = {};
    };
    auto createTmp = [this](VkDeviceSize bytes, VkBufferUsageFlags usage,
                            bool hostVisible, Tmp& out) {
        VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        ci.size = bytes;
        ci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo mem{};
        mem.usage = hostVisible ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST
                                : VMA_MEMORY_USAGE_GPU_ONLY;
        if (hostVisible) {
            mem.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
            mem.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        }
        VmaAllocationInfo allocInfo{};
        if (vmaCreateBuffer(m_allocator, &ci, &mem,
                &out.buffer, &out.allocation, &allocInfo) != VK_SUCCESS)
            return false;
        out.mapped = allocInfo.pMappedData;
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
    auto alignUp = [](VkDeviceAddress address, VkDeviceSize align) {
        return (address + align - 1u) / align * align;
    };

    struct BuildInput {
        Tmp data;
        Tmp triangleArray;
        VkDeviceAddress dataAddress = 0;
        VkDeviceAddress triangleAddress = 0;
        VkMicromapUsageEXT usage{};
        VkDeviceSize scratchBytes = 0;
    };
    std::vector<BuildInput> builds(uniqueGeometry.size());
    rtOpacityMicromaps_.resize(uniqueGeometry.size());
    VkDeviceSize scratchMax = 0;

    // All buffers/pipelines are freed after the blocking build; micromap
    // handles + storage remain alive with their BLAS counterparts.
    auto cleanup = [&]() {
        for (BuildInput& b : builds) {
            destroyTmp(b.data);
            destroyTmp(b.triangleArray);
        }
        if (pipeline) vkDestroyPipeline(device_, pipeline, nullptr);
        if (shader) vkDestroyShaderModule(device_, shader, nullptr);
    };
    auto fail = [&]() {
        cleanup();
        return false; // caller clears any created micromaps while idle
    };

    for (size_t i = 0; i < uniqueGeometry.size(); ++i) {
        const RayTracingInstance& g = uniqueGeometry[i];
        if (g.opacityMode != 1u) continue;
        const uint32_t triangleCount = g.indexCount / 3u;
        if (triangleCount == 0u || triangleCount > UINT32_MAX / 8u
            || triangleCount > 65535u * 64u)
            return fail();
        BuildInput& input = builds[i];
        const VkDeviceSize dataBytes = VkDeviceSize(triangleCount) * 4u;
        const VkDeviceSize triangleBytes =
            VkDeviceSize(triangleCount) * sizeof(VkMicromapTriangleEXT);
        if (!createTmp(dataBytes + 256u,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT,
                false, input.data))
            return fail();
        if (!createTmp(triangleBytes + 256u,
                VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT,
                true, input.triangleArray))
            return fail();
        input.dataAddress = alignUp(input.data.address, 256u);
        input.triangleAddress = alignUp(input.triangleArray.address, 256u);
        const size_t arrayPad = size_t(input.triangleAddress - input.triangleArray.address);
        if (!input.triangleArray.mapped)
            return fail();

        // Each triangle has one 32-bit input word: 16 2-state bits
        // in the first two bytes, with two bytes of padding.
        auto* entries = reinterpret_cast<VkMicromapTriangleEXT*>(
            static_cast<char*>(input.triangleArray.mapped) + arrayPad);
        for (uint32_t tri = 0; tri < triangleCount; ++tri) {
            entries[tri].dataOffset = tri * 4u;
            entries[tri].subdivisionLevel = 2u;
            entries[tri].format = uint16_t(VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT);
        }
        vmaFlushAllocation(m_allocator, input.triangleArray.allocation, 0, VK_WHOLE_SIZE);

        input.usage.count = triangleCount;
        input.usage.subdivisionLevel = 2u;
        input.usage.format = VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT;
        VkMicromapBuildInfoEXT bi{ VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT };
        bi.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
        bi.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
        bi.usageCountsCount = 1;
        bi.pUsageCounts = &input.usage;
        bi.data.deviceAddress = input.dataAddress;
        bi.triangleArray.deviceAddress = input.triangleAddress;
        bi.triangleArrayStride = sizeof(VkMicromapTriangleEXT);
        VkMicromapBuildSizesInfoEXT sizes{
            VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT };
        vkGetMicromapBuildSizesEXT(device_,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &sizes);
        if (sizes.micromapSize == 0u)
            return fail();
        scratchMax = std::max(scratchMax, sizes.buildScratchSize);

        Tmp storage{};
        if (!createTmp(sizes.micromapSize,
                VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT, false, storage))
            return fail();
        VkMicromapCreateInfoEXT create{ VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT };
        create.buffer = storage.buffer;
        create.size = sizes.micromapSize;
        create.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
        RtOpacityMicromap& out = rtOpacityMicromaps_[i];
        if (vkCreateMicromapEXT(device_, &create, nullptr, &out.handle) != VK_SUCCESS) {
            destroyTmp(storage);
            return fail();
        }
        out.buffer = storage.buffer;
        out.allocation = storage.allocation;
        input.scratchBytes = sizes.buildScratchSize;
    }

    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
    VkPhysicalDeviceProperties2 props{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &asProps };
    vkGetPhysicalDeviceProperties2(physicalDevice_, &props);
    const VkDeviceSize scratchAlign = std::max<VkDeviceSize>(
        256u, asProps.minAccelerationStructureScratchOffsetAlignment);
    Tmp scratch{};
    if (!createTmp(std::max<VkDeviceSize>(scratchMax, 1u) + scratchAlign,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, scratch))
        return fail();
    const VkDeviceAddress scratchAddress =
        alignUp(scratch.address, scratchAlign);

    const CommandSubmitHandle job = m_submitter->acquire();
    const VkCommandBuffer cmd = m_submitter->GetCommandBuffer(job);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        defaultPipeLayout, 0, 1, &defaultDescriptorSets_[0], 0, nullptr);
    struct OmmPush {
        uint64_t indices, shading, output;
        uint32_t firstIndex;
        int32_t baseVertex;
        uint32_t triangleCount;
        uint32_t textureIndex;
        float alphaFactor, alphaCutoff;
    };
    static_assert(sizeof(OmmPush) == 48);
    auto barrier = [&](VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                       VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkMemoryBarrier2 mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = srcStage;
        mb.srcAccessMask = srcAccess;
        mb.dstStageMask = dstStage;
        mb.dstAccessMask = dstAccess;
        VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
    };

    // Make uploaded index/shading data visible to the compute classifier.
    // Cached static geometry is often written on the transfer queue;
    // this consumer runs on the graphics queue after WaitForUploads().
    barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    for (size_t i = 0; i < uniqueGeometry.size(); ++i) {
        const RayTracingInstance& g = uniqueGeometry[i];
        if (g.opacityMode != 1u) continue;
        BuildInput& input = builds[i];
        OmmPush push{ indexes->deviceAddress, shading->deviceAddress,
            input.dataAddress, g.firstIndex, g.baseVertex, g.indexCount / 3u,
            g.opacityTextureIndex, g.opacityFactor, g.opacityCutoff };
        vkCmdPushConstants(cmd, defaultPipeLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (push.triangleCount + 63u) / 64u, 1, 1);
        // The compute writes become visible to the micromap builder's
        // specialized read access, not merely to another shader load.
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT,
            VK_ACCESS_2_MICROMAP_READ_BIT_EXT);

        VkMicromapBuildInfoEXT bi{ VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT };
        bi.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
        bi.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
        bi.dstMicromap = rtOpacityMicromaps_[i].handle;
        bi.usageCountsCount = 1;
        bi.pUsageCounts = &input.usage;
        bi.data.deviceAddress = input.dataAddress;
        bi.triangleArray.deviceAddress = input.triangleAddress;
        bi.triangleArrayStride = sizeof(VkMicromapTriangleEXT);
        bi.scratchData.deviceAddress = scratchAddress;
        vkCmdBuildMicromapsEXT(cmd, 1, &bi);
        barrier(VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT,
            VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT,
            VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT,
            VK_ACCESS_2_MICROMAP_READ_BIT_EXT | VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT);
    }
    m_submitter->submit(job);
    m_submitter->wait(job);
    destroyTmp(scratch);
    cleanup();
    std::fprintf(stderr, "[OMM] baked %zu geometry slots\n",
        rtOpacityMicromaps_.size());
    return true;
}
