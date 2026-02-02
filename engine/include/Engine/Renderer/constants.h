#pragma once

static constexpr uint32_t WIDTH = 1280;
static constexpr uint32_t HEIGHT = 720;

// Double buffering :)
static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
static constexpr int DESIRED_SWAPCHAING_IMAGE_COUNT = 2;

//static constexpr VkFormat SHADOWMAP_DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
static constexpr VkFormat SHADOWMAP_DEPTH_FORMAT = VK_FORMAT_D16_UNORM;

static constexpr int SHADOWMAP_SLOTS = 16;

static constexpr float default_near_clip = 0.01f;
static constexpr float default_far_clip = 50.0f;

// Yes, I'm hardcapping here for development.
static constexpr uint32_t MAX_INSTANCES = 1000;
static constexpr uint32_t MAX_MESHES = 1000;
static constexpr uint32_t MAX_LIGHTS = 10;
static constexpr uint32_t MAX_TEXTURES = 4096;
static constexpr uint32_t MAX_BUFFERS = 4096;
static constexpr uint32_t MAX_MATERIAL_INSTANCES = 1000;
static constexpr uint32_t MAX_MATERIALS = 1000;

