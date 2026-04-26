#pragma once
#include <Engine/App/app.h>


//  Not same layer
#include <Engine/Renderer/ringBinding.h>


std::vector<std::string> gltf_meshes_paths = {
	"content/models/Sponza/sponza.gltf"
};


class TestApp : public VulkanApp {
	using Super = VulkanApp;


	struct FrameResources {
		DescriptorSetHandle frameDescriptorSet;
		DescriptorSetHandle frameDynamicDescriptorSet;
	};
	FrameResources perFrameData[MAX_FRAMES_IN_FLIGHT];

	//
	RingBinding<FrameUniforms, 1> frameUniformsBinding;
	RingBinding<DrawSetData, MAX_DRAWS> drawDataBinding;


	// 
	CameraPositioner_FirstPerson cameraPositioner = CameraPositioner_FirstPerson(
		glm::vec3(0.0f, 1.0f, 0.0f), //pos
		glm::vec3(0.0f, 1.0f, 1.0f), //target
		glm::vec3(0.0f, 1.0f, 0.0f) //up
	);
	Camera camera = Camera(cameraPositioner);

	//
	PipelineHandle mainPipeline; // app shouldnt not be responsible for this. 

	void EarlyUpdate() {
		Super::EarlyUpdate();

		double x, y;
		glfwGetCursorPos(window, &x, &y);

		cameraPositioner.update(delta_time_s,
			glm::vec2(x / swapchain->extent_.width, y / -swapchain->extent_.height),
			rightMousePressed);

	}

	void Update() {
		Super::Update();
	}

	void LateUpdate() {
		Super::LateUpdate();
	}


	// Don't now if the callbacks are set to these. with the inheritance
	void handleMouseScroll(double xOffset, double yOffset) {
		const float scrollSpeed = 1.0f;
		glm::vec3 forward = cameraPositioner.getForward();
		cameraPositioner.setPosition(
			cameraPositioner.getPosition() + static_cast<float>(yOffset) * scrollSpeed * forward
		);
	}

	void handleMouseMovement(double xpos, double ypos) {
		//if (!rightMousePressed) return;
		//glm::vec2 mousePos = glm::vec2(
		//	static_cast<float>(xpos) / width_,
		//	static_cast<float>(ypos) / -height_
		//);
		//cameraPositioner.update(delta_time_s,
		//	mousePos,
		//	true
		//);
	}

	void handleKey(int key, int scancode, int action, int mods) {
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

	bool Initialize() {
		if (!Super::Initialize())
			return false;

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
				VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, // createFlags
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
							   sizeof(MaterialPushConstants)),
			}
		);

		// Just define renderPasses and grah and go.
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

		Scene imported_scene;
		std::filesystem::path p(gltf_meshes_paths[0]);
		UploadGltfToContext(context, p, imported_scene);

		Level lvl = {
			.scene = imported_scene,
			.tree = treeFromScene(context, imported_scene),
			.ctx = context,
			.tree_done = true
		};
		level = lvl;

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

		const uint32_t materialCount = static_cast<uint32_t>(context->materialInstances_.numObjects());

		std::vector<VkDescriptorImageInfo> imageInfos;
		std::vector<VkDescriptorImageInfo> samplerInfos;

		int count = (context->images_.numObjects());
		imageInfos.reserve(count);
		samplerInfos.reserve(count);

		context->images_.forEach([&](Image obj_) {

			// This is fast and crude fix. 
			// Swapchain depth image is created on the same pool and other images are too.
			// swapchaim color images are manages by the VkSwapchain object tho.
			if (!obj_.is_tex) { return; };

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


		int width_ = swapchain->current_width;
		int height_ = swapchain->current_height;

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
		BindVertex(cmd.commandBuffer);

		// BIND DESCRIPTOR SETS
		auto dynamic_set = context->descriptorSets_.get(perFrameData[swapchain->currentFrame_].frameDynamicDescriptorSet);
		auto update_after_bind_set = context->descriptorSets_.get(perFrameData[swapchain->currentFrame_].frameDescriptorSet);

		std::vector<int> visible;

		auto view = camera.getViewMatrix();
		auto proj = camera.getProjMatrix();
		proj[1][1] *= -1;

		auto viewProj = proj * view; // Freeze at first for debug


		if (level.tree_done) {
			frustumCullBVH(
				level.tree,
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

		for (uint32_t primId : visible) {
			uint32_t node = level.tree.primToNode[primId];

			auto it = level.scene.materialForNode.find(node);
			if (it == level.scene.materialForNode.end()) continue;

			uint32_t materialIndex = it->second;
			auto matHandle = context->materialInstances_.getHandle(materialIndex);
			auto mat = context->materialInstances_.get(matHandle);

			pipeline_map[mat->pipeline].push_back(node);

		}


		uint32_t dynamicOffsets[2] = {
			frameUniformsBinding.dynamicOffsetFrame(swapchain->currentFrame_),
			drawDataBinding.dynamicOffsetFrame(swapchain->currentFrame_),
		};

		VkDescriptorSet sets[2] = {
			dynamic_set->set,
			update_after_bind_set->set,
		};

		auto pipe_lyt = context->pipelines_.get(pipeline_map.begin()->first)->layout;

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

		for (const auto& [han_, visible] : pipeline_map) {
			//context->pipelines_.forEachHandle([&](PipelineHandle han_) {

			vkCmdBindPipeline(
				cmd.commandBuffer,
				//commandBuffers[swapchain.currentFrame_],
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				context->pipelines_.get(han_)->pipeline
			);

			for (uint32_t visible_node : visible) {
				//uint32_t i = tree_.primToNode[primId];

				auto it = level.scene.meshForNode.find(visible_node);
				if (it == level.scene.meshForNode.end()) continue;

				uint32_t meshIndex = it->second;

				auto mesh_desc = context->meshes_.get(
					context->meshes_.getHandle(meshIndex)
				);

				auto material_instance_index = level.scene.materialForNode[visible_node];
				pc.material_index = material_instance_index;

				auto matHandle = context->materialInstances_.getHandle(material_instance_index);
				auto mat = context->materialInstances_.get(matHandle);
				if (!mat) continue; // skip if invalid
				if (mat->pipeline != han_) continue;

				//// skip primitive if material's pipeline doesn't match current obj_
				//auto pipelineObj = context->pipelines_.get(mat->pipeline);
				//if (pipelineObj != obj_) continue;

				MaterialPushConstants pcm = mat->values;

				struct PC_All {
					MainViewPushConstants pc;
					MaterialPushConstants mpc;
				};
				PC_All all{ pc, pcm };

				vkCmdPushConstants(
					cmd.commandBuffer,
					//commandBuffers[swapchain.currentFrame_],
					pipe_lyt.layout,
					VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
					0, //offset to start of the range to push
					sizeof(PC_All), // size to push
					&all
				);

				for (auto& draw_set : mesh_desc->surfaces) {

					auto& model_ = level.scene.globalTransform[visible_node];
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

		//ImGui::Render();

		//// Record the ImGui draw data into your Vulkan command buffer.
		//// You must already be inside a render pass (or dynamic rendering block)
		//ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd.commandBuffer);

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

		if (vkEndCommandBuffer(commandBuffers[swapchain->currentFrame_]) != VK_SUCCESS) {
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
			submitInfo.pCommandBuffers = &commandBuffers[swapchain->currentFrame_];

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
