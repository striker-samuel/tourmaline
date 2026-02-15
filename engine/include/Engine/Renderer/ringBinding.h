#pragma once

static inline VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) {
    return (v + a - 1) & ~(a - 1);
}

template<typename T>
struct DynamicRingBinding
{
    BufferHandle buffer{};          // your wrapper handle
    VkBuffer vkBuffer{};            // cached if useful
    void* mapped = nullptr;          // if persistently mapped

    uint32_t binding = 0;            // descriptor binding number inside the set

    VkDeviceSize elemSize = sizeof(T);
    VkDeviceSize elemStride = 0;     // aligned
    VkDeviceSize range = sizeof(T); // descriptor range (usually elemSize)
    uint32_t     count = 0;     // ring slots (frames in flight, etc.)

    std::vector<uint32_t> dynOffsets; // byte offsets for vkCmdBindDescriptorSets

    void init(
        VkPhysicalDevice physicalDevice,
        Context* context,
        uint32_t binding_,
        uint32_t ringCount,
        VkBufferUsageFlags usageFlags,
        VmaMemoryUsage memUsage = VMA_MEMORY_USAGE_AUTO
    )
    {
        binding = binding_;
        count = ringCount;

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physicalDevice, &props);

        const VkDeviceSize align = props.limits.minUniformBufferOffsetAlignment; // for UBO dynamic
        // If you use this for SSBO dynamic too, use minStorageBufferOffsetAlignment there.

        elemStride = align_up(elemSize, align);
        range = elemSize;

        const VkDeviceSize totalSize = elemStride * count;

        buffer = context->createBuffer(
            totalSize,
            memUsage,
            usageFlags
        );

        vkBuffer = context->getVkBuffer(buffer);   // if you have accessor
        mapped = context->getMappedPtr(buffer);  // if persistently mapped (optional)

        dynOffsets.resize(count);
        for (uint32_t i = 0; i < count; ++i)
            dynOffsets[i] = static_cast<uint32_t>(i * elemStride);
    }

    // Descriptor write info (note: offset=0, range=sizeof(T))
    VkDescriptorBufferInfo descriptorInfo() const {
        VkDescriptorBufferInfo info{};
        info.buffer = vkBuffer;
        info.offset = 0;
        info.range = range;
        return info;
    }

    // The dynamic offset you pass when binding the set
    uint32_t dynamicOffset(uint32_t index) const {
        return dynOffsets[index];
    }

    // Pointer to the element memory (if mapped)
    T* elementPtr(uint32_t index) const {
        return reinterpret_cast<T*>(
            static_cast<uint8_t*>(mapped) + (VkDeviceSize)index * elemStride
            );
    }
};
