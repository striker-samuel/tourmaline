#include <Engine/Renderer/light.h>

template<typename T>
inline float length2(const T& v) {
	return glm::dot(v, v);
}


// Convert your Light to a view matrix (for spot / "directional-like" usage)
glm::mat4 GetLightView(const Light& l)
{
	glm::vec3 pos(l.position[0], l.position[1], l.position[2]);
	glm::vec3 dir(l.direction[0], l.direction[1], l.direction[2]);

	// Avoid zero-length direction
	if (length2(dir) < 1e-6f)
		dir = glm::vec3(0.0f, -1.0f, 0.0f); // default downwards

	dir = glm::normalize(dir);

	// Choose an up vector that isn't parallel to dir
	glm::vec3 up(0.0f, 1.0f, 0.0f);
	if (glm::abs(glm::dot(dir, up)) > 0.99f)
		up = glm::vec3(0.0f, 0.0f, 1.0f);

	return glm::lookAt(pos, pos + dir, up);
}

// Perspective projection for the light (spot light shadow map)
glm::mat4 GetLightProj(
	const Light& l,
	float aspect,
	float nearPlane,
	float farPlane
)
{
	// For spot lights, use outerAngle to define the FOV
	if (l.type == LIGHT_TYPE_SPOT) {
		// outerAngle is in degrees in your struct; FOV = 2 * outerAngle
		float fovRadians = l.outerAngle * 2.0f;
		return glm::perspective(fovRadians, aspect, nearPlane, farPlane);
	}

	// For point lights, this is only a placeholder if you *don't* do cubemap shadows.
	// Proper point light shadows need 6 views + same projection.
	float fovRadians = glm::radians(90.0f); // 90° for each cube face, if you go that route
	return glm::perspective(fovRadians, 1.0f, nearPlane, farPlane);
}

