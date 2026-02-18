#pragma once

#include <cstdint>
#include <Engine/Renderer/context.h>

static inline VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) {
	return (v + a - 1) & ~(a - 1);
}

template<typename T, uint32_t MaxElems>
struct RingBinding
{

	Context* context = nullptr;
	uint32_t binding = 0;
	uint32_t ringCount = 0;

	VkDeviceSize elemStride = sizeof(T);
	VkDeviceSize frameStride = 0;
	VkDeviceSize range = 0;
	DeviceBufferHandle buffer_h;
	void* mapped = nullptr;

	VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;

	void init(
		Context* context_,
		uint32_t binding_, // Manual count for bidning positions
		uint32_t ringCount_, // should be almost always FRAMES_IN_FLIGHT
		VkBufferUsageFlags usageFlags,
		VkDescriptorType descriptorType_,
		VmaMemoryUsage memUsage = VMA_MEMORY_USAGE_AUTO
	)
	{
		context = context_;
		binding = binding_;
		ringCount = ringCount_;
		descriptorType = descriptorType_;

		//if (descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
		//	assert(false, "invalid descriptor type for ringBuffer");
		//}

		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(*context->physicalDevice_, &props);

		const VkDeviceSize dynAlign =
			(descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
			? props.limits.minUniformBufferOffsetAlignment
			: props.limits.minStorageBufferOffsetAlignment;

		elemStride = sizeof(T);                 // you trust users to pad T if needed
		range = elemStride * MaxElems;     // expose the whole per-frame array
		frameStride = align_up(range, dynAlign); // only frame starts must satisfy dynAlign

		const VkDeviceSize totalSize = frameStride * ringCount;

		buffer_h = context->createBuffer(totalSize, memUsage, usageFlags);
		//mapped = context->getMappedPtr(buffer_h); // if persistently mapped
	}

	void uploadData(
		const std::array<T, MaxElems> &elems,
		uint32_t ringIndex,
		VkPipelineStageFlags dstStages,
		VkAccessFlags dstAccess
	) {

		context->UploadDataToBuffer(
			buffer_h,
			elems.data(),
			sizeof(T) * elems.size(),
			dstStages,
			dstAccess,
			0,
			dynamicOffsetFrame(ringIndex)
		);

	}


	BindingDesc bindingDesc(
		//const std::vector<VkSampler>& resolvedImmutableSamplers
	) const{
		BindingDesc bn{};
		bn.binding = binding;
		bn.type = descriptorType;
		bn.count = 1;
		bn.stages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;
		//bn.flags; // No flag;
		//bn.immutableSamplers = resolvedImmutableSamplers.empty()
		//	? nullptr
		//	: resolvedImmutableSamplers.data();
		return bn;
	}

	VkDescriptorBufferInfo descriptorInfo() const {
		VkDescriptorBufferInfo info{};
		info.buffer = context->buffers_.get(buffer_h);
		info.offset = 0;
		info.range = range; // IMPORTANT: per-frame array size
		return info;
	}

	// One dynamic offset per frame
	uint32_t dynamicOffsetFrame(uint32_t frameIndex) const {
		return static_cast<uint32_t>(VkDeviceSize(frameIndex) * frameStride);
	}

	// Pointer to element i inside frame f
	T* elementPtr(uint32_t frameIndex, uint32_t elemIndex) const {
		const VkDeviceSize off =
			VkDeviceSize(frameIndex) * frameStride +
			VkDeviceSize(elemIndex) * elemStride;
		return reinterpret_cast<T*>(static_cast<uint8_t*>(mapped) + off);
	}

	static constexpr uint32_t capacity_per_frame() { return MaxElems; }
};
