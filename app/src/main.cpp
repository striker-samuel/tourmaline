#pragma once

// This si the main cu so we want the implementation in this one but this is ugly as hell?
#define TINYGLTF_IMPLEMENTATION


#include <Engine/App/app.h>

//  Not same layer
#include <Engine/Renderer/ringBinding.h>

class TestApp : public VulkanApp {
	using Super = VulkanApp;

	CameraPositioner_FirstPerson cameraPositioner = CameraPositioner_FirstPerson(
		glm::vec3(0.0f, 1.0f, 0.0f), //pos
		glm::vec3(0.0f, 1.0f, 1.0f), //target
		glm::vec3(0.0f, 1.0f, 0.0f) //up
	);
	Camera camera_ = Camera(cameraPositioner);


	void Initialize() {
		Super::Initialize();

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


		//auto pipe_obj = context->pipelines_.get(mainPipeline);
		//auto aa = context->pipelineLayouts_.get(pipe_obj->plyt);
		//auto bb = aa->setLayouts;

		for (int i = 0; i < swapchain->framesInFlight_; i++) {

			perFrameData[i].frameDynamicDescriptorSet = context->createDescriptorSet(
				bb[0]
			);

			perFrameData[i].frameDescriptorSet = context->createDescriptorSet(
				bb[1]
			);
		}


		//camera_->setPerspective(45.0f, float(width_) / float(height_), default_near_clip, default_far_clip);

		Scene imported_scene;
		std::filesystem::path p(gltf_meshes_paths[0]);
		UploadGltfToContext(context, p, imported_scene);


		Level lvl = {
			.scene = imported_scene,
			.tree = treeFromScene(context, imported_scene),
			.ctx = context,
			.tree_done = true
		};

		//SetDescriptorSets();
		//UpdateBindlessTexturesSets();
	};


	void Record(VulkanApp& app) {



		/// Culling. Frameuniforms Uplaod ois no t here also depends on camera.
		std::vector<int> visible;

		auto view = camera_->getViewMatrix();
		auto proj = camera_->getProjMatrix();
		proj[1][1] *= -1;

		auto viewProj = proj * view; // Freeze at first for debug

		frustumCullBVH(
			*app->level.tree,
			0,
			viewProj,
			camera_->getPosition(),
			visible
		);

		// VISIBLE PIPELINES
		std::unordered_map<PipelineHandle, std::vector<int>> pipeline_map;

		for (uint32_t primId : visible) {
			uint32_t node = tree_.primToNode[primId];

			auto it = scene_.materialForNode.find(node);
			if (it == scene_.materialForNode.end()) continue;

			uint32_t materialIndex = it->second;
			auto matHandle = context->materialInstances_.getHandle(materialIndex);
			auto mat = context->materialInstances_.get(matHandle);

			pipeline_map[mat->pipeline].push_back(node);

		}

		auto com = AcquireMain();
		auto swapChainExtent = app.swapchain->GetExtent();


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
		app.BindVertex(commandBuffer);

		// BIND DESCRIPTOR SETS
		auto dynamic_set = context->descriptorSets_.get(perFrameData[swapchain.currentFrame_].frameDynamicDescriptorSet);
		auto update_after_bind_set = context->descriptorSets_.get(perFrameData[swapchain.currentFrame_].frameDescriptorSet);

		uint32_t dynamicOffsets[2] = {
			frameUniformsBinding.dynamicOffsetFrame(swapchain.currentFrame_),
			drawDataBinding.dynamicOffsetFrame(swapchain.currentFrame_),
		};

		VkDescriptorSet sets[2] = {
			dynamic_set->set,
			update_after_bind_set->set,
		};

		auto pipe_lyt = context->pipelines_.get(pipeline_map.begin()->first)->plyt;
		auto pipeline_layout = context->pipelineLayouts_.get(pipe_lyt);

		vkCmdBindDescriptorSets(
			com.commandBuffer,
			//commandBuffers[swapchain.currentFrame_],
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			pipeline_layout->layout,
			0,
			2,
			sets,
			2,
			dynamicOffsets
		);

		//


		vkCmdBeginRendering(
			cmd.commandBuffer,
			&cmd.renderingInfo
		);

		MainViewPushConstants pc{};
		pc.model = glm::mat4(1.0f);

		for (const auto& [han_, visible] : pipeline_map) {
			//context->pipelines_.forEachHandle([&](PipelineHandle han_) {

			vkCmdBindPipeline(
				com.commandBuffer,
				//commandBuffers[swapchain.currentFrame_],
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				context->pipelines_.get(han_)->pipeline
			);

			for (uint32_t visible_node : visible) {
				//uint32_t i = tree_.primToNode[primId];

				auto it = scene_.meshForNode.find(visible_node);
				if (it == scene_.meshForNode.end()) continue;

				uint32_t meshIndex = it->second;

				auto mesh_desc = context->meshes_.get(
					context->meshes_.getHandle(meshIndex)
				);

				auto material_instance_index = scene_.materialForNode[visible_node];
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
					com.commandBuffer,
					//commandBuffers[swapchain.currentFrame_],
					pipeline_layout->layout,
					VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
					0, //offset to start of the range to push
					sizeof(PC_All), // size to push
					&all
				);

				for (auto& draw_set : mesh_desc->surfaces) {

					auto& model_ = scene_.globalTransform[visible_node];
					vkCmdPushConstants(
						com.commandBuffer,
						//commandBuffers[swapchain.currentFrame_],
						pipeline_layout->layout,
						VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
						offsetof(MainViewPushConstants, model),
						sizeof(glm::mat4),
						glm::value_ptr(model_)
					);

					vkCmdDrawIndexed(
						com.commandBuffer,
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
		//ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);

		vkCmdEndRendering(
			com.commandBuffer
			//commandBuffers[swapchain.currentFrame_]
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
		barrier.image = swapchain.GetCurrentImage();
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(
			com.commandBuffer,
			//commandBuffers[swapchain.currentFrame_],
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, // after rendering finishes
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,          // before presentation
			0,
			0, nullptr,
			0, nullptr,
			1, &barrier
		);

	}

};

// --------------APP--------------------

int main(int argc, const char* argv[]) {
	if (argc > 1) {
		std::cout << argv[0];
	}

	TestApp app = TestApp();
	try {
		app.Run();
	}
	catch (const std::exception& e) {
		std::cerr << e.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}


std::vector<std::string> gltf_meshes_paths = {
	"content/models/Sponza/sponza.gltf" // Index 0
};
//

PipelineHandle mainPipeline;
RingBinding<FrameUniforms, 1> frameUniformsBinding;
RingBinding<DrawSetData, MAX_DRAWS> drawDataBinding;

struct FrameResources {
	DescriptorSetHandle frameDescriptorSet;
	DescriptorSetHandle frameDynamicDescriptorSet;
};

FrameResources perFrameData[MAX_FRAMES_IN_FLIGHT];

