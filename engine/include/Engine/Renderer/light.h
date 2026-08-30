#pragma once

#include <glm/glm.hpp>

namespace smvk {

	struct PointLight {
		glm::vec4 color = glm::vec4(1.0f);
		glm::vec3 position = { 0.0f, 0.0f, 0.0f };
		int32_t flags = 0;
		float intensity = .0f;
		int shadowSlot = -1;
	};

	struct SpotLight {
		glm::vec4 color = glm::vec4(1.0f);
		glm::vec3 position = { 0.0f, 0.0f, 0.0f };
		glm::vec3 direction = { 0.0f,0.0f,0.0f };
		int32_t flags = 0;
		float intensity = .0f;
		float innerAngle = glm::radians(10.0f);
		float outerAngle = glm::radians(20.0f);
		int shadowSlot = -1;
	};

	struct DirectionalLight {
		glm::vec4 color = glm::vec4(1.0f);
		// No use now but maybe used for masks 
		int32_t flags = 0;
		float direction[3] = { 0.0f,0.0f,0.0f };
		float intensity = .0f;
		int shadowSlot = -1;
	};

	struct alignas(16) Light {
		glm::vec4 color = glm::vec4(1.0f);
		float position[3] = { 0.0f,0.0f,0.0f };
		int32_t flags = 0;
		float direction[3] = { 0.0f,0.0f,0.0f };
		float intensity = .0f;
		int32_t pad_;
		float innerAngle = glm::radians(10.0f);
		float outerAngle = glm::radians(20.0f);
		int shadow_map_index = -1;
		float lightVP[16] = {};
	};

	static_assert(sizeof(Light) % 16 == 0);

	// -------------- GPU descriptions ---------------

	struct GpuCascadeDescription
	{
		float lightVP[16];
		float splitEnd;
		float bias;
		float normalBias;
		float pad_;
		glm::vec2 invShadowSize;
		glm::vec2 uvScale;
		glm::vec2 uvBias;
		glm::vec2 uvClampMin;
		glm::vec2 uvClampMax;
		float pad2_[2];
	};
	static_assert(sizeof(GpuCascadeDescription) % 16 == 0);

	// Lets just hardcode the different types of lights. 
	// they do need different data and are implemented quiter differently. Data is king, so from now on they are separated. 
	struct alignas(16) GpuDirectionalLightDescription
	{
		glm::vec4 color = glm::vec4(1.0f);
		float direction[3] = { 0.0f,0.0f,0.0f };
		int32_t flags = 0;
		float intensity = .0f;
		int shadow_map_index = -1;
		int numCascades = 1;
		float pad_[3];
		float lightVP[16] = {};
		GpuCascadeDescription cascades[4];
	};
	static_assert(sizeof(GpuDirectionalLightDescription) % 16 == 0);


	struct alignas(16) GpuSpotLightDescription {
		glm::vec4 color = glm::vec4(1.0f);
		float position[3] = { 0.0f,0.0f,0.0f };
		int32_t flags = 0;
		float direction[3] = { 0.0f,0.0f,0.0f };
		float intensity = .0f;
		float _pad1;
		float innerAngle = glm::radians(10.0f);
		float outerAngle = glm::radians(20.0f);
		int shadow_map_index = -1;
		float lightVP[16] = {};
	};
	static_assert(sizeof(GpuSpotLightDescription) % 16 == 0);

	glm::mat4 GetSpotLightView(
		const SpotLight& l
	);
	void GetDirectionalLightOrthoVP(
		const glm::vec3 lightDir,
		const glm::mat4& invCameraView,
		const glm::mat4& invCameraProj,
		float shadowNear,
		float shadowFar,
		glm::mat4& outView,
		glm::mat4& outProj,
		float zPadding,
		bool enableTexelSnap,
		int shadowMapRes
	);
	glm::mat4 GetPerspectiveProjectionForLight(
		const SpotLight& l,
		float aspect = 1.0f,
		float nearPlane = 0.1f,
		float farPlane = 100.0f
	);

};
