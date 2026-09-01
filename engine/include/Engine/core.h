#pragma once


// These are enabled at Cmake Engine project level
//GLM_FORCE_RADIANS
//GLM_FORCE_DEPTH_ZERO_TO_ONE
//GLM_ENABLE_EXPERIMENTAL
// 
//TINYGLTF_NO_STB_IMAGE
//TINYGLTF_NO_STB_IMAGE_WRITE
//TINYGLTF_NO_EXTERNAL_IMAGE
// 
//GLFW_INCLUDE_NONE

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/hash.hpp>
#include <GLFW/glfw3.h>

#include "smvk.h"
#include <Engine/RHI/resource_manager.h>
#include <Engine/RHI/swapchain.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/App/app.h>
#include <Engine/App/world.h>
#include <Engine/Utils/gltf.h> 
#include <Engine/platform.h>
#include <Engine/Camera/camera.h> 
#include <Engine/Renderer/light.h>
#include <Engine/ImGui/imgui_overlay.h>

