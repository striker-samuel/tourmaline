#pragma once

#include <Engine/shared/common.h>

enum LightType : int32_t {
	LIGHT_TYPE_POINT = 0,
	LIGHT_TYPE_SPOT = 1,
	LIGHT_TYPE_DIRECTIONAL = 2,
	// LIGHT_TYPE_AREA
};

struct alignas(16) Light {
	glm::vec4 color = glm::vec4(1.0f);       // offset 0,  size 16

	float position[3] = { 0.0f,0.0f,0.0f };     // offset 16, size 12
	int32_t flags = 0;
	//float _pad0;           // offset 28, size 4  -> total 32

	float direction[3] = { 0.0f,0.0f,0.0f };    // offset 32, size 12
	float intensity = 1.0f;       // offset 44, size 4  -> total 48

	int32_t type = LIGHT_TYPE_POINT;
	// in radians!
	float innerAngle = glm::radians(10.0f);
	float outerAngle = glm::radians(20.0f);
	int shadow_map_index = -1;
	float lightVP[16] = {};
};

static_assert(sizeof(Light) % 16 == 0);

glm::mat4 GetLightView(const Light& l);
glm::mat4 GetLightProj(const Light& l, float aspect = 1.0f, float nearPlane = 0.1f, float farPlane = 100.0f);

