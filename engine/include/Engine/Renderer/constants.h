#pragma once
#include <cassert>

static constexpr uint32_t WIDTH = 1280;
static constexpr uint32_t HEIGHT = 720;


// Right now the depth render target for the main view is reused every frame
// Thing is only one frame does render at a time due to single graghics queue with synchronization.
static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
static constexpr uint32_t DESIRED_SWAPCHAIN_IMAGE_COUNT = 2;

//static constexpr VkFormat SHADOWMAP_DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
static constexpr VkFormat SHADOWMAP_DEPTH_FORMAT = VK_FORMAT_D16_UNORM;

static constexpr int SHADOWMAP_SLOTS = 16;

static constexpr float default_near_clip = 0.01f;
static constexpr float default_far_clip = 50.0f;

static constexpr uint32_t MAX_DRAWS = 1024;
static constexpr uint32_t MAX_INSTANCES = 1000;
static constexpr uint32_t MAX_MESHES = 1000;
static constexpr uint32_t MAX_LIGHTS = 10;

static constexpr uint32_t MAX_TEXTURES = 4096;
static constexpr uint32_t MAX_IMAGES = 4096;

static constexpr uint32_t MAX_BUFFERS = 4096;  // This is crazy high in reality
static constexpr uint32_t MAX_MATERIAL_INSTANCES = 1000;
static constexpr uint32_t MAX_MATERIALS = 1000;

static constexpr uint32_t MAX_PUSH_CONSTANTS = 8;
//static constexpr uint32_t MAX_PUSH_CONSTANTS_SIZE = 64;

