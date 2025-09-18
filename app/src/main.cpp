#pragma once
#define VMA_DEBUG_INITIALIZE_ALLOCATIONS 1
#define VMA_DEBUG_DETECT_CORRUPTION 1
#define VMA_ASSERT(expr) assert(expr)
# define ENGINE_NAME "VulkanToy"

#include <cassert>

// ---- SETUP -----

#include <Engine/Vulkan/vulkan_setup.h>

#define VMA_IMPLEMENTATION

#define VMA_VULKAN_VERSION 1003800 // Vulkan 1.4
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION

// ------------------------------------

#include <Engine/Vulkan/vulkan_helpers.h>
#include <Engine/Memory/handle.h>
#include <Engine/Memory/pool.h>

#include <Engine/Shared/common.h>

#include <Engine/Camera/camera.h>
#include <Engine/Scene/scene.h>
#include <Engine/Renderer/primitives.h>
#include <Engine/Utils/gltf.h>
#include <Engine/Memory/context.h>

// Moved readFile here. readFile rnow is just used to read shader files.
// this should go to Platform layer. This is windows first for the foreseable future
#include <Engine/fileIO.h>

// ------------------------------------

static constexpr uint32_t WIDTH = 1280;
static constexpr uint32_t HEIGHT = 720;

// Double buffering :)
static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

static constexpr float default_near_clip = 0.01f;
static constexpr float default_far_clip = 100.0f;

// Yes, I'm hardcapping here for development.
static constexpr uint32_t MAX_INSTANCES = 1000;
static constexpr uint32_t MAX_MESHES = 1000;
static constexpr uint32_t MAX_TEXTURES = 4096;
static constexpr uint32_t MAX_MATERIAL_INSTANCES = 1000;
static constexpr uint32_t MAX_MATERIALS = 1000;

// ------------------------------------

enum class BufferRole { UBO, SSBO };
// This is ok, but other messes should go to a pool for staging, then deleted after GPU upload;
std::vector<LoadedMesh> loaded_meshes_ = { Quad, Triangle };

// ------------------------------------

//// Scene Hieracrhy manages transform
//struct HostInstance {
//	// We can just use Handles, reserve untyped uint32_t indexing for GPU structs
//	/*uint32_t*/ MeshHandle mesh;
//	MaterialInstanceHandle material;
//	uint32_t scene_node;
//	uint32_t LOD;
//};

// ------------------------------------------------------------------------------------------------

// Dont like this. currently limited by the 16 byte stride.
// Maybe i could do away with it but that needs ext checking and enalbing and mods in shader code
struct alignas(16) IndexSlot {
	uint32_t idx;      // put the 16-bit value here (zero-extend)
	uint32_t pad[3];   // 16 bytes total per element
};
static_assert(sizeof(IndexSlot) % 16 == 0);

struct PushConstants {
	glm::mat4 view;
	glm::mat4 proj;
	// The MaterialPipeline should be binded to desired material. This only gets us bindless addresing of the instnanceSlots.
	// A single MaterialModel. shader code, layout , rastersettings, can have many instancs with individual image attachments and constants.
	int material_instance_index;
	uint32_t     _pad0[3];
};

// ------------------------------------------------------------------------------------------------
// 
// This is the neeeded Host side state to manage meshes.
struct Mesh {

	VmaAllocation allocation;
	VmaAllocationInfo allocInfo;
	// Im gonna have a standard format so no propreties for selecting tri, fan, or else
	// Static meshes are an open question yet. But my guesss is better to do that when i have the VFS, and the DAM, scene format and everything.
	// this app still does not distinguish between runtime, and else;

	uint32_t first_index = 0;
	uint32_t vert_offset = 0;

	// LOD should choose first index and voffset but lets keep this like this.... its not that clear yer how to manage that properly
	uint32_t LOD;

	// I prefer to manage material + attachentes via MaterialInstanceInstead.
	// but maybe i could have a ref for ez acces here?
	//MaterialInstanceHandle material_instance;

	// Maybe name
	//uint32_t name;

};

// Lets go with uniform_layouts ext. GENERAL and UNDEFINED
struct Texture {

	// to be able to free. or queryu memory info. Could be moved really.
	VmaAllocation allocation;
	VmaAllocationInfo allocInfo;

	VkImage image;
	VkImageView imageView;
	// This is far from good.
	VkSampler sampler;
	// Other properties and attributes

	// I could use debug name
	// Srgb/lienar others
	VkFormat format;

};

// Initialized with crude defaults;
struct RasterSettings {
	uint32_t depthClampEnable = VK_FALSE;
	uint32_t rasterizerDiscardEnable = VK_FALSE;
	uint32_t polygonMode = VK_POLYGON_MODE_FILL;
	uint32_t lineWidth = 1.0f;
	uint32_t cullMode = VK_CULL_MODE_BACK_BIT;
	uint32_t frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	uint32_t depthBiasEnable = VK_FALSE;
} default_raster_settings;

// Materials aabstractions area really PipelineObjets. Raster conf, shader objs, descriptorlayout. 
// MaterialAttachments must match desc layout, adn MaterialConstants pc layout
// So we bake the POS. VkPipelineCache not implemented now
// Another name fot his could be MatModel?
struct MaterialPipeline {

	// We need to hash these tree. but wont do it now
	RasterSettings raster_settings;
	ShaderModuleHandle vert;
	ShaderModuleHandle frag;

	// Lets not complitcate this for now. same footprint for everyone. 
	//VkDescriptorSetLayout* layout;
	// 
	// Lets just hold this here for now
	VkPipelineLayout plyt;

	// Bindless, so material descriptors sets are at this level instead of binding per instnace, so;
	// Invalid initialzoiation!.
	DescriptorSetHandle ds;
	VkPipeline pipeline;
};

// This gets uploaded to Device. stride contraints. no Handles!
// This will go with pushcontants. May require specialization per shader model
struct MaterialPushConstants {

	// Color *=
	glm::vec4 ambient = glm::vec4(0.0f);
	glm::vec4 diffuse = glm::vec4(0.0f);


	// Index to bindless texture array. Doing this like so obviously makes it so we only have one def for material texture slots
	uint32_t texAmbient = 0;
	uint32_t texDiffuse = 0;
	uint32_t texAlpha = 0;
	uint32_t padding[1];

} default_mat_pc;

static_assert(sizeof(MaterialPushConstants) % 16 == 0);

// MatModels or MatPipelines can have several instnaces with different vlaues for constans and textures.
// Raster state, vert, frag, blending and more stay ther same.
struct MaterialInstance {
	MaterialPipelineHandle pipeline;
	MaterialPushConstants values = {};
};

struct DeviceBufferDescription {

	VkBuffer buffer;
	VmaAllocation alloc;
	VmaAllocationInfo allocInfo;
	BufferRole role;
	// size already in allocInfo
	//VkDeviceSize size;

	// Did it end in mappable memory? This is important to know for proper upload. Discard the bool in the future use 
	VkMemoryPropertyFlags memory_flags;
	// Helpers to interrogate buffer
	bool is_mapapble() const {
		return this->memory_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	}

};

struct DescriptorSet {
	DescriptorLayoutHandle layout;
	VkDescriptorSet set;
};

void createGraphicsPipeline(
	VkDevice* device,
	VkShaderModule vert,
	VkShaderModule frag,
	VkPipelineLayout* pipelineLayout,
	VkDescriptorSetLayout* descriptorSetLayouts,
	VkFormat depth_format,
	VkFormat swapchainImageFormat,
	VkPipeline& graphicsPipeline
) {

	// SHADER

	VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
	vertShaderStageInfo.sType =
		VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
	vertShaderStageInfo.module = vert;
	vertShaderStageInfo.pName = "mainVS";

	VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
	fragShaderStageInfo.sType =
		VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	fragShaderStageInfo.module = frag;
	fragShaderStageInfo.pName = "mainPS";

	VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo,
													  fragShaderStageInfo };

	// VERTEX FORMAT

	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType =
		VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	auto bindingDescription = Vertex::getBindingDescription();
	auto attributeDescriptions = Vertex::getAttributeDescriptions();

	vertexInputInfo.vertexBindingDescriptionCount = 1;
	vertexInputInfo.vertexAttributeDescriptionCount =
		static_cast<uint32_t>(attributeDescriptions.size());
	vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
	vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType =
		VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	inputAssembly.primitiveRestartEnable = VK_FALSE;

	// 

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	//

	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType =
		VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.depthClampEnable = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizer.lineWidth = 1.0f;
	rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
	rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizer.depthBiasEnable = VK_FALSE;

	//

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType =
		VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.sampleShadingEnable = VK_FALSE;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	//

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType =
		VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.stencilTestEnable = VK_FALSE;

	//

	VkPipelineColorBlendAttachmentState colorBlendAttachment{};
	colorBlendAttachment.colorWriteMask =
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	colorBlendAttachment.blendEnable = VK_FALSE;

	VkPipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.sType =
		VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.logicOpEnable = VK_FALSE;
	colorBlending.logicOp = VK_LOGIC_OP_COPY;
	colorBlending.attachmentCount = 1;
	colorBlending.pAttachments = &colorBlendAttachment;
	colorBlending.blendConstants[0] = 0.0f;
	colorBlending.blendConstants[1] = 0.0f;
	colorBlending.blendConstants[2] = 0.0f;
	colorBlending.blendConstants[3] = 0.0f;

	// These two are elemental. There are extensions for almost anything do. Maybe it is woth to go all in dynamic state.
	std::vector<VkDynamicState> dynamicStates = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};

	VkPipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount =
		static_cast<uint32_t>(dynamicStates.size());
	dynamicState.pDynamicStates = dynamicStates.data();

#pragma region ---[Push Constants]---

	constexpr uint32_t kPC_Total = 192;

	VkPushConstantRange range{};
	range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	range.offset = 0;
	range.size = kPC_Total;

	VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = &descriptorSetLayouts[0];
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &range;


	if (vkCreatePipelineLayout(*device, &layoutInfo, nullptr,
		pipelineLayout) != VK_SUCCESS) {
		throw std::runtime_error("failed to create pipeline layout!");
	}

#pragma endregion

	// Provide information for dynamic rendering
	VkPipelineRenderingCreateInfo pipeline_create{
		VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
	pipeline_create.pNext = VK_NULL_HANDLE;

	// We are using the single color buffer out.
	pipeline_create.colorAttachmentCount = 1;
	pipeline_create.pColorAttachmentFormats = &swapchainImageFormat;

	pipeline_create.depthAttachmentFormat = depth_format;
	pipeline_create.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = 2;
	pipelineInfo.pStages = shaderStages;
	pipelineInfo.pVertexInputState = &vertexInputInfo;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.layout = *pipelineLayout;
	pipelineInfo.pNext = &pipeline_create;

	if (vkCreateGraphicsPipelines(*device, VK_NULL_HANDLE, 1, &pipelineInfo,
		nullptr, &graphicsPipeline) != VK_SUCCESS) {
		throw std::runtime_error("failed to create graphics pipeline!");
	}

}



// This style of handling things is lifted up from LightweightVk.
class Context {

	// 1 allocator per device. Device is implicit in allocator...
	VmaAllocator* allocator_;
	VkDevice* device_;

	// These three things are primary citizens of Device Memory Residency
	VmaPool D_2DImagesPool;
	VmaPool D_ShaderPool;

	// Different pools for differnet uses: ...Skinned. Static. Dynamic
	VmaPool D_geo;
	VmaPool D_DynamicMeshPool;
	VmaPool D_StagingPool;

public:

	DescriptorPoolHandle default_descriptor_pool;
	DescriptorLayoutHandle default_descriptorLayout;

	// We neeed VkDescriptorPool pools here
	// We move from VkDp to another because its cheaper to reset them than to evict particular sets.
	// Same with CommandPools. I have a stupid simple design now with just 1 CommandPool.
	Pool<DeviceBufferDescription, DeviceBufferDescription> buffers_;
	Pool<Texture, Texture> textures_;
	Pool<Mesh, Mesh> meshes_;


	// These two have confusing names;
	Pool<DescriptorSet, DescriptorSet> descriptrorSetPool_;
	Pool<DescriptorPool, VkDescriptorPool> descriptorSetPoolPool_;

	Pool<DescriptorLayout, VkDescriptorSetLayout> descriptorSetLayoutPool_;
	Pool<MaterialPipeline, MaterialPipeline> materialPipelinePool_;
	Pool<MaterialInstance, MaterialInstance> materialInstancesPool_;

	Pool<ShaderModule, VkShaderModule> shaderModulePool_;

	Context(VmaAllocator* allocator, VkDevice* device) {
		allocator_ = allocator;
		device_ = device;

		// Lets reserve upfront.
		textures_.objects_.reserve(MAX_TEXTURES);

		//materialPipelinePool_.objects_.reserve(1000);
		// This is a small reservation. just testing 
		buffers_.objects_.reserve(1000);

		InitializeDeviceVmaPools();

		// Lets have one dpool
		default_descriptor_pool = createDescriptorPool();
		default_descriptorLayout = createDescriptorLayout();


	};
	// Maybe destructor should clear the vmaPools. Device side allocations are not managed as clear by the OS.
	~Context() {};

	void InitializeDeviceVmaPools() {

		// DEVICE-LOCAL pool for 2D images
		{
			VkImageCreateInfo imgCI{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
			imgCI.imageType = VK_IMAGE_TYPE_2D;
			imgCI.format = VK_FORMAT_R8G8B8A8_UNORM;
			imgCI.extent = { 4, 4, 1 };
			imgCI.mipLevels = 1;
			imgCI.arrayLayers = 1;
			imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
			imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
			imgCI.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

			VmaAllocationCreateInfo aCI{};
			aCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

			uint32_t typeIndex = UINT32_MAX;
			VK_CHECK(vmaFindMemoryTypeIndexForImageInfo(*allocator_, &imgCI, &aCI, &typeIndex));

			VmaPoolCreateInfo pci{};
			pci.memoryTypeIndex = typeIndex;
			pci.blockSize = 64 * 1024 * 1024;
			VK_CHECK(vmaCreatePool(*allocator_, &pci, &D_2DImagesPool));
		}

		// DEVICE-LOCAL pool for static geometry buffers
		{
			VkBufferCreateInfo bufCI{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			bufCI.size = 64 * 1024; // dummy
			bufCI.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

			VmaAllocationCreateInfo aCI{};
			aCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

			uint32_t typeIndex = UINT32_MAX;
			VK_CHECK(vmaFindMemoryTypeIndexForBufferInfo(*allocator_, &bufCI, &aCI, &typeIndex));

			VmaPoolCreateInfo pci{};
			pci.memoryTypeIndex = typeIndex;
			pci.blockSize = 64 * 1024 * 1024;
			VK_CHECK(vmaCreatePool(*allocator_, &pci, &D_geo));
		}

		{
			VkBufferCreateInfo bufCI{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			bufCI.size = 64 * 1024;
			bufCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

			VmaAllocationCreateInfo aCI{};
			aCI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
				| VMA_ALLOCATION_CREATE_MAPPED_BIT;
			aCI.usage = VMA_MEMORY_USAGE_AUTO;

			uint32_t typeIndex = UINT32_MAX;
			vmaFindMemoryTypeIndexForBufferInfo(*allocator_, &bufCI, &aCI, &typeIndex);

			VmaPoolCreateInfo pci{};
			pci.memoryTypeIndex = typeIndex; // host-visible & host-coherent type
			pci.blockSize = 32 * 1024 * 1024;
			VK_CHECK(vmaCreatePool(*allocator_, &pci, &D_StagingPool));
		}


	}

	MaterialPipelineHandle createMaterialPipeline(ShaderModuleHandle vert, ShaderModuleHandle frag, RasterSettings raster_settings, VkFormat depth_format, VkFormat swapchainImageFormat);
	MaterialInstanceHandle createMaterialInstance(MaterialPipelineHandle mat_pipeline);

	TextureHandle createTextureImage(std::string image_path, VkFormat format);
	ShaderModuleHandle createShaderModule(std::vector<char> code);
	DescriptorLayoutHandle createDescriptorLayout();
	DescriptorPoolHandle createDescriptorPool();
	DescriptorSetHandle createDescriptorSet(DescriptorLayoutHandle lyt);
	DeviceBufferDescriptionHandle createBuffer(uint32_t size, VmaMemoryUsage vma_usage, VkBufferUsageFlags usage_flags);

	void UploadDataToBuffer(VkCommandBuffer cmdBuf, DeviceBufferDescriptionHandle buffer, void* data, size_t dataSize, VkPipelineStageFlags dstStages);

};

//void Context::UpdateDescriptorSet(DescriptorSetHandle, void* data, uint32_t size) {
//
//	// this depends on lyt as we see.
//	std::array<VkWriteDescriptorSet, 2> descriptorWrites{};
//
//	descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
//	descriptorWrites[0].dstSet = frameDescriptorSets[i];
//	descriptorWrites[0].dstBinding = 0;
//	descriptorWrites[0].dstArrayElement = 0;
//	descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
//	descriptorWrites[0].descriptorCount = 1;
//	descriptorWrites[0].pBufferInfo = &instanceIndexBufferInfo;
//
//	descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
//	descriptorWrites[1].dstSet = frameDescriptorSets[i];
//	descriptorWrites[1].dstBinding = 1;
//	descriptorWrites[1].dstArrayElement = 0;
//	descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
//	descriptorWrites[1].descriptorCount = 1;
//	descriptorWrites[1].pBufferInfo = &instanceBufferInfo;
//
//	vkUpdateDescriptorSets(device,
//		static_cast<uint32_t>(descriptorWrites.size()),
//		descriptorWrites.data(),
//		0,
//		nullptr
//	);
//}

DescriptorSetHandle Context::createDescriptorSet(
	DescriptorLayoutHandle lyt
) {
	// Lets make pool selection automatic
	auto descriptorPool = descriptorSetPoolPool_.get(default_descriptor_pool);
	auto layout = descriptorSetLayoutPool_.get(lyt);

	std::array<VkDescriptorSetLayout, 1> layouts = {
		*layout
	};

	VkDescriptorSetAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	// This needs to be implemented using rotating pools.
	allocInfo.descriptorPool = *descriptorPool;
	allocInfo.descriptorSetCount = 1;
	//allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

	allocInfo.pSetLayouts = layouts.data();

	DescriptorSet ds;
	VkResult result = vkAllocateDescriptorSets(*device_, &allocInfo, &ds.set);
	ds.layout = lyt;

	if (result == VK_ERROR_OUT_OF_POOL_MEMORY) {
		// Rotate the descriptor pool. try again
		// Manage live dspools. defrag
	}

	if (result != VK_SUCCESS) {
		throw std::runtime_error("failed to allocate descriptor sets!");
	}

	return descriptrorSetPool_.create(std::move(ds));

}

DeviceBufferDescriptionHandle Context::createBuffer(uint32_t size, VmaMemoryUsage vma_usage, VkBufferUsageFlags usage_flags) {
	DeviceBufferDescription desc{};

	VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	ci.size = size;
	ci.usage = usage_flags;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo aci{};
	aci.usage = vma_usage;
	// Optionally choose pools here based on usage_flags/usage

	// i need to assert this!
	vmaCreateBuffer(*allocator_, &ci, &aci, &desc.buffer, &desc.alloc, &desc.allocInfo);

	// Query memory properties to know if it’s host-visible:
	VkMemoryPropertyFlags props{};
	vmaGetAllocationMemoryProperties(*allocator_, desc.alloc, &props);
	desc.memory_flags = props;

	// Infer role from usage flags
	if (usage_flags & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) desc.role = BufferRole::UBO;
	else if (usage_flags & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) desc.role = BufferRole::SSBO;
	else desc.role = BufferRole::SSBO; // default or add a "Other" role

	return buffers_.create(std::move(desc));
}

void Context::UploadDataToBuffer(
	VkCommandBuffer cmdBuf,
	DeviceBufferDescriptionHandle buffer,
	void* data,
	size_t dataSize,
	VkPipelineStageFlags dstStages)
{
	DeviceBufferDescription* bufferDesc = buffers_.get(buffer);
	const VkAccessFlags dstAccess =
		(bufferDesc->role == BufferRole::UBO) ? VK_ACCESS_UNIFORM_READ_BIT
		: VK_ACCESS_SHADER_READ_BIT;

	if (bufferDesc->is_mapapble()) {
		// Direct copy into mapped allocation
		VkResult result = vmaCopyMemoryToAllocation(*allocator_, data, bufferDesc->alloc, 0, dataSize);
		assert(result == VK_SUCCESS);

		// Host write -> shader read
		VkBufferMemoryBarrier b{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
		b.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
		b.dstAccessMask = dstAccess;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.buffer = bufferDesc->buffer;
		b.offset = 0;
		b.size = VK_WHOLE_SIZE;

		vkCmdPipelineBarrier(
			cmdBuf,
			VK_PIPELINE_STAGE_HOST_BIT,
			dstStages,
			0,
			0, nullptr,
			1, &b,
			0, nullptr);
		return;
	}

	// Device-local path: stage the caller's data
	VkBuffer stagingBuf; VmaAllocation stagingAlloc;
	VkBufferCreateInfo sInfo{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = dataSize,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	};
	VmaAllocationCreateInfo sAlloc{};
	sAlloc.usage = VMA_MEMORY_USAGE_AUTO;
	sAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
	sAlloc.pool = D_StagingPool;

	vmaCreateBuffer(*allocator_, &sInfo, &sAlloc, &stagingBuf, &stagingAlloc, nullptr);

	// Fill the staging buffer with the *provided* data
	VkResult result = vmaCopyMemoryToAllocation(*allocator_, data, stagingAlloc, 0, dataSize);
	assert(result == VK_SUCCESS);

	// Copy staging -> dst
	VkBufferCopy copyRegion{ .size = dataSize };
	vkCmdCopyBuffer(cmdBuf, stagingBuf, bufferDesc->buffer, 1, &copyRegion);

	// Make the copy visible to shaders
	VkBufferMemoryBarrier b2{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
	b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b2.dstAccessMask = dstAccess;
	b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b2.buffer = bufferDesc->buffer;
	b2.offset = 0;
	b2.size = VK_WHOLE_SIZE;

	vkCmdPipelineBarrier(
		cmdBuf,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		dstStages,
		0,
		0, nullptr,
		1, &b2,
		0, nullptr);

	vmaDestroyBuffer(*allocator_, stagingBuf, stagingAlloc);
}

// Lets just use 1 layout for now. We will need more shapes for shadows and others is my guess.
DescriptorLayoutHandle Context::createDescriptorLayout() {
	// INDEX DATA
	VkDescriptorSetLayoutBinding instanceIndexBinding{};
	instanceIndexBinding.binding = 0;
	instanceIndexBinding.descriptorCount = 1;
	instanceIndexBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	instanceIndexBinding.pImmutableSamplers = nullptr;
	instanceIndexBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	VkDescriptorSetLayoutBinding instanceDataBinding{};
	instanceDataBinding.binding = 1;
	instanceDataBinding.descriptorCount = 1;
	instanceDataBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	instanceDataBinding.pImmutableSamplers = nullptr;
	instanceDataBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	VkDescriptorSetLayoutBinding sampledImageBinding{};
	sampledImageBinding.binding = 2;
	sampledImageBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	sampledImageBinding.descriptorCount = MAX_TEXTURES;
	sampledImageBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutBinding sampBinding{};
	sampBinding.binding = 3;
	sampBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	sampBinding.descriptorCount = MAX_TEXTURES;
	sampBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	std::array<VkDescriptorSetLayoutBinding, 4> bindings = {
		instanceIndexBinding,
		instanceDataBinding,
		//
		sampledImageBinding,
		sampBinding,
	};

	// Per-binding flags — same size as bindings[]
	std::array<VkDescriptorBindingFlags, 4> bindingFlags = {
		0,
		0,
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // bindless textures
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT // bindless samplers
	};

	// Chain flags struct
	VkDescriptorSetLayoutBindingFlagsCreateInfo bindless_flags{};
	bindless_flags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
	bindless_flags.pNext = nullptr;
	bindless_flags.bindingCount = static_cast<uint32_t>(bindingFlags.size());
	bindless_flags.pBindingFlags = bindingFlags.data();

	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
	layoutInfo.pBindings = bindings.data();
	layoutInfo.pNext = &bindless_flags;
	layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;

	VkDescriptorSetLayout descriptorSetLayout;

	if (vkCreateDescriptorSetLayout(*device_, &layoutInfo, nullptr,
		&descriptorSetLayout) != VK_SUCCESS) {
		throw std::runtime_error("failed to create descriptor set layout!");
	}

	auto r = descriptorSetLayoutPool_.create(std::move(descriptorSetLayout));
	return r;

}

DescriptorPoolHandle Context::createDescriptorPool() {
	const uint32_t bindless_sets = MAX_FRAMES_IN_FLIGHT;
	const uint32_t max_textures = MAX_TEXTURES;

	const uint32_t total_sampled_images = bindless_sets * max_textures;
	const uint32_t total_samplers = bindless_sets * max_textures;

	std::array<VkDescriptorPoolSize, 4> poolSizes = { {
	  { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,   2u * MAX_FRAMES_IN_FLIGHT },
	  { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,   2u * MAX_FRAMES_IN_FLIGHT },
	  { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,    total_sampled_images      },
	  { VK_DESCRIPTOR_TYPE_SAMPLER,          total_samplers            },
	} };

	VkDescriptorPoolCreateInfo poolInfo{ };
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = 1000;
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT; // required
	poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
	poolInfo.pPoolSizes = poolSizes.data();

	VkDescriptorPool dpool;
	VK_CHECK(vkCreateDescriptorPool(*device_, &poolInfo, nullptr, &dpool));
	return descriptorSetPoolPool_.create(std::move(dpool));
}

MaterialPipelineHandle Context::createMaterialPipeline(ShaderModuleHandle vert, ShaderModuleHandle frag, RasterSettings raster_settings, VkFormat depth_format, VkFormat swapchainImageFormat) {

	// We ure using native VkPipelineLayout to ease things for now so we dont have to formalize a way to describe that.
	MaterialPipeline mat_pipe = {};
	mat_pipe.raster_settings = default_raster_settings;
	mat_pipe.frag = frag;
	mat_pipe.vert = vert;

	auto v = shaderModulePool_.get(vert);
	auto f = shaderModulePool_.get(frag);

	// Lets use default
	auto def_lyt = descriptorSetLayoutPool_.get(default_descriptorLayout);

	std::array<VkDescriptorSetLayout, 1> lyts = {
		*def_lyt
		// Thats the default. we need at least that for Instance Data.
		// We could add a second lyt ot have different layoitus per MaterialModel/MaterialPipeline
		//mat_pipe.lyt
	};

	createGraphicsPipeline(
		device_,
		*v,
		*f,
		&mat_pipe.plyt,
		def_lyt,
		depth_format,
		swapchainImageFormat,
		mat_pipe.pipeline
	);

	return materialPipelinePool_.create(std::move(mat_pipe));

}

MaterialInstanceHandle Context::createMaterialInstance(MaterialPipelineHandle mat_pipeline)
{
	MaterialInstance mat_instance = {};
	mat_instance.pipeline = mat_pipeline;

	return materialInstancesPool_.create(std::move(mat_instance));
}

TextureHandle Context::createTextureImage(std::string image_path, VkFormat format) {

	int texWidth, texHeight, texChannels;

	stbi_uc* pixels = stbi_load(image_path.c_str(), &texWidth, &texHeight,
		&texChannels, STBI_rgb_alpha);

	VkDeviceSize imageSize = texWidth * texHeight * 4;

	if (!pixels) {
		throw std::runtime_error("failed to load texture image!");
	}

	VkImageCreateInfo imgCI{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imgCI.imageType = VK_IMAGE_TYPE_2D;
	imgCI.format = format;               // pick your format
	imgCI.extent = { static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight), 1 };                      // width, height, depth
	imgCI.mipLevels = 1;                                      // >1 if you want mipmaps
	imgCI.arrayLayers = 1;                                      // >1 for arrays/cubemaps
	imgCI.samples = VK_SAMPLE_COUNT_1_BIT;                  // MSAA? set >1
	imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;                // GPU-friendly
	imgCI.usage = VK_IMAGE_USAGE_SAMPLED_BIT              // sample as texture
		| VK_IMAGE_USAGE_TRANSFER_DST_BIT;        // upload via copy
	imgCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;              // we’ll transition later. Well jus use transition General always

	VmaAllocationCreateInfo aCI{};
	aCI.usage = VMA_MEMORY_USAGE_GPU_ONLY; // should be compatible with the pool’s memoryType
	aCI.pool = D_2DImagesPool;                      // <--- CRITICAL: place this allocation in the pool

	VkImage image = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	VmaAllocationInfo allocInfo{};

	VkResult res = vmaCreateImage(*allocator_, &imgCI, &aCI, &image, &alloc, &allocInfo);
	if (res != VK_SUCCESS) {
		throw std::runtime_error("vmaCreateImage (in pool) failed");
	}

	Texture tex{};
	tex.allocation = alloc;
	tex.format = format;
	tex.image = image;
	//tex.imageView

	TextureHandle imgHandle = textures_.create(std::move(tex));

	return imgHandle;

}

ShaderModuleHandle Context::createShaderModule(std::vector<char> code) {

	auto shader = createDeviceShaderModule(
		*device_,
		std::move(code)
	);

	return shaderModulePool_.create(std::move(shader));

};

// Need device allocation freeing
void destroy(Context* ctx, TextureHandle handle) {

	// We need to free from device pool.
	ctx->textures_.destroy(handle);
};

void destroy(Context* ctx, DeviceBufferDescriptionHandle handle) {

	// We need to freee device allocation
	ctx->buffers_.destroy(handle);

};

class VulkanApp {

public:

	int width_ = WIDTH;
	int height_ = HEIGHT;

	bool Run();

private:


	Context* context;

	DescriptorSetHandle frameDescriptorSets[MAX_FRAMES_IN_FLIGHT];

	//  Defaulting to empty scene
	Scene* scene_ = new Scene();

	InstanceData instance_data_[MAX_INSTANCES];
	uint32_t instance_count_ = 10;

	// 1 allocator per device and instance. =)
	// So just one
	VmaAllocator app_allocator_;

	VkBuffer static_vertex_buffer_;
	VmaAllocation static_vertex_allocation_;

	VkBuffer static_index_buffer_;
	VmaAllocation static_index_allocation_;

	// I doubt this is really needed.
	VkFence f_level_ready_;


	// _---------------

	VkQueue graphicsQueue_;
	VkQueue computeQueue_;
	VkQueue presentQueue_;
	VkQueue transferQueue_;

#pragma region ===[ App / Core State ]===

	float delta_time_;

	// This is bad. We coupled View with app...
	// Although i like the single cameraView model for an app. Lets not complicate setup
	// I dont want to stablish layouts for multicamera view. then compose ui on top
	// Memory was already split to Context.
	Camera camera_ = Camera(cameraPositioner);

#pragma endregion

#pragma region ===[ Swapchain ]===

	VkSwapchainKHR swapChain;
	uint32_t swapchainImageCount;
	VkImage* swapChainImages;
	VkImageView* swapChainImageViews;

	VkFormat swapChainImageFormat;
	VkExtent2D swapChainExtent;

#pragma endregion

	// Main depth image is the one for the main view.
	VkImage main_depthImage;
	VkDeviceMemory main_depthImageMemory;
	VkImageView main_depthImageView;
	VmaAllocation main_d_alloc;

#pragma region ===[ GLFW / Window State ]===
	GLFWwindow * window;
#pragma endregion

#pragma region ===[ Vulkan Instance / Device / Queues ]===
	VkInstance instance;
	VkDebugUtilsMessengerEXT debugMessenger;
	VkSurfaceKHR surface;

	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice device;
#pragma endregion

	// We need to move this to context;
#pragma region ===[ Uniforms / Descriptor Sets ]==

	// These should be moved to Context
	DeviceBufferDescriptionHandle uniforms_[MAX_FRAMES_IN_FLIGHT];
	DeviceBufferDescriptionHandle instanceDataUniforms_[MAX_FRAMES_IN_FLIGHT];
	DeviceBufferDescriptionHandle instanceIndexUniforms_[MAX_FRAMES_IN_FLIGHT];

#pragma endregion

#pragma region ===[ Pipeline / Render Resources ]===
	// We separate command pools for each frames becuase its cheaper to reset them this way
	VkCommandPool framesCommandPool[MAX_FRAMES_IN_FLIGHT];
	VkCommandBuffer commandBuffers[MAX_FRAMES_IN_FLIGHT];

#pragma endregion

	// Ok, yes. lets do sync on app code. No framegraph yet :)
#pragma region ===[ Synchronization Primitives ]===

	VkSemaphore * renderFinishedSemaphores;
	VkSemaphore imageAvailableSemaphores[MAX_FRAMES_IN_FLIGHT];
	VkFence inFlightFences[MAX_FRAMES_IN_FLIGHT];

	uint32_t currentFrame = 0;
#pragma endregion

#pragma region ===[ Input / Camera State ]===
	glm::vec3 last_camera_pos;
	bool rightMousePressed = false;
	bool firstMouse;
	float lastX, lastY;
	int currentKey = -1; // Keeps track of the currently pressed key (W, A, S, D)
#pragma endregion

#pragma region ===[ Flags / Resize Events ]===
	bool framebufferResized = false;
#pragma endregion


	void InitVulkanApp();
	void CreateDefaultMaterialPipeline();
	void LoadScene();
	void UploadMeshData();

	void UpdateDeltaTime() {

		// Its a bad idea to have this across the fucntion border. Latency sensitive.
		static auto startTime = std::chrono::high_resolution_clock::now();
		auto currentTime = std::chrono::high_resolution_clock::now();
		delta_time_ = std::chrono::duration<float, std::chrono::seconds::period>(
			currentTime - startTime)
			.count();
	}

	void UpdateInstanceBuffer(uint32_t currentImage) {

		// THIS MUST REMAIN HERE. SENSITIVE TO LAG OTHERWISE
		static auto startTime = std::chrono::high_resolution_clock::now();
		auto currentTime = std::chrono::high_resolution_clock::now();
		delta_time_ = std::chrono::duration<float, std::chrono::seconds::period>(
			currentTime - startTime)
			.count();

		// Moved this to the HEAP. This will be state of the app instead of being fabricated here anyways.
		InstanceData* data = new InstanceData[MAX_INSTANCES];
		int i = 0;
		//for (const glm::mat4 tr : scene_->globalTransform) {
		for (const glm::mat4 tr : scene_->localTransform) {
			std::memcpy(
				data[i].transform,
				glm::value_ptr(tr),
				sizeof(float) * 16);

			data[i].color = { i * 0.20f,0.3f,-i * 0.7f,1.0f };

			i++;
		}

		std::array<uint16_t, MAX_INSTANCES> indexData = {};
		for (uint16_t index = 0; index < MAX_INSTANCES; index++) {
			indexData[index] = index;
		}

		std::vector<IndexSlot> indexSlotData(MAX_INSTANCES);
		for (size_t i = 0; i < MAX_INSTANCES; ++i) {
			indexSlotData[i].idx = static_cast<uint32_t>(indexData[i]); // zero-extend
		}

		VkResult result{};

		// This is bad, will stall. Frame update should use same command buffer for all update operations.
		// FrameGraph! FrameGraph!
		VkCommandBuffer cmdBuf = beginSingleTimeCommands();

		static_assert(sizeof(IndexSlot) * MAX_INSTANCES < (1 << 16),
			"Instance buffer too large for 64KB UBO limit");

		context->UploadDataToBuffer(
			cmdBuf,
			instanceIndexUniforms_[currentImage],
			indexSlotData.data(),
			sizeof(IndexSlot) * MAX_INSTANCES,
			VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
		);

		context->UploadDataToBuffer(
			cmdBuf,
			instanceDataUniforms_[currentImage],
			data,
			sizeof(InstanceData) * MAX_INSTANCES,
			VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
		);

		endSingleTimeCommands(cmdBuf);
		delete[] data;
	}

	void mainLoop() {

		auto lastTime = std::chrono::high_resolution_clock::now();

		while (!glfwWindowShouldClose(window)) {
			glfwPollEvents();

			auto now = std::chrono::high_resolution_clock::now();
			float delta = std::chrono::duration<float>(now - lastTime).count();
			lastTime = now;

			double x, y;
			glfwGetCursorPos(window, &x, &y);
			cameraPositioner.update(delta,
				glm::vec2(x / width_, y / -height_),
				rightMousePressed);

			//std::cout << camera_.getPosition() << std::endl;

			// Synthetic movement
			for (size_t i = 0; i < scene_->localTransform.size(); ++i) {
				glm::mat4& local = scene_->localTransform[i];
				float phase = std::sin(i + delta_time_ * 0.35f);  // staggered phases
				local[3][0] = static_cast<float>(i) * 1.4f + phase;
				markAsChanged(*scene_, i);
			}
			recalculateGlobalTransforms(*scene_);

			drawFrame();

		}

		vkDeviceWaitIdle(device);
	}

	void cleanupSwapChain() {

		vkDestroyImageView(device, main_depthImageView, nullptr);
		vmaDestroyImage(app_allocator_, main_depthImage, main_d_alloc);

		for (uint32_t i = 0; i < swapchainImageCount; ++i) {
			vkDestroyImageView(device, swapChainImageViews[i], nullptr);
		}
		delete[] swapChainImageViews;

		delete[] swapChainImages;
		swapChainImageViews = nullptr;

		vkDestroySwapchainKHR(device, swapChain, nullptr);
	}

	void recreateSwapChain() {

		int width = 0, height = 0;
		glfwGetFramebufferSize(window, &width, &height);
		while (width == 0 || height == 0) {
			glfwGetFramebufferSize(window, &width, &height);
			glfwWaitEvents();
		}

		width_ = width;
		height_ = height;

		vkDeviceWaitIdle(device);

		cleanupSwapChain();
		createSwapChain();
		createSwapchainImageViews();
		createDepthResources();

		// ---- update camera projection to match new size ----
		float newAspect = float(width) / float(height);
		camera_.setPerspective(45.0f, newAspect, default_near_clip, default_far_clip);
	}

	void setupDebugMessenger() {
		if (!enableValidationLayers) return;

		VkDebugUtilsMessengerCreateInfoEXT createInfo;
		populateDebugMessengerCreateInfo(createInfo);

		if (CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr,
			&debugMessenger) != VK_SUCCESS) {
			throw std::runtime_error("failed to set up debug messenger!");
		}
	}

	void createSwapChain() {
		SwapChainSupportDetails swapChainSupport =
			querySwapChainSupport(physicalDevice, surface);

		VkSurfaceFormatKHR surfaceFormat =
			chooseSwapSurfaceFormat(swapChainSupport.formats);
		VkPresentModeKHR presentMode =
			chooseSwapPresentMode(swapChainSupport.presentModes);
		VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities, window);

		swapchainImageCount = swapChainSupport.capabilities.minImageCount + 1;
		swapChainImages = new VkImage[swapchainImageCount];
		swapChainImageViews = new VkImageView[swapchainImageCount];

		if (swapChainSupport.capabilities.maxImageCount > 0 &&
			swapchainImageCount > swapChainSupport.capabilities.maxImageCount) {
			swapchainImageCount = swapChainSupport.capabilities.maxImageCount;
		}

		VkSwapchainCreateInfoKHR createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		createInfo.surface = surface;

		createInfo.minImageCount = swapchainImageCount;
		createInfo.imageFormat = surfaceFormat.format;
		createInfo.imageColorSpace = surfaceFormat.colorSpace;
		createInfo.imageExtent = extent;
		createInfo.imageArrayLayers = 1;
		createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

		QueueFamilyIndices indices = findQueueFamilies(physicalDevice, surface);
		uint32_t queueFamilyIndices[] = { indices.graphicsFamily.value(),
										 indices.presentFamily.value() };

		if (indices.graphicsFamily != indices.presentFamily) {
			createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
			createInfo.queueFamilyIndexCount = 2;
			createInfo.pQueueFamilyIndices = queueFamilyIndices;
		}
		else {
			createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		}

		createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
		createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		createInfo.presentMode = presentMode;
		createInfo.clipped = VK_TRUE;

		if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapChain) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create swap chain!");
		}

		vkGetSwapchainImagesKHR(device, swapChain, &swapchainImageCount, nullptr);
		vkGetSwapchainImagesKHR(device, swapChain, &swapchainImageCount, swapChainImages);

		swapChainImageFormat = surfaceFormat.format;
		swapChainExtent = extent;
	}

	void createSwapchainImageViews() {

		for (uint32_t i = 0; i < swapchainImageCount; ++i) {

			VkImageView view_;

			createImageView(
				swapChainImages[i], swapChainImageFormat, VK_IMAGE_ASPECT_COLOR_BIT, view_
			);

			swapChainImageViews[i] = view_;
		}

	}


	void createDepthResources() {
		VkFormat depthFormat = findDepthFormat(physicalDevice);

		createImage(
			swapChainExtent.width,
			swapChainExtent.height,
			depthFormat,
			VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			main_depthImage
			//main_d_alloc
		);

		createImageView(main_depthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, main_depthImageView);
	}

	// --------------------------

	TextureHandle createTextureImage(std::string texture_path, VkFormat format) {
		int texWidth, texHeight, texChannels;

		stbi_uc* pixels = stbi_load(texture_path.c_str(), &texWidth, &texHeight,
			&texChannels, STBI_rgb_alpha);

		VkDeviceSize imageSize = texWidth * texHeight * 4;

		if (!pixels) {
			throw std::runtime_error("failed to load texture image!");
		}

		VkBuffer stagingBuffer;
		VkDeviceMemory stagingBufferMemory;
		createBuffer(
			imageSize,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			stagingBuffer,
			stagingBufferMemory);

		void* data;
		vkMapMemory(device, stagingBufferMemory, 0, imageSize, 0, &data);
		memcpy(data, pixels, static_cast<size_t>(imageSize));
		vkUnmapMemory(device, stagingBufferMemory);

		stbi_image_free(pixels);

		auto texHandle = context->createTextureImage(texture_path, format);
		auto texture_image = context->textures_.get(texHandle);

		//createImage(
		//	texWidth,
		//	texHeight,
		//	VK_FORMAT_R8G8B8A8_SRGB,
		//	VK_IMAGE_TILING_OPTIMAL,
		//	VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		//	VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		//	*texture_image
		//);

		transitionImageLayout(
			texture_image->image,
			VK_FORMAT_R8G8B8A8_SRGB,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		copyBufferToImage(stagingBuffer,
			texture_image->image,
			static_cast<uint32_t>(texWidth),
			static_cast<uint32_t>(texHeight)
		);

		// We shoudl go just to general really;
		transitionImageLayout(
			texture_image->image,
			VK_FORMAT_R8G8B8A8_SRGB,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		);

		vkDestroyBuffer(device, stagingBuffer, nullptr);
		vkFreeMemory(device, stagingBufferMemory, nullptr);
		return texHandle;

	}

	void createTextureImageView(const VkImage* textureImage, VkImageView* textureImageView, VkFormat format) {
		createImageView(
			*textureImage,
			format,
			//VK_FORMAT_R8G8B8A8_SRGB,
			VK_IMAGE_ASPECT_COLOR_BIT,
			*textureImageView
		);
	}

	void createTextureSampler(VkSampler* textureSampler) {
		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;

		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(physicalDevice, &properties);

		samplerInfo.anisotropyEnable = VK_TRUE;
		samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
		samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;

		assert(properties.limits.maxUniformBufferRange >= 1 << 16);

		samplerInfo.unnormalizedCoordinates = VK_FALSE;
		samplerInfo.compareEnable = VK_FALSE;
		samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;

		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.mipLodBias = 0.0f;
		samplerInfo.minLod = 0.0f;
		samplerInfo.maxLod = 0.0f;

		if (vkCreateSampler(device, &samplerInfo, nullptr, textureSampler) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create texture sampler!");
		}

	}

	void createImageView(
		VkImage image,
		VkFormat format,
		VkImageAspectFlags aspectFlags,
		VkImageView& imageView
	) {
		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = format;
		// viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.aspectMask = aspectFlags;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = 1;

		if (vkCreateImageView(device, &viewInfo, nullptr, &imageView) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create texture image view!");
		}

	}
	// Obly used now for depthImage.
	void createImage(
		uint32_t width,
		uint32_t height,
		VkFormat format,
		VkImageTiling tiling,
		VkImageUsageFlags usage,
		VkMemoryPropertyFlags properties,
		VkImage& image
	) {


		VkImageCreateInfo imgCreateInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		imgCreateInfo.imageType = VK_IMAGE_TYPE_2D;
		imgCreateInfo.extent.width = width;
		imgCreateInfo.extent.height = height;
		imgCreateInfo.extent.depth = 1;
		imgCreateInfo.mipLevels = 1;
		imgCreateInfo.arrayLayers = 1;
		imgCreateInfo.format = format;
		imgCreateInfo.tiling = tiling;
		//imgCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imgCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		//imgCreateInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		imgCreateInfo.usage = usage;
		imgCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;

		VmaAllocationCreateInfo allocCreateInfo = {};
		allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
		allocCreateInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
		allocCreateInfo.priority = 1.0f;

		VmaAllocation alloc;
		vmaCreateImage(app_allocator_, &imgCreateInfo, &allocCreateInfo, &image, &alloc, nullptr);
		//return alloc;
	}

	//// Obly used now for depthImage.
	//void createImage(
	//	uint32_t width,
	//	uint32_t height,
	//	VkFormat format,
	//	VkImageTiling tiling,
	//	VkImageUsageFlags usage,
	//	VkMemoryPropertyFlags properties,
	//	VkImage& image,
	//	VmaAllocation& allocation
	//) {

	//	VkImageCreateInfo imgCreateInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	//	imgCreateInfo.imageType = VK_IMAGE_TYPE_2D;
	//	imgCreateInfo.extent.width = width;
	//	imgCreateInfo.extent.height = height;
	//	imgCreateInfo.extent.depth = 1;
	//	imgCreateInfo.mipLevels = 1;
	//	imgCreateInfo.arrayLayers = 1;
	//	imgCreateInfo.format = format;
	//	imgCreateInfo.tiling = tiling;
	//	//imgCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	//	imgCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	//	//imgCreateInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	//	imgCreateInfo.usage = usage;
	//	imgCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;

	//	VmaAllocationCreateInfo allocCreateInfo = {};
	//	allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
	//	allocCreateInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
	//	allocCreateInfo.priority = 1.0f;

	//	VmaAllocation alloc;
	//	VK_CHECK(vmaCreateImage(app_allocator_, &imgCreateInfo, &allocCreateInfo, &image, &allocation, nullptr));
	//}

	void transitionImageLayout(VkImage image, VkFormat format,
		VkImageLayout oldLayout, VkImageLayout newLayout) {
		VkCommandBuffer commandBuffer = beginSingleTimeCommands();

		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;

		if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

			if (hasStencilComponent(format)) {
				barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
			}
		}
		else {
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		}

		VkPipelineStageFlags sourceStage;
		VkPipelineStageFlags destinationStage;

		if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
			newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
			barrier.srcAccessMask = 0;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

			sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		}
		else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
			newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

			sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		}
		else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
			newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
			barrier.srcAccessMask = 0;
			barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

			sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		}
		else {
			throw std::invalid_argument("unsupported layout transition!");
		}

		vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0,
			nullptr, 0, nullptr, 1, &barrier);

		endSingleTimeCommands(commandBuffer);
	}

	void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width,
		uint32_t height) {
		VkCommandBuffer commandBuffer = beginSingleTimeCommands();

		VkBufferImageCopy region{};
		region.bufferOffset = 0;
		region.bufferRowLength = 0;
		region.bufferImageHeight = 0;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount = 1;
		region.imageOffset = { 0, 0, 0 };
		region.imageExtent = { width, height, 1 };

		vkCmdCopyBufferToImage(commandBuffer, buffer, image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		endSingleTimeCommands(commandBuffer);
	}

	void createInstanceDataBuffers() {

		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {

			instanceIndexUniforms_[i] = context->createBuffer(
				sizeof(IndexSlot) * MAX_INSTANCES,
				// Just using this ftm:
				VMA_MEMORY_USAGE_AUTO,
				VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
			);
			instanceDataUniforms_[i] = context->createBuffer(
				sizeof(InstanceData) * MAX_INSTANCES,
				VMA_MEMORY_USAGE_AUTO,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
			);
		}
	}

	void UploadDefaultDescriptorSets()  // no params; uses frameDescriptorSets[]
	{
		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {


			auto ii_data = context->buffers_.get(instanceIndexUniforms_[i]);
			auto id_data = context->buffers_.get(instanceDataUniforms_[i]);

			VkDescriptorBufferInfo instanceIndexBufferInfo{
				.buffer = ii_data->buffer,
				.offset = 0,
				.range = sizeof(IndexSlot) * MAX_INSTANCES
			};
			VkDescriptorBufferInfo instanceBufferInfo{
				.buffer = id_data->buffer,
				.offset = 0,
				.range = sizeof(InstanceData) * MAX_INSTANCES
			};

			auto setHandle = frameDescriptorSets[i];
			auto setObj = context->descriptrorSetPool_.get(setHandle);
			VkDescriptorSet vkSet = setObj->set;

			VkWriteDescriptorSet writes[2]{};

			writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[0].dstSet = vkSet;
			writes[0].dstBinding = 0; // UBO
			writes[0].dstArrayElement = 0;
			writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			writes[0].descriptorCount = 1;
			writes[0].pBufferInfo = &instanceIndexBufferInfo;

			writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[1].dstSet = vkSet;
			writes[1].dstBinding = 1; // SSBO
			writes[1].dstArrayElement = 0;
			writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[1].descriptorCount = 1;
			writes[1].pBufferInfo = &instanceBufferInfo;

			vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
		}
	}

	void createBuffer(
		VkDeviceSize size,
		VkBufferUsageFlags usage,
		VkMemoryPropertyFlags properties,
		VkBuffer& buffer,
		VkDeviceMemory& bufferMemory
	) {
		VkBufferCreateInfo bufferInfo{};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = size;
		bufferInfo.usage = usage;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
			throw std::runtime_error("failed to create buffer!");
		}

		VkMemoryRequirements memRequirements;
		vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

		VkMemoryAllocateInfo allocInfo{};

		// This should be moved to VMA
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = memRequirements.size;
		allocInfo.memoryTypeIndex =
			findMemoryType(
				physicalDevice,
				memRequirements.memoryTypeBits,
				properties
			);


		if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to allocate buffer memory!");
		}

		vkBindBufferMemory(device, buffer, bufferMemory, 0);
	}

	void createCommandBuffers() {

		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = framesCommandPool[currentFrame];
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = (uint32_t)MAX_FRAMES_IN_FLIGHT;

		if (vkAllocateCommandBuffers(device, &allocInfo, commandBuffers) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to allocate command buffers!");
		}
	}

	//void createCommandBuffers() {
	//	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
	//		VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	//		ai.commandPool = framesCommandPool[i];
	//		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	//		ai.commandBufferCount = 1;
	//		VK_CHECK(vkAllocateCommandBuffers(device, &ai, &commandBuffers[i]));
	//	}
	//}

	void UpdateBindlessTextures() {

		const uint32_t materialCount = static_cast<uint32_t>(context->materialInstancesPool_.numObjects());

		// --- Build image infos (sampled images) ---
		std::vector<VkDescriptorImageInfo> imageInfos;
		std::vector<VkDescriptorImageInfo> samplerInfos;

		int count = (context->textures_.numObjects());
		imageInfos.reserve(count);
		samplerInfos.reserve(count);

		for (int i = 0; i < count; i++)
		{
			auto tex = context->textures_.objects_[i].obj_;

			VkDescriptorImageInfo ii{};
			ii.sampler = VK_NULL_HANDLE; // separate samplers -> leave null
			ii.imageView = tex.imageView;
			ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imageInfos.push_back(ii);

			VkDescriptorImageInfo si{};
			si.sampler = tex.sampler;
			si.imageView = VK_NULL_HANDLE;            // not used for SAMPLER
			si.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED; // ignored for SAMPLER
			samplerInfos.push_back(si);
		};

		//// --- Two writes: images at index 2, samplers at index 3 ---
		VkWriteDescriptorSet writes[2]{};

		for (auto setHandle : frameDescriptorSets)
		{

			auto set = context->descriptrorSetPool_.get(setHandle);

			writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[0].dstSet = set->set;
			writes[0].dstBinding = 2;
			writes[0].dstArrayElement = 0;
			writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			writes[0].descriptorCount = imageInfos.size();
			writes[0].pImageInfo = imageInfos.data();

			writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[1].dstSet = set->set;
			writes[1].dstBinding = 3;
			writes[1].dstArrayElement = 0;
			writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
			writes[1].descriptorCount = samplerInfos.size();   // or 1 if you use a single shared sampler
			writes[1].pImageInfo = samplerInfos.data();

			vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

		}

		//const uint32_t imgCount = std::min<uint32_t>(imageInfos.size(), MAX_TEXTURES);
		//const uint32_t samCount = std::min<uint32_t>(samplerInfos.size(), MAX_TEXTURES);

		//if (imgCount > 0) {
		//	writes[0].descriptorCount = imgCount;
		//	// ...
		//	vkUpdateDescriptorSets(device, 1, &writes[0], 0, nullptr);
		//}
		//if (samCount > 0) {
		//	writes[1].descriptorCount = samCount;
		//	// ...
		//	vkUpdateDescriptorSets(device, 1, &writes[1], 0, nullptr);
		//}


	}

	void recordDraw(VkCommandBuffer commandBuffer, uint32_t imageIndex) {

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

		if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
			throw std::runtime_error("failed to begin recording command buffer!");
		}

		VkRenderingAttachmentInfoKHR depthAttachment{};
		depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
		depthAttachment.imageView = main_depthImageView;
		depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL_KHR;
		depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		//depthAttachment.clearValue.depthStencil = { 1.0f, 0 };
		//Lets do reverse z
		depthAttachment.clearValue.depthStencil = { 0.0f, 0 };

		VkRenderingAttachmentInfoKHR colorAttachment{};
		colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
		colorAttachment.imageView = swapChainImageViews[imageIndex];  // actual view
		colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.clearValue = { {0.0f, 0.0f, 0.0f, 1.0f} };

		// Begin dynamic rendering
		VkRenderingInfoKHR renderingInfo{};
		renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
		renderingInfo.renderArea.offset = { 0, 0 };
		renderingInfo.renderArea.extent = swapChainExtent;
		renderingInfo.layerCount = 1;
		renderingInfo.colorAttachmentCount = 1;
		renderingInfo.pColorAttachments = &colorAttachment;
		renderingInfo.pDepthAttachment = &depthAttachment;

		VkImageMemoryBarrier barrierToColorAttachment{};
		barrierToColorAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrierToColorAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // or VK_IMAGE_LAYOUT_PRESENT_SRC_KHR if reusing
		barrierToColorAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrierToColorAttachment.srcAccessMask = 0;  // No access before
		barrierToColorAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrierToColorAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrierToColorAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrierToColorAttachment.image = swapChainImages[imageIndex];
		barrierToColorAttachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrierToColorAttachment.subresourceRange.baseMipLevel = 0;
		barrierToColorAttachment.subresourceRange.levelCount = 1;
		barrierToColorAttachment.subresourceRange.baseArrayLayer = 0;
		barrierToColorAttachment.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(
			commandBuffer,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			0,
			0, nullptr,
			0, nullptr,
			1, &barrierToColorAttachment
		);

		vkCmdBeginRendering(commandBuffer, &renderingInfo);

		// Flipped because of this :)
		// https://www.saschawillems.de/blog/2019/03/29/flipping-the-vulkan-viewport/
		VkViewport vp{};
		vp.x = 0.0f;
		vp.y = 0.0f;
		vp.width = (float)swapChainExtent.width;
		vp.height = (float)swapChainExtent.height;
		vp.minDepth = 0.0f;
		vp.maxDepth = 1.0f;
		vkCmdSetViewport(commandBuffer, 0, 1, &vp);

		VkRect2D scissor{};
		scissor.offset = { 0, 0 };
		scissor.extent = swapChainExtent;
		vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

		VkBuffer vertexBuffers[] = { static_vertex_buffer_ };
		VkDeviceSize offsets[] = { 0 };

		vkCmdBindVertexBuffers(
			commandBuffer,
			0,
			1,
			vertexBuffers,
			offsets
		);
		vkCmdBindIndexBuffer(commandBuffer,
			static_index_buffer_,
			0,
			VK_INDEX_TYPE_UINT32)
			;

		// I need to batch per materialpipe

		for (auto mat_pipe_entry : context->materialPipelinePool_.objects_) {

			vkCmdBindPipeline(
				commandBuffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				mat_pipe_entry.obj_.pipeline
			);

			auto sets = context->descriptrorSetPool_.get(frameDescriptorSets[currentFrame]);
			auto pipe_lyt = mat_pipe_entry.obj_.plyt;

			vkCmdBindDescriptorSets(
				commandBuffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				pipe_lyt,
				0,
				1,
				&sets->set,
				0,
				nullptr);

			PushConstants pc{};
			pc.view = camera_.getViewMatrix();
			pc.proj = camera_.getProjMatrix();
			pc.proj[1][1] *= -1;

			for (int i = 0; i < scene_->hierarchy.size(); i++) {

				auto mesh_ = loaded_meshes_[scene_->meshForNode[i]];
				auto material_instance_index = scene_->materialForNode[i];
				pc.material_instance_index = material_instance_index;

				MaterialPushConstants pcm = context->materialInstancesPool_.objects_[material_instance_index].obj_.values;

				struct PC_All {
					PushConstants pc;               // 144
					MaterialPushConstants mpc;      // 48
				};
				PC_All all{ pc, pcm };

				// I could split this.
				vkCmdPushConstants(
					commandBuffer,
					pipe_lyt,
					VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
					0,
					sizeof(PC_All), &all);

				vkCmdDrawIndexed(
					commandBuffer,
					static_cast<uint32_t>(mesh_.indices.size()), //indexCount
					1, //instanceCount
					mesh_.first_index, //firstIndex
					mesh_.vert_offset, //vertexOffset
					i
				);
			}
		}

		vkCmdEndRendering(commandBuffer);

		// Not neeeded with dynamic renderinng because attachment info is simplified from renderpass system, simplifying attachment layouts away.
		//transitionImageLayout(
		//	swapChainImages[currentFrame], VK_FORMAT_B8G8R8A8_SRGB, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
		//);

		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = 0;  // Nothing needs to read it before present
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = swapChainImages[imageIndex];
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(
			commandBuffer,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, // after rendering finishes
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,          // before presentation
			0,
			0, nullptr,
			0, nullptr,
			1, &barrier
		);


		if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
			throw std::runtime_error("failed to record command buffer!");
		}
	}

	void createSyncObjects() {

		VkSemaphoreCreateInfo semaphoreInfo{};
		semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

		VkSemaphoreCreateInfo semInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

		for (uint32_t i = 0; i < swapchainImageCount; i++) {
			if (vkCreateSemaphore(device, &semInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS) {
				throw std::runtime_error("failed to create render-finished semaphore!");
			}
		}

		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
			if (
				vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS ||
				//vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS ||
				vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS) {
				throw std::runtime_error(
					"failed to create synchronization objects for a frame!");
			}
		}
	}

	// ---------------------------------------------------------------------------

	void drawFrame() {
		vkWaitForFences(device, 1, &inFlightFences[currentFrame], VK_TRUE,
			UINT64_MAX);

		uint32_t imageIndex;
		VkResult result = vkAcquireNextImageKHR(
			device, swapChain, UINT64_MAX, imageAvailableSemaphores[currentFrame],
			VK_NULL_HANDLE, &imageIndex);


		if (result == VK_ERROR_OUT_OF_DATE_KHR) {
			recreateSwapChain();
			return;
		}
		else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
			throw std::runtime_error("failed to acquire swap chain image!");
		}

		// BATCH PER MESH, SHADER, INSTANCE

		// We should check for size and promote or demote from pushconstants -> ubo -> ssbo.
		UpdateDeltaTime();
		UpdateInstanceBuffer(currentFrame);

		vkResetFences(device, 1, &inFlightFences[currentFrame]);
		vkResetCommandPool(device, framesCommandPool[currentFrame], 0);

		//vkResetCommandBuffer(commandBuffers[currentFrame],
		//	/*VkCommandBufferResetFlagBits*/ 0);

		UploadDefaultDescriptorSets();
		UpdateBindlessTextures();

		recordDraw(commandBuffers[currentFrame], imageIndex);

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

		VkSemaphore waitSemaphores[] = { imageAvailableSemaphores[currentFrame] };
		VkPipelineStageFlags waitStages[] = {
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;

		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffers[currentFrame];

		VkSemaphore signalSemaphores[] = { renderFinishedSemaphores[currentFrame] };
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = signalSemaphores;

		if (
			vkQueueSubmit(
				graphicsQueue_,
				1,
				&submitInfo,
				inFlightFences[currentFrame]) != VK_SUCCESS
			) {
			throw std::runtime_error("failed to submit draw command buffer!");
		}


		VkPresentInfoKHR presentInfo{};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = signalSemaphores;

		VkSwapchainKHR swapChains[] = { swapChain };
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = swapChains;
		presentInfo.pImageIndices = &imageIndex;

		result = vkQueuePresentKHR(presentQueue_, &presentInfo);

		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
			framebufferResized) {
			framebufferResized = false;
			recreateSwapChain();
		}
		else if (result != VK_SUCCESS) {
			throw std::runtime_error("failed to present swap chain image!");
		}

		currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
	}

	// UI
	static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
	static void scrollCallback(GLFWwindow* window, double xOffset, double yOffset);
	static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
	static void mouseCallback(GLFWwindow* window, double xpos, double ypos);
	static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);

	void handleMouseScroll(double xOffset, double yOffset);
	void handleMouseMovement(double xpos, double ypos);
	void handleMouseButton(int button, int action, int mods);
	void handleKey(int key, int scancode, int action, int mods);

	// utils
	VkCommandBuffer beginSingleTimeCommands();
	void endSingleTimeCommands(VkCommandBuffer commandBuffer);
	void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);

	// TEMP
	std::vector<MaterialInstanceHandle> material_instance_handles;
	std::vector<MaterialPipelineHandle> material_pipe_handles;

};

void VulkanApp::CreateDefaultMaterialPipeline() {

	auto vertShaderCode = readFile("shaders/example.vert.hlsl.spv");
	auto vertColorFragCode = readFile("shaders/vertex_color.frag.hlsl.spv");
	auto fragShaderCode = readFile("shaders/example.frag.hlsl.spv");

	//VkShaderModule vertShaderModule = createShaderModule(device, vertShaderCode);
	//VkShaderModule fragShaderModule = createShaderModule(device, fragShaderCode);
	//VkShaderModule vertexColorFrag = createShaderModule(device, vertColorFragCode);

	auto vertHandle = context->createShaderModule(
		vertShaderCode
	);
	auto fragHandle = context->createShaderModule(
		fragShaderCode
	);

	auto def_pipe = context->createMaterialPipeline(
		vertHandle,
		fragHandle,
		default_raster_settings,
		findDepthFormat(physicalDevice), // THIS IS REALLY BAD. I WANT FIXED DEPTH FORMAT OR EARLY OUT ON DEVICE CRTEATION. lifted form vulkan tutorial
		swapChainImageFormat
	);

	material_pipe_handles.push_back(def_pipe);
}

void VulkanApp::LoadScene() {

	//rootNode
	addNode(*scene_, -1, 0);
	addNode(*scene_, 0, 1);
	addNode(*scene_, 0, 1);

	//std::cout << std::filesystem::current_path() << std::endl;

	LoadedMesh mesh_;
	auto a_ = GltfMesh("models/avocado_gltf/Avocado.gltf");
	mesh_.indices = a_.indices;
	mesh_.vertices = a_.vertices;

	LoadedMesh mesh_2;
	auto b_ = GltfMesh("models/BoomBox/BoomBox.gltf");
	mesh_2.indices = b_.indices;
	mesh_2.vertices = b_.vertices;

	// this is crap. just for testing 
	loaded_meshes_.push_back(mesh_);
	loaded_meshes_.push_back(mesh_2);

	auto m_m = scene_->localTransform[0];

	scene_->meshForNode[0] = 1;
	scene_->materialForNode[0] = 0;

	scene_->meshForNode[1] = 3;
	scene_->materialForNode[1] = 1;

	scene_->meshForNode[2] = 2;
	scene_->materialForNode[2] = 2;

	scene_->localTransform[2][0][0] = 100;
	scene_->localTransform[2][1][1] = 100;
	scene_->localTransform[2][2][2] = 100;

	scene_->localTransform[1][0][0] = 100;
	scene_->localTransform[1][1][1] = 100;
	scene_->localTransform[1][2][2] = 100;

	auto image1 = createTextureImage(
		"textures/viking_room.png",
		VK_FORMAT_R8G8B8A8_SRGB
	);

	auto image2 = createTextureImage(
		"textures/texture.jpg",
		VK_FORMAT_R8G8B8A8_SRGB
	);
	auto image3 = createTextureImage(
		"models/avocado_gltf/Avocado_baseColor.png",
		VK_FORMAT_R8G8B8A8_SRGB
	);


	createTextureImageView(
		&context->textures_.get(image1)->image,
		&context->textures_.get(image1)->imageView,
		VK_FORMAT_R8G8B8A8_SRGB
	);


	createTextureImageView(
		&context->textures_.get(image2)->image,
		&context->textures_.get(image2)->imageView,
		VK_FORMAT_R8G8B8A8_SRGB
	);

	createTextureImageView(
		&context->textures_.get(image3)->image,
		&context->textures_.get(image3)->imageView,
		VK_FORMAT_R8G8B8A8_SRGB
	);


	createTextureSampler(
		&context->textures_.get(image1)->sampler
	);

	createTextureSampler(
		&context->textures_.get(image2)->sampler
	);

	createTextureSampler(
		&context->textures_.get(image3)->sampler
	);


	auto material_pipe_0 = context->materialPipelinePool_.getHandle(0);

	context->createMaterialInstance(material_pipe_0);
	auto mat2_h = context->createMaterialInstance(material_pipe_0);
	context->createMaterialInstance(material_pipe_0);


	context->materialInstancesPool_.get(mat2_h)->values.texDiffuse = 1;

	//context->createMaterialInstance(material_pipe_0);
	//context->createMaterialInstance(material_pipe_0);


}

void VulkanApp::UploadMeshData() {
	// Upload Mesh data
	// query meshes for upload.

	uint32_t static_vertex_buffer_size = 0;
	uint32_t static_index_buffer_size = 0;

	for (int node = 0; node < static_cast<int>(scene_->hierarchy.size()); ++node) {
		int mIdx = scene_->meshForNode[node];
		const auto& m = loaded_meshes_[mIdx];
		static_vertex_buffer_size += static_cast<uint32_t>(sizeof(Vertex) * m.vertices.size());
		static_index_buffer_size += static_cast<uint32_t>(sizeof(uint32_t) * m.indices.size());
	}

	// Compute totals and write per-mesh offsets (skip slot 0 = empty)
	VkDeviceSize totalVertexBytes = 0;
	VkDeviceSize totalIndexBytes = 0;

	uint32_t runningVertexOffset = 0; // in vertices
	uint32_t runningFirstIndex = 0; // in indices

	for (int i = 0; i < loaded_meshes_.size(); ++i) {
		LoadedMesh& m = loaded_meshes_[i];

		// this is not correct
		m.vert_offset = runningVertexOffset;         // elements, not bytes
		m.first_index = runningFirstIndex;           // elements, not bytes

		totalVertexBytes += sizeof(Vertex) * m.vertices.size();
		totalIndexBytes += sizeof(uint32_t) * m.indices.size();

		runningVertexOffset += static_cast<uint32_t>(m.vertices.size());
		runningFirstIndex += static_cast<uint32_t>(m.indices.size());
	}

	// Create GPU static buffers (DST)
	{
		assert(totalVertexBytes > 0 && "Vertex buffer size was 0");
		VkBufferCreateInfo vbInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = totalVertexBytes,
			.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
		};
		VmaAllocationCreateInfo vbAlloc{ .usage = VMA_MEMORY_USAGE_GPU_ONLY };

		vmaCreateBuffer(
			app_allocator_,
			&vbInfo,
			&vbAlloc,
			&static_vertex_buffer_,
			&static_vertex_allocation_,
			nullptr
		);
	}
	{
		assert(totalIndexBytes > 0 && "Index buffer size was 0");
		VkBufferCreateInfo ibInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = totalIndexBytes,
			.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
		};
		VmaAllocationCreateInfo ibAlloc{ .usage = VMA_MEMORY_USAGE_GPU_ONLY };
		vmaCreateBuffer(app_allocator_, &ibInfo, &ibAlloc,
			&static_index_buffer_, &static_index_allocation_, nullptr);
	}

	// Staging + upload: VERTICES
	{
		VkBuffer stagingBuf; VmaAllocation stagingAlloc;
		VkBufferCreateInfo sInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = totalVertexBytes,
			.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		};
		VmaAllocationCreateInfo sAlloc{};
		sAlloc.usage = VMA_MEMORY_USAGE_AUTO;
		sAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

		vmaCreateBuffer(app_allocator_, &sInfo, &sAlloc, &stagingBuf, &stagingAlloc, nullptr);

		VkDeviceSize curr = 0; // byte offset
		for (int i = 0; i < loaded_meshes_.size(); ++i) {
			const auto& m = loaded_meshes_[i];
			const VkDeviceSize bytes = sizeof(Vertex) * m.vertices.size();
			if (bytes == 0) continue;

			vmaCopyMemoryToAllocation(app_allocator_, m.vertices.data(), stagingAlloc, curr, bytes);
			curr += bytes;
		}

		copyBuffer(stagingBuf, static_vertex_buffer_, totalVertexBytes);
		vmaDestroyBuffer(app_allocator_, stagingBuf, stagingAlloc);
	}

	// Staging + upload: INDICES
	{
		VkBuffer stagingBuf; VmaAllocation stagingAlloc;
		VkBufferCreateInfo sInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = totalIndexBytes,
			.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		};
		VmaAllocationCreateInfo sAlloc{};
		sAlloc.usage = VMA_MEMORY_USAGE_AUTO;
		sAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

		vmaCreateBuffer(app_allocator_, &sInfo, &sAlloc, &stagingBuf, &stagingAlloc, nullptr);

		VkDeviceSize curr = 0; // byte offset
		for (int i = 0; i < loaded_meshes_.size(); ++i) {
			const auto& m = loaded_meshes_[i];
			const VkDeviceSize bytes = sizeof(uint32_t) * m.indices.size();
			if (bytes == 0) continue;

			// Indices remain local (0-based). We won't rewrite them here.
			vmaCopyMemoryToAllocation(app_allocator_, m.indices.data(), stagingAlloc, curr, bytes);
			curr += bytes;
		}

		copyBuffer(stagingBuf, static_index_buffer_, totalIndexBytes);
		vmaDestroyBuffer(app_allocator_, stagingBuf, stagingAlloc);
	}

}

bool VulkanApp::Run() {

	if (volkInitialize() != VK_SUCCESS) {
		std::cerr << "Failed to initialize Volk\n";
		glfwTerminate();
		return false;
	};

	// GLFW;

	if (!glfwInit()) {
		std::cerr << "Failed to initialize GLFW\n";
		return false;
	}

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

	window = glfwCreateWindow(width_, height_, "Vulkan", nullptr, nullptr);

	if (!window) {
		std::cerr << "Failed to create GLFW window\n";
		glfwTerminate();
		return false;
	}
	glfwMakeContextCurrent(window);
	glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
	glfwSetWindowTitle(window, ENGINE_NAME);
	glfwSetKeyCallback(window, VulkanApp::keyCallback);
	glfwSetCursorPosCallback(window, VulkanApp::mouseCallback);
	glfwSetMouseButtonCallback(window, VulkanApp::mouseButtonCallback);
	glfwSetScrollCallback(
		window, VulkanApp::scrollCallback);  // Register scroll callback

	glfwSetWindowUserPointer(window, this);
	glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);

	// Pick physical device, create logical device, get queues from physdevice. Create VMA allocator
	InitVulkanApp();

	createSwapChain();
	renderFinishedSemaphores = new VkSemaphore[swapchainImageCount];
	createSwapchainImageViews();

	QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice, surface);

	VkCommandPoolCreateInfo poolInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value(),
	};

	for (int fn = 0; fn < MAX_FRAMES_IN_FLIGHT; fn++) {
		if (vkCreateCommandPool(device, &poolInfo, nullptr, &framesCommandPool[fn]) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create graphics command pool!");
		}
	}

	// createDescriptorSets. Create menas allocating. no data is present there still
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		frameDescriptorSets[i] = context->createDescriptorSet(
			context->default_descriptorLayout
		);
	}

	camera_.setPerspective(45.0f, float(width_) / float(height_), default_near_clip, default_far_clip);

	createInstanceDataBuffers();
	createCommandBuffers();
	createSyncObjects();
	createDepthResources();

	CreateDefaultMaterialPipeline();
	LoadScene();
	UploadMeshData();
	UpdateBindlessTextures();

	mainLoop();

	//cleanup();
	vmaDestroyAllocator(app_allocator_);

	return true;

};

void VulkanApp::InitVulkanApp() {

	{
		if (enableValidationLayers && !checkValidationLayerSupport()) {
			throw std::runtime_error(
				"validation layers requested, but not available!");
		}

		VkApplicationInfo appInfo{};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.pApplicationName = "VulkanToy";
		appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.pEngineName = "Sam2025Vulkan_one_three";
		appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.apiVersion = VK_API_VERSION_1_3;

		VkInstanceCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		createInfo.pApplicationInfo = &appInfo;

		auto extensions = getRequiredExtensions();
		createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
		createInfo.ppEnabledExtensionNames = extensions.data();

		if (enableValidationLayers) {
			createInfo.enabledLayerCount =
				static_cast<uint32_t>(validationLayers.size());
			createInfo.ppEnabledLayerNames = validationLayers.data();

			VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo;
			populateDebugMessengerCreateInfo(debugCreateInfo);
			createInfo.pNext = (void*)&debugCreateInfo;

		}
		else {
			createInfo.enabledLayerCount = 0;
			createInfo.pNext = nullptr;
		}

		if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
			throw std::runtime_error("failed to create instance!");
		}

		volkLoadInstance(instance);

	}

	if (enableValidationLayers) {

		VkDebugUtilsMessengerCreateInfoEXT createInfo;
		populateDebugMessengerCreateInfo(createInfo);

		if (CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr,
			&debugMessenger) != VK_SUCCESS) {
			throw std::runtime_error("failed to set up debug messenger!");
		}
	}


	// Create Surface
	if (glfwCreateWindowSurface(instance, window, nullptr, &surface) !=
		VK_SUCCESS) {
		throw std::runtime_error("failed to create window surface!");
	}

	// Pick Device
	uint32_t deviceCount = 0;
	vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

	if (deviceCount == 0) {
		throw std::runtime_error("failed to find GPUs with Vulkan support!");
	}

	std::vector<VkPhysicalDevice> devices(deviceCount);
	vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

	for (const auto& device : devices) {
		if (isDeviceSuitable(device, surface)) {
			physicalDevice = device;
			break;
		}
	}

	if (physicalDevice == VK_NULL_HANDLE) {
		throw std::runtime_error("failed to find a suitable GPU!");
	}

	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(physicalDevice, &properties);

#ifndef DEBUG

	std::cout << "----DEVICE INFO-----" << std::endl;

	std::cout << "	maxUniformBufferRange = "
		<< properties.limits.maxUniformBufferRange << " bytes\n";

	std::cout << "	minUniformBufferOffsetAlignment = "
		<< properties.limits.minUniformBufferOffsetAlignment << " bytes\n";

	std::cout << "	maxStorageBufferRange = "
		<< properties.limits.maxStorageBufferRange << " bytes\n\n";
#endif

	// Create Logical Device
	{
		QueueFamilyIndices indices = findQueueFamilies(physicalDevice, surface);

		std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
		std::set<uint32_t> uniqueQueueFamilies = { indices.graphicsFamily.value(),
												  indices.presentFamily.value() };

		float queuePriority = 1.0f;
		for (uint32_t queueFamily : uniqueQueueFamilies) {
			VkDeviceQueueCreateInfo queueCreateInfo{};
			queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queueCreateInfo.queueFamilyIndex = queueFamily;
			queueCreateInfo.queueCount = 1;
			queueCreateInfo.pQueuePriorities = &queuePriority;
			queueCreateInfos.push_back(queueCreateInfo);
		}

		VkPhysicalDeviceFeatures deviceFeatures{
			.samplerAnisotropy = VK_TRUE
		};
		deviceFeatures.samplerAnisotropy = VK_TRUE;

		VkDeviceCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

		createInfo.queueCreateInfoCount =
			static_cast<uint32_t>(queueCreateInfos.size());
		createInfo.pQueueCreateInfos = queueCreateInfos.data();

		createInfo.pEnabledFeatures = &deviceFeatures;

		createInfo.enabledExtensionCount =
			static_cast<uint32_t>(deviceExtensions.size());
		createInfo.ppEnabledExtensionNames = deviceExtensions.data();

		if (enableValidationLayers) {
			createInfo.enabledLayerCount =
				static_cast<uint32_t>(validationLayers.size());
			createInfo.ppEnabledLayerNames = validationLayers.data();
		}
		else {
			createInfo.enabledLayerCount = 0;
		}


		// Descriptor indexing (core, not EXT)
		VkPhysicalDeviceDescriptorIndexingFeatures descIdx{};
		descIdx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
		// Enable what you actually need:
		descIdx.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
		descIdx.descriptorBindingPartiallyBound = VK_TRUE;
		descIdx.runtimeDescriptorArray = VK_TRUE;
		descIdx.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;

		// Maybe I dont want this one
		descIdx.descriptorBindingVariableDescriptorCount = VK_TRUE;

		VkPhysicalDeviceVulkan13Features vulkan13Features{};
		vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		vulkan13Features.dynamicRendering = VK_TRUE;
		vulkan13Features.pNext = &descIdx;
		createInfo.pNext = &vulkan13Features;


		if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) !=
			VK_SUCCESS) {
			throw std::runtime_error("failed to create logical device!");
		}

		volkLoadDevice(device);

		vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue_);
		vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue_);
	}

	VmaAllocatorCreateInfo allocatorCreateInfo = {};
	allocatorCreateInfo.physicalDevice = physicalDevice;
	allocatorCreateInfo.device = device;
	allocatorCreateInfo.instance = instance;
	allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_3;
	allocatorCreateInfo.flags =
		VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT |
		VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;

	// This next flag is to get handles. To interporate resources between instances, or even between Vulkan and Dx or things like that.
	// VMA_ALLOCATOR_CREATE_KHR_EXTERNAL_MEMORY_WIN32_BIT

	VmaVulkanFunctions vulkanFunctions;
	VkResult res = vmaImportVulkanFunctionsFromVolk(&allocatorCreateInfo, &vulkanFunctions);
	allocatorCreateInfo.pVulkanFunctions = &vulkanFunctions;

	res = vmaCreateAllocator(&allocatorCreateInfo, &app_allocator_);
	assert(res == VK_SUCCESS);
	context = new Context(&app_allocator_, &device); // remember to delete it on shutdown
}
void VulkanApp::mouseCallback(GLFWwindow* window, double xpos, double ypos) {
	auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
	if (app) app->handleMouseMovement(xpos, ypos);
}

void VulkanApp::keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
	auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
	if (app) app->handleKey(key, scancode, action, mods);
}

void VulkanApp::mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
	auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
	if (app) app->handleMouseButton(button, action, mods);
}

void VulkanApp::scrollCallback(GLFWwindow* window, double xOffset, double yOffset) {
	auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
	if (app) app->handleMouseScroll(xOffset, yOffset);
}

void VulkanApp::handleMouseScroll(double xOffset, double yOffset) {
	const float scrollSpeed = 1.0f;
	glm::vec3 forward = cameraPositioner.getForward();
	cameraPositioner.setPosition(
		cameraPositioner.getPosition() + static_cast<float>(yOffset) * scrollSpeed * forward
	);
}

void VulkanApp::handleMouseMovement(double xpos, double ypos) {
	if (!rightMousePressed) return;

	glm::vec2 mousePos = glm::vec2(
		static_cast<float>(xpos) / width_,
		static_cast<float>(ypos) / -height_
	);
	cameraPositioner.update(delta_time_ / 1000,
		mousePos,
		true
	);
}

void VulkanApp::handleMouseButton(int button, int action, int mods) {
	if (button == GLFW_MOUSE_BUTTON_RIGHT) {
		rightMousePressed = (action == GLFW_PRESS);
	}
}

void VulkanApp::handleKey(int key, int scancode, int action, int mods) {
	bool isPressed = action != GLFW_RELEASE;

	switch (key) {
	case GLFW_KEY_R:
	case GLFW_KEY_F:
		cameraPositioner.reset();
		break;
	case GLFW_KEY_W:
	case GLFW_KEY_UP:
		cameraPositioner.movement_.forward_ = isPressed;
		break;
	case GLFW_KEY_S:
	case GLFW_KEY_DOWN:
		cameraPositioner.movement_.backward_ = isPressed;
		break;
	case GLFW_KEY_A:
	case GLFW_KEY_LEFT:
		cameraPositioner.movement_.left_ = isPressed;
		break;
	case GLFW_KEY_D:
	case GLFW_KEY_RIGHT:
		cameraPositioner.movement_.right_ = isPressed;
		break;
	case GLFW_KEY_Q:
	case GLFW_KEY_SPACE:
		cameraPositioner.movement_.up_ = isPressed;
		break;
	case GLFW_KEY_E:
		cameraPositioner.movement_.down_ = isPressed;
		break;
	case GLFW_KEY_LEFT_SHIFT:
		cameraPositioner.movement_.fastSpeed_ = isPressed;
		break;
	default:
		break;
	}
}

void VulkanApp::framebufferResizeCallback(GLFWwindow* window, int width,
	int height) {
	auto app =
		reinterpret_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
	app->framebufferResized = true;
}

VkCommandBuffer VulkanApp::beginSingleTimeCommands() {
	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandPool = framesCommandPool[currentFrame];
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer commandBuffer;
	vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	vkBeginCommandBuffer(commandBuffer, &beginInfo);

	return commandBuffer;
}

void VulkanApp::endSingleTimeCommands(VkCommandBuffer commandBuffer) {
	vkEndCommandBuffer(commandBuffer);

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;

	vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE);
	vkQueueWaitIdle(graphicsQueue_);

	vkFreeCommandBuffers(device, framesCommandPool[currentFrame], 1, &commandBuffer);
}

void VulkanApp::copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
	VkCommandBuffer commandBuffer = beginSingleTimeCommands();

	VkBufferCopy copyRegion{};
	copyRegion.size = size;
	vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

	endSingleTimeCommands(commandBuffer);
}

int main(int argc, const char* argv[]) {
	if (argc > 1) {
		std::cout << argv[0];
	}

	VulkanApp app = VulkanApp();
	try {
		app.Run();
	}
	catch (const std::exception& e) {
		std::cerr << e.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
