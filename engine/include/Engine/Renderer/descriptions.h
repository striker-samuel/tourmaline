#pragma once

#include <Engine/Shared/common.h> // Needed for vec3
#include <Engine/Renderer/vertex.h>


enum CULLING_MODE {
	GPU_CULLING,
	CPU_CULLING
};

struct DrawSetData {
	// Identity Matrix initialization requires this ugly ass shit without goign into macros or else. 
	//alignas(16)
	float transform[16] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};
	glm::vec4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
};

static_assert(sizeof(DrawSetData) % 16 == 0);

/// --------------------------------------

struct MainViewPushConstants {
	glm::mat4 model;
	int material_index;
	uint32_t     _pad0[3];
};

struct MaterialPushConstants {

	// Color *=
	glm::vec4 ambient = glm::vec4(0.06f);
	glm::vec4 diffuse = glm::vec4(1.0f);
	int tex[4] = { -1, -1, -1, -1 }; //diffuse ,ambient, alpha // unused 

};

static_assert(sizeof(MainViewPushConstants) % 16 == 0);
static_assert(sizeof(MaterialPushConstants) % 16 == 0);

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

//

struct CascadeDescription
{
	glm::mat4 lightVP;
	float splitEnd;        // view-space depth end for this cascade
	float bias;
	float normalBias;

	glm::vec2 invShadowSize;  // 1/(atlasWidth, atlasHeight) OR 1/(tileWidth,tileHeight) depending how you do PCF
	glm::vec2 uvScale;        // tile scale in atlas UV
	glm::vec2 uvBias;         // tile offset in atlas UV
	glm::vec2 uvClampMin;     // clamp inside tile (avoid bleeding)
	glm::vec2 uvClampMax;
};

