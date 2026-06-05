#pragma once
#include <Engine/App/app.h>

#include <Engine/Renderer/ringBinding.h>

std::vector<std::string> gltf_meshes_paths = {
	"content/models/Sponza/sponza.gltf"
};


// SETIINGS
enum CULLING_MODE : uint32_t {
	NO_CULLING = 0,
	CPU_CULLING = 1,
	GPU_CULLING = 1,
};

// 

struct DrawSetData {
	float transform[16] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};
	glm::vec4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
};

static_assert(sizeof(DrawSetData) % 16 == 0);

struct FrameUniforms
{
	glm::mat4 view;
	glm::mat4 proj;
	glm::mat4 viewProj;

	glm::mat4 invView;
	glm::mat4 invProj;
	glm::mat4 invViewProj;

	glm::vec3 cameraPos;
	float     time;

	glm::vec2 viewportSize;
	glm::vec2 invViewportSize;

	float nearPlane;
	float farPlane;
	float exposure;
	float padding; // keep 16-byte alignment
};
static_assert(sizeof(FrameUniforms) % 16 == 0);


struct MainViewPushConstants {
	glm::mat4 model;
	int material_index;
	uint32_t     _pad0[3];
};
static_assert(sizeof(MainViewPushConstants) % 16 == 0);



struct Level : Scene {

	MeshManager* man = nullptr;
	BvhTree tree = {};
	BvhTree static_tree = {};


	// This should be peged to recalulateGlobalTransforms and dirty update. 
	// Crude patch to get it running now. ;)
	bool tree_done = false;

	~Level() {}


	void Upload(
		std::filesystem::path gltf_model_path
	) {

		tinygltf::Model model;
		LoadModelFromAsciiFile(model, gltf_model_path.string().c_str());

		GltfToSceneImporter importer(*man, *this, model, std::move(gltf_model_path));
		importer.importNodeRecursive(/*rootNodeIdx*/0, /*parentSceneNode=*/-1, /*level=*/0);

		// This is needed or else scene will not carry transforms effectively
		markAsChanged(*this, 0);
		recalculateGlobalTransforms(*this);

	}

};



class TestApp : public VulkanApp {

public:
	~TestApp() {};

private:
	using Super = VulkanApp;
	// this is bad. redesign!
	MeshManager man = {};
	Level current_level = {};
	
	// 
	void UpdateCamera(float dt);

	// App resources -- This should just be spawned by the RG
	struct FrameResources {
		DescriptorSetHandle frameDescriptorSet;
		DescriptorSetHandle frameDynamicDescriptorSet;
	};
	FrameResources perFrameData[MAX_FRAMES_IN_FLIGHT];
	// 

	RingBinding<FrameUniforms, 1> frameUniformsBinding;
	RingBinding<DrawSetData, MAX_DRAWS> drawDataBinding;
	PipelineHandle mainPipeline;

	// App state
	CameraPositioner_FirstPerson cameraPositioner = CameraPositioner_FirstPerson(
		glm::vec3(0.0f, 1.0f, 0.0f), //pos
		glm::vec3(0.0f, 1.0f, 1.0f), //target
		glm::vec3(0.0f, 1.0f, 0.0f) //up
	);
	Camera camera = Camera(cameraPositioner);

	void EarlyUpdate() override {
		Super::EarlyUpdate();
	}

	void Update() {
		Super::Update();
		UpdateCamera(delta_time_s);
	}

	void LateUpdate() {
		Super::LateUpdate();
	}

	bool Initialize() {
		if (!Super::Initialize())
			return false;

		man = MeshManager(
			context
		);
		man.InitalizeGeoBuffers();

		// Init
		frameUniformsBinding.init(
			context,
			0, //bindding #
			MAX_FRAMES_IN_FLIGHT,
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
		);

		drawDataBinding.init(
			context,
			1, //bindding #
			MAX_FRAMES_IN_FLIGHT,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, //bufferUsageFlags
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC //descriptorType
		);

		auto vertShaderCode = readFile("content/shaders/default.vert.hlsl.spv");
		auto fragShaderCode = readFile("content/shaders/default.frag.hlsl.spv");

		auto vert_shader_h = context->CreateShaderModule(
			vertShaderCode
		);
		auto fragHandle = context->CreateShaderModule(
			fragShaderCode
		);

		PipelineLayoutDesc desc;
		desc.sets = {
			DescriptorSetLayoutDesc{
				0, // setLayout #
				0, //VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, // createFlags
				{
					frameUniformsBinding.bindingDesc(), // binding 0,0
					drawDataBinding.bindingDesc(), //binding 0,1
				}
			},
			// These two need to be separated due to UPDATE_AFTER_BIND and dynamicOffsets incompatibility in same setLayout
			DescriptorSetLayoutDesc{
				.set = 1,
				.createFlags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
				.bindings = {
				BindingDesc{
					.binding = 2,
					.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
					.count = MAX_TEXTURES,
					.stages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT,
					.flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
							  VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
					.immutableSamplers = {}
				},
			// Example bindless samplers
			BindingDesc{
				.binding = 3,
				.type = VK_DESCRIPTOR_TYPE_SAMPLER,
				.count = MAX_TEXTURES, // We carry 1 to one samplers, this is wasteful but lets do this ftm
				.stages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT,
				.flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
						  VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
				.immutableSamplers = {}
			}
		}
	}
		};
		desc.pushConstants.push_back(
			{
			.stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			.offset = 0,
			.size = uint32_t(sizeof(MainViewPushConstants) +
							   sizeof(Material::Values)
			),
			}
		);

		// Just define renderPasses and grah and go.s
		// App shouldnt need to do this really...
		mainPipeline = context->CreatePipeline(
			{
				.depthClampEnable = VK_FALSE, // needs to be enabled
				.rasterizerDiscardEnable = VK_FALSE,
				.polygonMode = VK_POLYGON_MODE_FILL,
				.cullMode = VK_CULL_MODE_BACK_BIT,
				.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE
			}, // rasterDesc
			desc, // layoutDesc
			vert_shader_h, // vert
			fragHandle, //frag
			findDepthFormat(physicalDevice),
			swapchain->GetImageFormat()
		);
		auto pipe_obj = context->pipelines_.get(mainPipeline);
		auto bb = pipe_obj->layout.sets;

		for (int i = 0; i < swapchain->framesInFlight_; i++) {

			perFrameData[i].frameDynamicDescriptorSet = context->CreateDescriptorSet(
				bb[0]
			);

			perFrameData[i].frameDescriptorSet = context->CreateDescriptorSet(
				bb[1]
			);
		}

		// Dedfault camera settings
		camera.setPerspective(45.0f, float(swapchain->extent_.width) / float(swapchain->extent_.height), default_near_clip, default_far_clip);

		std::filesystem::path p(gltf_meshes_paths[0]);
		current_level.man = &man;
		current_level.Upload(
			p
		);
		current_level.static_tree = treeFromScene(current_level,
			[&](uint32_t index) -> const Mesh& {
				// Highly illegal. but lets roll with this,
				return this->man.meshes.objects_[index].obj_;
			}
		);
		current_level.tree_done = true;

		SetDescriptorSets();
		UpdateBindlessTexturesSets();

		return true;
	};

	void SetDescriptorSets()
	{
		// Dynamic
		for (size_t frameIndex = 0; frameIndex < MAX_FRAMES_IN_FLIGHT; frameIndex++) {

			auto setHandle = perFrameData[frameIndex].frameDynamicDescriptorSet;
			auto setObj = context->descriptorSets_.get(setHandle);
			VkDescriptorSet vkSet = setObj->set;

			// FrameUniforms 
			auto frameUniformsBufferHandle = context->buffers_.get(frameUniformsBinding.buffer_h);
			VkDescriptorBufferInfo frameUniformsInfo{
				.buffer = frameUniformsBufferHandle->buffer,
				.offset = 0,
				.range = sizeof(FrameUniforms) // this is the size of the window accesed per bind
			};

			// Draw data
			auto drawsDataBufferHandle = context->buffers_.get(drawDataBinding.buffer_h);
			VkDescriptorBufferInfo drawBufferInfo{
				.buffer = drawsDataBufferHandle->buffer,
				.offset = 0,
				.range = sizeof(DrawSetData) * MAX_DRAWS
			};
			VkWriteDescriptorSet writes[2]{};

			writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[0].dstSet = vkSet;
			writes[0].dstBinding = 0; // UBO
			writes[0].dstArrayElement = 0;
			writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
			writes[0].descriptorCount = 1;
			writes[0].pBufferInfo = &frameUniformsInfo;

			writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[1].dstSet = vkSet;
			writes[1].dstBinding = 1; // SSBO
			writes[1].dstArrayElement = 0;
			writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
			writes[1].descriptorCount = 1;
			writes[1].pBufferInfo = &drawBufferInfo;

			vkUpdateDescriptorSets(device, 2 /*descriptorWriteCount*/, writes, 0, nullptr);

		}
	}

	void UpdateBindlessTexturesSets() {

		const uint32_t materialCount = static_cast<uint32_t>(man.materials.numObjects());

		std::vector<VkDescriptorImageInfo> imageInfos;
		std::vector<VkDescriptorImageInfo> samplerInfos;

		int count = (context->textures_.numObjects());
		imageInfos.reserve(count);
		samplerInfos.reserve(count);

		//auto smp = context->samplers_.get(context->samplers.at("default"));
		////image_obj.sampler = *smp;

		//VkDescriptorImageInfo si{};
		//si.sampler = *smp;
		//si.imageView = VK_NULL_HANDLE;            // not used for SAMPLER
		//si.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED; // ignored for SAMPLER
		//samplerInfos.push_back(si);

		// this has problems.
		// theres compressiona around index meaning. 
		// textureIndex/imageIndex
		context->textures_.forEach([&](Image obj_)
			{
				VkDescriptorImageInfo ii{};
				ii.sampler = VK_NULL_HANDLE; // separate samplers -> leave null
				ii.imageView = obj_.sampleView;
				ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				imageInfos.push_back(ii);

				// Sampler for the bindless texure approach needs refacotr.
				// Doesnt Really make sense.
				VkDescriptorImageInfo si{};
				si.sampler = obj_.sampler;
				si.imageView = VK_NULL_HANDLE;            // not used for SAMPLER
				si.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED; // ignored for SAMPLER
				samplerInfos.push_back(si);
			}
		);

		for (size_t frameIndex = 0; frameIndex < MAX_FRAMES_IN_FLIGHT; frameIndex++) {
			//// --- Two writes: images at index 2, samplers at index 3 ---
			VkWriteDescriptorSet writes[2]{};

			auto setHandle = perFrameData[frameIndex].frameDescriptorSet;
			auto set = context->descriptorSets_.get(setHandle);

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
			writes[1].descriptorCount = static_cast<uint32_t>(samplerInfos.size());   // or 1 if you use a single shared sampler
			writes[1].pImageInfo = samplerInfos.data();

			vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
		}

	}


	void UpdateFrameUniforms(uint32_t currentFrame) {

		int width_ = swapchain->GetExtent().width;
		int height_ = swapchain->GetExtent().height;

		auto view = camera.getViewMatrix();
		auto proj = camera.getProjMatrix();
		proj[1][1] *= -1;

		auto viewProj = proj * view;

		FrameUniforms data{};
		data.cameraPos = cameraPositioner.cameraPosition_;

		data.view = view;
		data.proj = proj;
		data.viewProj = viewProj;

		data.invView = glm::inverse(view);
		data.invProj = glm::inverse(proj);
		data.invViewProj = glm::inverse(viewProj);

		data.time = 0.0f;
		data.viewportSize = { width_, height_ };
		data.invViewportSize = { 1.0f / width_, 1.0f / height_ };

		// Not implemented for now
		data.nearPlane = 0.f;
		data.farPlane = 0.f;
		data.exposure = 0.f;

		// HOST MAP UPLOAD OPTIMIST PATH.
		context->UploadDataToBuffer(
			frameUniformsBinding.buffer_h,
			&data,
			sizeof(FrameUniforms)/* * 1*/,
			VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
			VK_ACCESS_SHADER_READ_BIT,
			0,
			frameUniformsBinding.dynamicOffsetFrame(currentFrame)
		);

	}

	void DrawFrame() {
		// Pure virtual
		//Super::DrawFrame();

		auto cmd = AcquireMain();
		auto swapChainExtent = swapchain->GetExtent();

		UpdateFrameUniforms(swapchain->GetCurrentFrameIndex());

		// VIEWPORT
		// Flipped because of this :)
		// https://www.saschawillems.de/blog/2019/03/29/flipping-the-vulkan-viewport/
		VkViewport vp{};
		vp.x = 0.0f;
		vp.y = 0.0f;
		vp.width = (float)swapChainExtent.width;
		vp.height = (float)swapChainExtent.height;
		vp.minDepth = 0.0f;
		vp.maxDepth = 1.0f;
		vkCmdSetViewport(cmd.commandBuffer, 0, 1, &vp);

		// SCISSOR.
		VkRect2D scissor{};
		scissor.offset = { 0, 0 };
		scissor.extent = swapChainExtent;
		vkCmdSetScissor(cmd.commandBuffer, 0, 1, &scissor);

		// BIND VETEX
		man.BindVertex(cmd.commandBuffer);

		// BIND DESCRIPTOR SETS
		auto dynamic_set = context->descriptorSets_.get(perFrameData[swapchain->currentFrame_].frameDynamicDescriptorSet);
		auto update_after_bind_set = context->descriptorSets_.get(perFrameData[swapchain->currentFrame_].frameDescriptorSet);

		std::vector<int> visible;

		auto view = camera.getViewMatrix();
		auto proj = camera.getProjMatrix();
		proj[1][1] *= -1;

		auto viewProj = proj * view; // Freeze at first for debug

		if (current_level.tree_done) {
			frustumCullBVH(
				current_level.static_tree,
				0,
				viewProj,
				camera.getPosition(),
				visible
			);
		}
		else {
			assert(false && "tree wasnt done");
		}

		// VISIBLE PIPELINES
		std::unordered_map<PipelineHandle, std::vector<int>> pipeline_map;

		for (uint32_t primSlot : visible) {
			if (primSlot >= current_level.static_tree.primToNode.size()) continue;

			uint32_t node = current_level.static_tree.primToNode[primSlot];

			auto it = current_level.materialForNode.find(node);
			if (it == current_level.materialForNode.end()) continue;

			uint32_t materialIndex = it->second;
			auto matHandle = man.materials.getHandle(materialIndex);
			auto mat = man.materials.get(matHandle);
			if (!mat) continue;

			// Lets just hardcode them to mainPipeline.
			// im doing empyty pipelines right now! be careful
			//assert(mat->pipeline);
			if (!mat->pipeline) {
				mat->pipeline = mainPipeline;
			}

			pipeline_map[mat->pipeline].push_back(primSlot); // NOT node
		}

		//assert(!pipeline_map.empty() && "map empty");
		if (pipeline_map.empty()) {
			vkCmdBeginRendering(cmd.commandBuffer, &cmd.renderingInfo);
			vkCmdEndRendering(cmd.commandBuffer);

			swapchain->AdvanceFrame();




			return; // This is illefgal
		}

		auto pipe_lyt = context->pipelines_.get(pipeline_map.begin()->first)->layout;

		uint32_t dynamicOffsets[2] = {
			frameUniformsBinding.dynamicOffsetFrame(swapchain->currentFrame_),
			drawDataBinding.dynamicOffsetFrame(swapchain->currentFrame_),
		};

		VkDescriptorSet sets[2] = {
			dynamic_set->set,
			update_after_bind_set->set,
		};


		vkCmdBindDescriptorSets(
			cmd.commandBuffer,
			//commandBuffers[swapchain.currentFrame_],
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			pipe_lyt.layout,
			0,
			2,
			sets,
			2,
			dynamicOffsets
		);

		// Rendering Info is not set up
		vkCmdBeginRendering(
			cmd.commandBuffer,
			&cmd.renderingInfo
		);

		MainViewPushConstants pc{};
		pc.model = glm::mat4(1.0f);

		for (const auto& [han_, visible] : pipeline_map)
		{
			//context->pipelines_.forEachHandle([&](PipelineHandle han_) {

			vkCmdBindPipeline(
				cmd.commandBuffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				context->pipelines_.get(han_)->pipeline
			);


			//for (int visible_node = 0; visible_node < level.scene.hierarchy.size(); visible_node++) {
			for (uint32_t visible_prim : visible) {
				uint32_t visible_node = current_level.static_tree.primToNode[visible_prim];

				auto it = current_level.meshForNode.find(visible_node);
				if (it == current_level.meshForNode.end()) continue;

				uint32_t meshIndex = it->second;

				auto mesh_desc = man.meshes.get(
					man.meshes.getHandle(meshIndex)
				);

				auto material_instance_index = current_level.materialForNode[visible_node];
				pc.material_index = material_instance_index;

				auto matHandle = man.materials.getHandle(material_instance_index);
				auto mat = man.materials.get(matHandle);

				assert(mat);
				//if (!mat) continue; // skip if invalid
				//if (mat->pipeline != han_) continue;

				Material::Values pcm = mat->values;

				struct PC_All {
					MainViewPushConstants pc;
					Material::Values mpc;
				};
				PC_All all{ pc, pcm };

				vkCmdPushConstants(
					cmd.commandBuffer,
					pipe_lyt.layout,
					VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
					0, //offset to start of the range to push
					sizeof(PC_All), // SIZE
					&all
				);

				for (auto& draw_set : mesh_desc->surfaces) {

					auto& model_ = current_level.globalTransform[visible_node];
					vkCmdPushConstants(
						cmd.commandBuffer,
						//commandBuffers[swapchain.currentFrame_],
						pipe_lyt.layout,
						VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
						offsetof(MainViewPushConstants, model),
						sizeof(glm::mat4),
						glm::value_ptr(model_)
					);

					vkCmdDrawIndexed(
						cmd.commandBuffer,
						//commandBuffers[swapchain.currentFrame_],
						draw_set.index_count,
						1,
						draw_set.first_index,
						draw_set.base_vertex,
						0
					);

				}
			}
		}

		UiPass();

		ImGui::Render();

		// Record the ImGui draw data into your Vulkan command buffer.
		// You must already be inside a render pass (or dynamic rendering block)
		ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd.commandBuffer);

		vkCmdEndRendering(
			cmd.commandBuffer
		);

		// SUBMITs
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = 0;  // Nothing needs to read it before present
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = swapchain->GetCurrentImage();
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(
			cmd.commandBuffer,
			//commandBuffers[swapchain.currentFrame_],
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, // after rendering finishes
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,          // before presentation
			0,
			0, nullptr,
			0, nullptr,
			1, &barrier
		);

		VkResult result;

		if (vkEndCommandBuffer(cmd.commandBuffer) != VK_SUCCESS) {
			throw std::runtime_error("failed to record command buffer!");
		}
		// Submit Frame 
		{

			VkSubmitInfo submitInfo{};
			submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

			VkSemaphore waitSemaphores[] = { swapchain->imageAvailableSemaphores_[swapchain->currentFrame_] };
			VkPipelineStageFlags waitStages[] = {
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
			submitInfo.waitSemaphoreCount = 1;
			submitInfo.pWaitSemaphores = waitSemaphores;
			submitInfo.pWaitDstStageMask = waitStages;

			submitInfo.commandBufferCount = 1;
			//submitInfo.pCommandBuffers = &commandBuffers[swapchain->currentFrame_];
			submitInfo.pCommandBuffers = &cmd.commandBuffer;

			VkSemaphore signalSemaphores[] = { swapchain->renderFinishedSemaphores_[swapchain->currentFrame_] };
			submitInfo.signalSemaphoreCount = 1;
			submitInfo.pSignalSemaphores = signalSemaphores;

			if (
				vkQueueSubmit(
					context->default_queue,
					1,
					&submitInfo,
					swapchain->GetInFlightFence()
				) != VK_SUCCESS
				) {

				assert(false && "failed to submit draw");
				throw std::runtime_error("failed to submit draw command buffer!");
			}


			VkPresentInfoKHR presentInfo{};
			presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

			presentInfo.waitSemaphoreCount = 1;
			presentInfo.pWaitSemaphores = signalSemaphores;

			VkSwapchainKHR swapChains[] = { swapchain->swapchain_ };
			presentInfo.swapchainCount = 1;
			presentInfo.pSwapchains = swapChains;
			presentInfo.pImageIndices = &swapchain->currentImageIndex_;

			result = vkQueuePresentKHR(presentQueue, &presentInfo);

			if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
				framebufferResized) {
				framebufferResized = false;
				swapchain->Recreate();
			}
			else if (result != VK_SUCCESS) {
				throw std::runtime_error("failed to present swap chain image!");
			}

		}

		swapchain->AdvanceFrame();
		//currentFrame = (currentFrame + 1) % framesInFlight;

	}


};

void TestApp::UpdateCamera(float dt)
{
    auto& movement = cameraPositioner.movement_;

    movement.forward_ = input.isKeyHeld(GLFW_KEY_W);
    movement.backward_ = input.isKeyHeld(GLFW_KEY_S);
    movement.left_ = input.isKeyHeld(GLFW_KEY_A);
    movement.right_ = input.isKeyHeld(GLFW_KEY_D);

	// Check the behacviour of this. 
	// is up and down in camera orientation, not world space.
    movement.up_ = input.isKeyHeld(GLFW_KEY_E) || input.isKeyHeld(GLFW_KEY_SPACE);
    movement.down_ = input.isKeyHeld(GLFW_KEY_Q) || input.isKeyHeld(GLFW_KEY_LEFT_CONTROL);

    movement.fastSpeed_ =
        input.isKeyHeld(GLFW_KEY_LEFT_SHIFT) ||
        input.isKeyHeld(GLFW_KEY_RIGHT_SHIFT);

	// NDC, but to frameBuffer dimensions. 
	//glm::vec2 mousePos = glm::vec2(
	//	(static_cast<float>(input.getMouseX()) / static_cast<float>(swapchain->GetExtent().width)) * 2.0f - 1.0f,
	//	1.0f - (static_cast<float>(input.getMouseY()) / static_cast<float>(swapchain->GetExtent().height)) * 2.0f
	//);
	
	// NDC, to window dimensions. These two may not match, depends on DPI settings. Be careful.
	// Input should be normalized and not be affected by resolution!
	glm::vec2 mousePos = glm::vec2(
		(static_cast<float>(input.getMouseX()) / static_cast<float>(swapchain->windowWidth)) * 2.0f - 1.0f,
		1.0f - (static_cast<float>(input.getMouseY()) / static_cast<float>(swapchain->windowHeight)) * 2.0f
	);

    const bool mousePressed =
        input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_RIGHT);

    if (input.isMouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT))
    {
        cameraPositioner.resetMousePosition(mousePos);
    }

    cameraPositioner.update(
        static_cast<double>(dt),
        mousePos,
        mousePressed
    );
}
