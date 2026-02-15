#pragma once

#include <Engine/Shared/common.h> // Needed for vec3
#include <Engine/Renderer/vertex.h>

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


//// Specialized descirptios like these may go the way of the dodo or move to App 
struct MainViewPushConstants {
	glm::mat4 model;
	//glm::mat4 view;
	//glm::mat4 proj;
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
