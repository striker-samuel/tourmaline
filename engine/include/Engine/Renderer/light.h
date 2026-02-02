#pragma once

#include <Engine/shared/common.h>


// ---------- Host Descriptions ------------

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


enum LightType : int32_t {
	LIGHT_TYPE_POINT = 0,
	LIGHT_TYPE_SPOT = 1
	//LIGHT_TYPE_DIRECTIONAL = 2, // This will just go out trough other means.
};

struct alignas(16) Light {
	glm::vec4 color = glm::vec4(1.0f);       // offset 0,  size 16

	float position[3] = { 0.0f,0.0f,0.0f };     // offset 16, size 12
	int32_t flags = 0;
	//float _pad0;           // offset 28, size 4  -> total 32

	float direction[3] = { 0.0f,0.0f,0.0f };    // offset 32, size 12
	float intensity = .0f;       // offset 44, size 4  -> total 48

	int32_t type = LIGHT_TYPE_POINT;
	// in radians!
	float innerAngle = glm::radians(10.0f);
	float outerAngle = glm::radians(20.0f);
	int shadow_map_index = -1;	 // This is the index to the ShadowMapSlot
	float lightVP[16] = {};
};

static_assert(sizeof(Light) % 16 == 0);


// Right now this is very obviously all AOS code. It may be changed later to SOA

// -------------- GPU descriptions ---------------

// Lets just hardcode the different types of lights. 
// they do need different data and are implemented quiter differently. Data is king, so from now on they are separated. 
struct alignas(16) GpuDirectionalLightDescription {

	glm::vec4 color = glm::vec4(1.0f);
	float direction[3] = { 0.0f,0.0f,0.0f };
	int32_t flags = 0;
	float intensity = .0f;
	int shadow_map_index = -1; // This may change for cascade reasons. dont know yet.
	float pad_;
	float lightVP[16] = {};
};

static_assert(sizeof(GpuDirectionalLightDescription) % 16 == 0);

struct GpuCascadeDescription
{
	float lightVP[16];

	float splitEnd;        // view-space depth end for this cascade
	float bias;
	float normalBias;
	float pad_;

	glm::vec2 invShadowSize;  // 1/(atlasWidth, atlasHeight) OR 1/(tileWidth,tileHeight) depending how you do PCF
	glm::vec2 uvScale;        // tile scale in atlas UV
	glm::vec2 uvBias;         // tile offset in atlas UV

	glm::vec2 uvClampMin;     // clamp inside tile (avoid bleeding)
	glm::vec2 uvClampMax;
	float pad2_[2];
};

static_assert(sizeof(GpuCascadeDescription) % 16 == 0);

struct alignas(16) GpuSpotLightDescription {

	glm::vec4 color = glm::vec4(1.0f);       // offset 0,  size 16

	float position[3] = { 0.0f,0.0f,0.0f };     // offset 16, size 12
	int32_t flags = 0;
	//float _pad0;           // offset 28, size 4  -> total 32

	float direction[3] = { 0.0f,0.0f,0.0f };    // offset 32, size 12
	float intensity = .0f;       // offset 44, size 4  -> total 48


	float _pad1;
	float innerAngle = glm::radians(10.0f);
	float outerAngle = glm::radians(20.0f);

	int shadow_map_index = -1;	 // This is the index to the ShadowMapSlot
	float lightVP[16] = {};
};

static_assert(sizeof(GpuSpotLightDescription) % 16 == 0);

// Spot lights


glm::mat4 GetSpotLightView(const SpotLight& l);


// Perspective lights

glm::mat4 GetPerspectiveProjectionForLight(const SpotLight& l, float aspect = 1.0f, float nearPlane = 0.1f, float farPlane = 100.0f);

void GetDirectionalLightOrthoVP(
	const DirectionalLight& l,
	const glm::mat4& cameraView,
	const glm::mat4& cameraProj,
	float shadowNear,
	float shadowFar,
	glm::mat4& outView,
	glm::mat4& outProj,
	// Optional quality/stability knobs:
	float zPadding,  // world units padding along light Z (prevents clipping)
	//	float xyPadding,   // world units padding in light X/Y (edge safety)
	bool  enableTexelSnap,   // reduce shimmering
	int   shadowMapRes    // used for snapping; set to your shadow map size
);

// This gets you 8 worldSpace positions that are the corners of the boundingBox of the frustrum of camera proj and view used. Uses mainView camera conventions, so reverse-z infinite.
static std::array<glm::vec3, 8> GetFrustumCornersWorldSpace_Rays_ReverseZInfFarFriendly(
	const glm::mat4& proj,
	const glm::mat4& view,
	float nearDist,
	float farDist
);
