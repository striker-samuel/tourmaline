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

std::array<glm::vec3, 8> GetFrustumCornersWorldSpace(
	const glm::mat4& proj,
	const glm::mat4& view
)
{
	const glm::mat4 inv = glm::inverse(proj * view);

	std::array<glm::vec3, 8> corners;
	int i = 0;

	for (int x = 0; x < 2; ++x)
		for (int y = 0; y < 2; ++y)
			for (int z = 0; z < 2; ++z)
			{
				glm::vec4 pt = inv * glm::vec4(
					2.0f * x - 1.0f,
					2.0f * y - 1.0f,
					2.0f * z - 1.0f,
					1.0f
				);
				corners[i++] = glm::vec3(pt) / pt.w;
			}

	return corners;
}

glm::mat4 GetDirectionalLightOrthoVP(
	const Light& l,
	const glm::mat4& cameraView,
	const glm::mat4& cameraProj
)
{
	assert(l.type == LIGHT_TYPE_DIRECTIONAL);

	if (l.type != LIGHT_TYPE_DIRECTIONAL)
		return glm::mat4(1.0f); // safe-ish fallback (or return cached last-good VP)

	glm::vec3 lightDir = glm::normalize(glm::vec3(
		l.direction[0],
		l.direction[1],
		l.direction[2]
	));

	// Build light view matrix
	glm::vec3 center = glm::vec3(0.0f);
	auto corners = GetFrustumCornersWorldSpace(cameraProj, cameraView);
	for (auto& c : corners)
		center += c;
	center /= corners.size();

	glm::mat4 lightView = glm::lookAt(
		center - lightDir * 100.0f,
		center,
		glm::vec3(0, 1, 0)
	);

	// Compute bounds in light space
	glm::vec3 min(FLT_MAX);
	glm::vec3 max(-FLT_MAX);

	for (auto& c : corners)
	{
		glm::vec3 ls = glm::vec3(lightView * glm::vec4(c, 1.0));
		min = glm::min(min, ls);
		max = glm::max(max, ls);
	}

	// Optional: stabilize Z range
	const float zMult = 10.0f;
	if (min.z < 0) min.z *= zMult;
	else           min.z /= zMult;
	if (max.z < 0) max.z /= zMult;
	else           max.z *= zMult;

	glm::mat4 lightProj = glm::ortho(
		min.x, max.x,
		min.y, max.y,
		min.z, max.z
	);

	return lightProj * lightView;
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

