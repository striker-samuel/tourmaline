#include <Engine/Renderer/light.h>

#include <array>
#include <cfloat>
#include <cmath>
#include <algorithm>


template<typename T>
inline float length2(const T& v) {
	return glm::dot(v, v);
}

//glm::mat4 MakeOrthoRH_ZO(
//	float left, float right,
//	float bottom, float top,
//	float zNear, float zFar
//)
//{
//	// Maps:
//	//  x -> [-1, 1]
//	//  y -> [-1, 1]
//	//  z -> [ 0, 1]   (Vulkan ZO)
//	//
//	// Assumes right-handed view space where camera looks down -Z (GLM lookAt style).
//	// zNear/zFar are in the same space as your light-view transformed points.
//
//	const float rl = right - left;
//	const float tb = top - bottom;
//	const float fn = zFar - zNear;
//
//	glm::mat4 m(1.0f);
//	m[0][0] = 2.0f / rl;
//	m[1][1] = 2.0f / tb;
//	m[2][2] = -1.0f / fn;                 // note the minus (RH, looking down -Z)
//	m[3][0] = -(right + left) / rl;
//	m[3][1] = -(top + bottom) / tb;
//	m[3][2] = -zNear / fn;                // ZO: near->0, far->1
//	return m;
//}


glm::mat4 smvk::GetSpotLightView(const smvk::SpotLight& l)
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

static std::array<glm::vec3, 8> smvk::GetFrustumCornersWorldSpace_Rays_ReverseZInfFarFriendly(
	const glm::mat4& invProj,
	const glm::mat4& invView,
	float nearDist,
	float farDist
)
{
	const glm::vec3 camPos = glm::vec3(invView[3]); // translation column in GLM

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

void smvk::GetDirectionalLightOrthoVP(
	const glm::vec3 lightDirInput,
	const glm::mat4& invCameraView,
	const glm::mat4& invCameraProj,
	float shadowNear,
	float shadowFar,
	glm::mat4& outView,
	glm::mat4& outProj,
	float zPadding,
	bool enableTexelSnap,
	int shadowMapRes
)
{
	if (!(shadowFar > shadowNear))
	{
		assert(false && "shadowFar must be greater than shadowNear");
		return;
	}

	glm::vec3 lightDir = lightDirInput;

	if (glm::dot(lightDir, lightDir) < 1e-6f)
	{
		assert(false && "Directional light direction is zero");
		lightDir = glm::vec3(0.0f, -1.0f, 0.0f);
	}

	lightDir = glm::normalize(lightDir);

	// Build the world-space corners of the camera frustum slice.
	const auto corners =
		GetFrustumCornersWorldSpace_Rays_ReverseZInfFarFriendly(
			invCameraProj,
			invCameraView,
			shadowNear,
			shadowFar
		);

	// Find center of the frustum slice.
	glm::vec3 center(0.0f);

	for (const glm::vec3& corner : corners)
	{
		center += corner;
	}

	center /= static_cast<float>(corners.size());

	// Construct light orientation.
	const glm::vec3 up = ChooseSafeUpForLookAt(lightDir);

	//
	// The exact distance of the light camera does not affect an orthographic
	// projection's X/Y coverage.
	//
	// We only need it far enough back that the entire relevant volume lies
	// in front of the light camera.
	//
	const float lightBackoff =
		(shadowFar - shadowNear) + zPadding;

	const glm::vec3 lightPosition =
		center - lightDir * lightBackoff;

	outView = glm::lookAt(
		lightPosition,
		center,
		up
	);

	// Transform the actual frustum slice into light space and find its bounds.
	glm::vec3 minBounds(FLT_MAX);
	glm::vec3 maxBounds(-FLT_MAX);

	for (const glm::vec3& corner : corners)
	{
		const glm::vec3 cornerLS =
			glm::vec3(outView * glm::vec4(corner, 1.0f));

		minBounds = glm::min(minBounds, cornerLS);
		maxBounds = glm::max(maxBounds, cornerLS);
	}

	//
	// Optional small XY safety padding.
	//
	// This is not the old hardcoded coverage radius.
	// It only expands the calculated frustum bounds slightly so tiny numerical
	// errors don't clip shadows exactly at the edge.
	//
	const float xyPadding = 0.1f;

	minBounds.x -= xyPadding;
	maxBounds.x += xyPadding;
	minBounds.y -= xyPadding;
	maxBounds.y += xyPadding;

	// Expand depth range so casters just outside the camera frustum can still
	// cast shadows into visible geometry.
	minBounds.z -= zPadding;
	maxBounds.z += zPadding;

	float minX = minBounds.x;
	float maxX = maxBounds.x;
	float minY = minBounds.y;
	float maxY = maxBounds.y;

	//
	// Optional texel snapping.
	//
	// Snap the orthographic projection center in light space while preserving
	// the dynamically calculated width/height.
	//
	if (enableTexelSnap && shadowMapRes > 0)
	{
		const float width = maxX - minX;
		const float height = maxY - minY;

		const float unitsPerTexelX =
			width / static_cast<float>(shadowMapRes);

		const float unitsPerTexelY =
			height / static_cast<float>(shadowMapRes);

		glm::vec2 centerLS(
			(minX + maxX) * 0.5f,
			(minY + maxY) * 0.5f
		);

		glm::vec2 snappedCenter;

		snappedCenter.x =
			std::floor(centerLS.x / unitsPerTexelX) *
			unitsPerTexelX;

		snappedCenter.y =
			std::floor(centerLS.y / unitsPerTexelY) *
			unitsPerTexelY;

		const glm::vec2 delta =
			snappedCenter - centerLS;

		// Move the light-space coordinate system itself.
		outView =
			glm::translate(
				glm::mat4(1.0f),
				glm::vec3(delta.x, delta.y, 0.0f)
			) *
			outView;

		// Since the view matrix moved by delta, shift the bounds by the same
		// amount.
		minX += delta.x;
		maxX += delta.x;
		minY += delta.y;
		maxY += delta.y;
	}

	//
	// glm::lookAt produces RH view space looking down -Z.
	//
	// glm::orthoRH_ZO expects positive near/far distances, so convert the
	// light-space Z coordinates.
	//
	const float zNear =
		std::max(0.001f, -maxBounds.z);

	const float zFar =
		std::max(zNear + 0.001f, -minBounds.z);

	outProj = glm::orthoRH_ZO(
		minX,
		maxX,
		minY,
		maxY,
		zNear,
		zFar
	);
}

//void smvk::GetDirectionalLightOrthoVP(
//	const glm::vec3 lightDir,
//	const glm::mat4& invCameraView,
//	const glm::mat4& invCameraProj,
//	float shadowNear,
//	float shadowFar,
//	glm::mat4& outView,
//	glm::mat4& outProj,
//	// Optional quality/stability knobs:
//	float zPadding,  // world units padding along light Z (prevents clipping)
//	//	float xyPadding,   // world units padding in light X/Y (edge safety)
//	bool  enableTexelSnap,   // reduce shimmering
//	int   shadowMapRes    // used for snapping; set to your shadow map size
//)
//{
//	float xyPadding = 0.1;
//
//	if (!(shadowFar > shadowNear))
//		assert(false && "shadowFar was less than shadowNear");
//	//		return;
//
//	// Normalize light direction
//	//glm::vec3 lightDir = glm::normalize(glm::vec3(l.direction[0], l.direction[1], l.direction[2]));
//
//	// 8 corners of the camera frustum slice (works for reverse-Z + infinite far)
//	const auto corners = GetFrustumCornersWorldSpace_Rays_ReverseZInfFarFriendly(
//		invCameraProj,
//		invCameraView,
//		shadowNear,
//		shadowFar
//	);
//
//	// Compute slice center
//	glm::vec3 center(0.0f);
//	for (const auto& c : corners) center += c;
//	center /= static_cast<float>(corners.size());
//
//	float radius = 100.0f;          // <-- hardcoded shadow coverage radius in WORLD units
//	float zExtent = 100.0f;        // <-- depth coverage along light direction (world units)
//	float lightBackoff = zExtent * 0.5f; // just needs to be enough to see the box
//	glm::vec3 up = ChooseSafeUpForLookAt(lightDir);
//
//	outView = glm::lookAt(center - lightDir * lightBackoff, center, up);
//	// Build a fixed-size box in LIGHT SPACE around the transformed center
//	glm::vec3 centerLS = glm::vec3(outView * glm::vec4(center, 1.0f));
//
//	float width = 2.0f * radius;
//	float height = 2.0f * radius;
//
//	if (enableTexelSnap && shadowMapRes > 0)
//	{
//		const float unitsPerTexelX = width / float(shadowMapRes);
//		const float unitsPerTexelY = height / float(shadowMapRes);
//
//		// Snap the LIGHT-SPACE center to the texel grid
//		glm::vec2 snappedCenter;
//		snappedCenter.x = std::floor(centerLS.x / unitsPerTexelX) * unitsPerTexelX;
//		snappedCenter.y = std::floor(centerLS.y / unitsPerTexelY) * unitsPerTexelY;
//
//		glm::vec2 delta = snappedCenter - glm::vec2(centerLS.x, centerLS.y);
//
//		// IMPORTANT: bake that delta into the VIEW matrix (light-space translation)
//		outView = glm::translate(glm::mat4(1.0f), glm::vec3(delta.x, delta.y, 0.0f)) * outView;
//
//		// Update centerLS to the snapped one (optional clarity)
//		centerLS.x = snappedCenter.x;
//		centerLS.y = snappedCenter.y;
//	}
//
//	// Now build min/max from (snapped) centerLS and FIXED extents
//	float minX = centerLS.x - radius;
//	float maxX = centerLS.x + radius;
//	float minY = centerLS.y - radius;
//	float maxY = centerLS.y + radius;
//
//	// Z: cover +/- zExtent/2 around centerLS.z
//	float minZ = centerLS.z - (zExtent * 0.5f);
//	float maxZ = centerLS.z + (zExtent * 0.5f);
//
//	// Convert view-space Z (negative in front) to positive distances for orthoRH_ZO
//	float zNear = std::max(0.001f, -maxZ);
//	float zFar = std::max(zNear + 0.001f, -minZ);
//
//	outProj = glm::orthoRH_ZO(minX, maxX, minY, maxY, zNear, zFar);
//
//	// Compute cascade split end (view-space depth)
//	// For cascades: splitEnd is the view-space depth where this cascade ends
//	// Positive view-space depth means looking along -Z from camera
//	// We need to transform shadowFar into view space to get splitEnd
//	float cascadeSplitEnd = shadowFar; // default: single cascade covers full range
//
//	// outView and outProj are now set up. The light VP will be outProj * outView.
//	// For cascade splitting, we'd need to adjust the projection based on splitEnd.
//	// For 1 cascade, the full lightVP is used directly.
//}

glm::mat4 smvk::GetPerspectiveProjectionForLight(
	const smvk::SpotLight& l,
	float aspect,
	float nearPlane,
	float farPlane
)
{
	float fovRadians = l.outerAngle * 2.0f;
	return glm::perspective(fovRadians, aspect, nearPlane, farPlane);
}

//std::array<glm::mat4,6 > GetProjectionsForSpotLight(
//	const PointLight& l,
//	float aspect,
//	float nearPlane,
//	float farPlane
//)
//{
//	float fovRadians = glm::radians(90.0f); // 90� for each cube face, if you go that route
//	return glm::perspective(fovRadians, 1.0f, nearPlane, farPlane);
//}
