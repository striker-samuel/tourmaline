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

#include "platform.h"
#include "smvk.h"

#ifdef ENGINE_VULKAN
#include <Engine/Vulkan/impl.h>
#include <Engine/Vulkan/vulkanApp.h>
#endif
