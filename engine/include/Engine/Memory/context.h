//#include <Engine/Memory/pool.h>
//
//
//// Lets not do LODs yet.
//struct MeshGpu {
//	// MeshData Fields.
//	uint32_t index_count;
//	VkBuffer buffer;
//};
//
//
//enum StorageType {
//	StorageType_Device,
//	StorageType_HostVisible,
//	StorageType_Memoryless
//};
//
//
//struct BufferDesc final {
//	uint8_t usage = 0;
//	StorageType storage = StorageType_HostVisible;
//	size_t size = 0;
//	const void* data = nullptr;
//	const char* debugName = "";
//};
//
//
//enum BufferUsageBits : uint8_t {
//	BufferUsageBits_Index = 1 << 0,
//	BufferUsageBits_Vertex = 1 << 1,
//	BufferUsageBits_Uniform = 1 << 2,
//	BufferUsageBits_Storage = 1 << 3,
//	BufferUsageBits_Indirect = 1 << 4,
//	// ray tracing
//	BufferUsageBits_ShaderBindingTable = 1 << 5,
//	BufferUsageBits_AccelStructBuildInputReadOnly = 1 << 6,
//	BufferUsageBits_AccelStructStorage = 1 << 7
//};
//
//struct MaterialGpu {
//};
//
//struct UniformBufferAllocation {
//	VkBuffer buffer;
//	VmaAllocation alloc;
//	VmaAllocationInfo allocInfo;
//	VkDeviceSize size;
//	VkMemoryPropertyFlags memory_flags;
//
//	// Did it end in mappable memory? This is important to know for proper upload. Helper to know
//	bool is_mapapble() {
//		return this->memory_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
//	}
//
//};
//
//using BufferHandle = Handle<struct Buffer>;
//
//BufferHandle Context::createBuffer(VkDeviceSize bufferSize,
//	VkBufferUsageFlags usageFlags,
//	VkMemoryPropertyFlags memFlags,
//	//lvk::Result* outResult,
//	const char* debugName) {
//	//LVK_PROFILER_FUNCTION_COLOR(LVK_PROFILER_COLOR_CREATE);
//
//	//LVK_ASSERT(bufferSize > 0);
//
//#define ENSURE_BUFFER_SIZE(flag, maxSize)                                                             \
//  if (usageFlags & flag) {                                                                            \
//    if (!LVK_VERIFY(bufferSize <= maxSize)) {                                                         \
//      Result::setResult(outResult, Result(Result::Code::RuntimeError, "Buffer size exceeded" #flag)); \
//      return {};                                                                                      \
//    }                                                                                                 \
//  }
//
//	const VkPhysicalDeviceLimits& limits = getVkPhysicalDeviceProperties().limits;
//
//	ENSURE_BUFFER_SIZE(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, limits.maxUniformBufferRange);
//	// any buffer
//	ENSURE_BUFFER_SIZE(VK_BUFFER_USAGE_FLAG_BITS_MAX_ENUM, limits.maxStorageBufferRange);
//#undef ENSURE_BUFFER_SIZE
//
//	VulkanBuffer buf = {
//		.bufferSize_ = bufferSize,
//		.vkUsageFlags_ = usageFlags,
//		.vkMemFlags_ = memFlags,
//	};
//
//	const VkBufferCreateInfo ci = {
//		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
//		.pNext = nullptr,
//		.flags = 0,
//		.size = bufferSize,
//		.usage = usageFlags,
//		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
//		.queueFamilyIndexCount = 0,
//		.pQueueFamilyIndices = nullptr,
//	};
//
//	if (LVK_VULKAN_USE_VMA) {
//		VmaAllocationCreateInfo vmaAllocInfo = {};
//
//		// Initialize VmaAllocation Info
//		if (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
//			vmaAllocInfo = {
//				.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
//				.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
//				.preferredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
//			};
//		}
//
//		if (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
//			// Check if coherent buffer is available.
//			VK_ASSERT(vkCreateBuffer(vkDevice_, &ci, nullptr, &buf.vkBuffer_));
//			VkMemoryRequirements requirements = {};
//			vkGetBufferMemoryRequirements(vkDevice_, buf.vkBuffer_, &requirements);
//			vkDestroyBuffer(vkDevice_, buf.vkBuffer_, nullptr);
//			buf.vkBuffer_ = VK_NULL_HANDLE;
//
//			if (requirements.memoryTypeBits & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
//				vmaAllocInfo.requiredFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
//				buf.isCoherentMemory_ = true;
//			}
//		}
//
//		vmaAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
//
//		vmaCreateBufferWithAlignment((VmaAllocator)getVmaAllocator(), &ci, &vmaAllocInfo, 16, &buf.vkBuffer_, &buf.vmaAllocation_, nullptr);
//
//		// handle memory-mapped buffers
//		if (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
//			vmaMapMemory((VmaAllocator)getVmaAllocator(), buf.vmaAllocation_, &buf.mappedPtr_);
//		}
//	}
//
//	// set debug name
//	//VK_ASSERT(lvk::setDebugObjectName(vkDevice_, VK_OBJECT_TYPE_BUFFER, (uint64_t)buf.vkBuffer_, debugName));
//
//	// handle shader access
//	if (usageFlags & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
//		const VkBufferDeviceAddressInfo ai = {
//			.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
//			.buffer = buf.vkBuffer_,
//		};
//		buf.vkDeviceAddress_ = vkGetBufferDeviceAddress(vkDevice_, &ai);
//		LVK_ASSERT(buf.vkDeviceAddress_);
//	}
//
//	return buffers_.create(std::move(buf));
//}
//
//
//// May be a good idea to namespace this:
//
//}; class Context {
//
//private:
//
//	VmaAllocator allocator_;
//
//public:
//
//	// We may not event need it!
//	//bool initialize();
//
//	bool createBuffer(const BufferDesc& requestedDec, const char* debugName);
//	bool createMesh();
//
//	//bool createInstance();
//	//bool createTexture();
//	//bool createMaterial();
//	//bool createShaderModule();
//
//	Pool<lvk::Buffer, lvk::VulkanBuffer> buffers_;
//
//};
//
//Context::() {
//	buffers_.objects_.reserve(256);
//}
//
//
//Context::createBuffer(const BufferDesc& requestedDesc, const char* debugName) {
//	
//	BufferDesc desc = requestedDesc;
//
//	if (debugName && *debugName)
//		desc.debugName = debugName;
//
//	if (!useStaging_ && (desc.storage == StorageType_Device)) {
//		desc.storage = StorageType_HostVisible;
//	}
//
//	// Use staging device to transfer data into the buffer when the storage is private to the device
//	VkBufferUsageFlags usageFlags = (desc.storage == StorageType_Device) ? VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
//		: 0;
//
//	if (desc.usage == 0) {
//		//Result::setResult(outResult, Result(Result::Code::ArgumentOutOfRange, "Invalid buffer usage"));
//		return {};
//	}
//
//	if (desc.usage & BufferUsageBits_Index) {
//		usageFlags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
//	}
//	if (desc.usage & BufferUsageBits_Vertex) {
//		usageFlags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
//	}
//	if (desc.usage & BufferUsageBits_Uniform) {
//		usageFlags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR;
//	}
//
//	if (desc.usage & BufferUsageBits_Storage) {
//		usageFlags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR;
//	}
//
//	if (desc.usage & BufferUsageBits_Indirect) {
//		usageFlags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR;
//	}
//
//	if (desc.usage & BufferUsageBits_ShaderBindingTable) {
//		usageFlags |= VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR;
//	}
//
//	if (desc.usage & BufferUsageBits_AccelStructBuildInputReadOnly) {
//		usageFlags |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR;
//	}
//
//	if (desc.usage & BufferUsageBits_AccelStructStorage) {
//		usageFlags |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR;
//	}
//
//	//LVK_ASSERT_MSG(usageFlags, "Invalid buffer usage");
//
//	const VkMemoryPropertyFlags memFlags = storageTypeToVkMemoryPropertyFlags(desc.storage);
//
//	//Result result;
//	VkResult result;
//	BufferHandle handle = createBuffer(desc.size, usageFlags, memFlags, &result, desc.debugName);
//
//	if (!LVK_VERIFY(result.isOk())) {
//		Result::setResult(outResult, result);
//		return {};
//	}
//
//	if (desc.data) {
//		upload(handle, desc.data, desc.size, 0);
//	}
//
//	Result::setResult(outResult, Result());
//
//	return { this, handle };
//
//};
