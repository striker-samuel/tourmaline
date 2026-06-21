#pragma once
#include <cassert>

// DEFAULT INITIALIZATION

static constexpr uint32_t DEFAULT_WIDTH = 1280;
static constexpr uint32_t DEFAULT_HEIGHT = 720;

// Maybe move this to swapchain.h??
// Right now the depth render target for the main view is reused every frame
// Thing is only one frame does render at a time due to single graghics queue with synchronization.
static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
static constexpr uint32_t DESIRED_SWAPCHAIN_IMAGE_COUNT = 2;

// RESOURCES
static constexpr uint32_t MAX_BUFFERS = 4096;
static constexpr uint32_t MAX_DESCRIPTORS = 4096;
static constexpr uint32_t MAX_DRAWS = 1024;
static constexpr uint32_t MAX_INSTANCES = 1000;
static constexpr uint32_t MAX_MESHES = 1000;
static constexpr uint32_t MAX_LIGHTS = 10;
static constexpr uint32_t MAX_TEXTURES = 4096;
static constexpr uint32_t MAX_IMAGES = 4096;


static constexpr uint32_t MAX_PUSH_CONSTANTS = 8;

// CAMERA
static constexpr float default_near_clip = 0.01f;
static constexpr float default_far_clip = 50.0f;