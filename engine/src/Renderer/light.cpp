#include <Engine/Renderer/light.h>

#include <array>
#include <cfloat>
#include <cmath>
#include <algorithm>


template<typename T>
inline float length2(const T& v) {
	return glm::dot(v, v);
}

glm::mat4 MakeOrthoRH_ZO(
	float left, float right,
	float bottom, float top,
	float zNear, float zFar
)
{
	// Maps:
	//  x -> [-1, 1]
	//  y -> [-1, 1]
	//  z -> [ 0, 1]   (Vulkan ZO)
	//
	// Assumes right-handed view space where camera looks down -Z (GLM lookAt style).
	// zNear/zFar are in the same space as your light-view transformed points.

	const float rl = right - left;
	const float tb = top - bottom;
	const float fn = zFar - zNear;

	glm::mat4 m(1.0f);
	m[0][0] = 2.0f / rl;
	m[1][1] = 2.0f / tb;
	m[2][2] = -1.0f / fn;                 // note the minus (RH, looking down -Z)
	m[3][0] = -(right + left) / rl;
	m[3][1] = -(top + bottom) / tb;
	m[3][2] = -zNear / fn;                // ZO: near->0, far->1
	return m;
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


// Production-ish directional light VP fit for a camera frustum slice.
// - Uses ray-based frustum corners (works with reverse-Z + infinite far camera)
// - Enforces near/far ordering
// - Removes zMult hacks
// - Adds configurable Z padding
// - Optional texel snapping for stability (reduces shimmering)
//
// Assumptions:
// - cameraProj is the *actual* camera projection you render with (reverse-Z / infinite ok)
// - shadowNear/shadowFar are *finite* distances along camera view rays (CSM slice range)
// - outProj is built as a normal (non-reverse-Z) ortho by default; keep your shadow pass as
//   clearDepth=1.0 and compare=LESS/LEQUAL.
//
// Notes:
// - If you flip Y for Vulkan in your pipeline with proj[1][1]*=-1, do that AFTER calling this.

static std::array<glm::vec3, 8> GetFrustumCornersWorldSpace_Rays_ReverseZInfFarFriendly(
	const glm::mat4& proj,
	const glm::mat4& view,
	float nearDist,
	float farDist
)
{
	// Camera world transform
	const glm::mat4 invView = glm::inverse(view);
	const glm::vec3 camPos = glm::vec3(invView[3]); // translation column in GLM

	// Inverse projection to unproject clip -> view
	const glm::mat4 invProj = glm::inverse(proj);

	// NDC XY corners
	const glm::vec2 ndcXY[4] = {
		{-1.0f, -1.0f},
		{ 1.0f, -1.0f},
		{ 1.0f,  1.0f},
		{-1.0f,  1.0f},
	};

	std::array<glm::vec3, 8> corners{};

	for (int i = 0; i < 4; ++i)
	{
		// IMPORTANT:
		// - w MUST be 1 (a point), not 0 (a direction).
		// - For reverse-Z camera projections, the near plane maps to NDC.z = 1.
		//   (Your MakeInfReversedZProjRH uses reverse-Z.)
		const glm::vec4 clip(ndcXY[i].x, ndcXY[i].y, 1.0f, 1.0f);

		glm::vec4 viewPt = invProj * clip;

		viewPt /= viewPt.w;

		glm::vec3 dirView = glm::vec3(viewPt);

		dirView = glm::normalize(dirView);

		// Transform direction to world (ignore translation)
		glm::vec3 dirWorld = glm::normalize(glm::mat3(invView) * dirView);

		corners[i + 0] = camPos + dirWorld * nearDist;
		corners[i + 4] = camPos + dirWorld * farDist;
	}

	return corners;
}

static glm::vec3 ChooseSafeUpForLookAt(const glm::vec3& forward /*normalized*/)
{
	glm::vec3 up(0.0f, 1.0f, 0.0f);
	if (std::abs(glm::dot(forward, up)) > 0.99f)
		up = glm::vec3(0.0f, 0.0f, 1.0f);
	return up;
}

void GetDirectionalLightOrthoVP(
	const Light& l,
	const glm::mat4& cameraView,
	const glm::mat4& cameraProj,
	float shadowNear,
	float shadowFar,
	glm::mat4& outView,
	glm::mat4& outProj,
	// Optional quality/stability knobs:
	float zPadding,  // world units padding along light Z (prevents clipping)
	float xyPadding,   // world units padding in light X/Y (edge safety)
	bool  enableTexelSnap,   // reduce shimmering
	int   shadowMapRes    // used for snapping; set to your shadow map size
)
{
	assert(l.type == LIGHT_TYPE_DIRECTIONAL);
	if (l.type != LIGHT_TYPE_DIRECTIONAL)
		return;

	if (!(shadowFar > shadowNear))
		assert(false && "shadowFar was less than shadowNear");
	//		return;

		// Normalize light direction
	glm::vec3 lightDir = glm::normalize(glm::vec3(l.direction[0], l.direction[1], l.direction[2]));

	// 8 corners of the camera frustum slice (works for reverse-Z + infinite far)
	const auto corners = GetFrustumCornersWorldSpace_Rays_ReverseZInfFarFriendly(
		cameraProj, cameraView, shadowNear, shadowFar
	);

	// Compute slice center
	glm::vec3 center(0.0f);
	for (const auto& c : corners) center += c;
	center /= static_cast<float>(corners.size());

	// Build light view matrix with a safe up vector
	const glm::vec3 up = ChooseSafeUpForLookAt(lightDir);

	// "Backoff" just needs to be large enough so the camera sees the fitted box.
	// It does not affect the ortho extents since we compute them from transformed corners.
	//const float lightBackoff = 100.0f;
	const float lightBackoff = 100.0f;
	
	outView = glm::lookAt(
		center - lightDir * lightBackoff,
		center,
		up
	);

	// Compute bounds in light space
	glm::vec3 minLS(FLT_MAX);
	glm::vec3 maxLS(-FLT_MAX);

	for (const auto& c : corners)
	{
		const glm::vec3 ls = glm::vec3(outView * glm::vec4(c, 1.0f));
		minLS = glm::min(minLS, ls);
		maxLS = glm::max(maxLS, ls);
	}

	// Pad X/Y to avoid edge clipping (optional)
	minLS.x -= xyPadding; maxLS.x += xyPadding;
	minLS.y -= xyPadding; maxLS.y += xyPadding;

	float minZ = minLS.z - zPadding; // most negative (farthest)
	float maxZ = maxLS.z + zPadding; // least negative (closest)

	// Convert view-space Z (negative in front) to positive distances
	float zNear = std::max(0.001f, -maxZ);       // closest distance
	float zFar = std::max(zNear + 0.001f, -minZ); // farthest distance


	// Optional: texel snapping in light space (stabilizes shadows during camera motion)
	if (enableTexelSnap && shadowMapRes > 0)
	{
		const float width = (maxLS.x - minLS.x);
		const float height = (maxLS.y - minLS.y);

		// Prevent divide-by-zero
		if (width > 1e-6f && height > 1e-6f)
		{
			const float worldUnitsPerTexelX = width / static_cast<float>(shadowMapRes);
			const float worldUnitsPerTexelY = height / static_cast<float>(shadowMapRes);

			minLS.x = std::floor(minLS.x / worldUnitsPerTexelX) * worldUnitsPerTexelX;
			maxLS.x = std::floor(maxLS.x / worldUnitsPerTexelX) * worldUnitsPerTexelX;

			minLS.y = std::floor(minLS.y / worldUnitsPerTexelY) * worldUnitsPerTexelY;
			maxLS.y = std::floor(minLS.y / worldUnitsPerTexelY) * worldUnitsPerTexelY + height;
			// (keep height consistent; avoids slow drift)
		}
	}

	outProj = glm::orthoRH_ZO(minLS.x, maxLS.x, minLS.y, maxLS.y, zNear, zFar);
}

// Perspective projection for the light (spot light shadow map)
glm::mat4 GetPerspectiveProjectionForLight(
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

glm::mat4 GetOrthoProjectionForLight(
	const Light& l,
	float width,
	float height
) {
	assert(l.type == LIGHT_TYPE_DIRECTIONAL);
	return glm::ortho(0.0f, width, 0.0f, height, -1.0f, 1.0f);

}



