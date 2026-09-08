#include <glm/gtc/matrix_transform.hpp>
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
static std::array<glm::vec3, 8>
GetFrustumCornersWorldSpace_Rays_ReverseZInfFarFriendly(
	const glm::mat4& invProj,
	const glm::mat4& invView,
	float nearDist,
	float farDist
)
{
	const glm::vec2 ndcXY[4] =
	{
		{-1.0f, -1.0f},
		{ 1.0f, -1.0f},
		{ 1.0f,  1.0f},
		{-1.0f,  1.0f},
	};

	std::array<glm::vec3, 8> corners{};

	for (int i = 0; i < 4; ++i)
	{
		// Reverse-Z near plane = NDC z 1.
		glm::vec4 clip(
			ndcXY[i].x,
			ndcXY[i].y,
			1.0f,
			1.0f
		);

		glm::vec4 viewPt = invProj * clip;
		viewPt /= viewPt.w;

		glm::vec3 rayView = glm::vec3(viewPt);

		// RH camera looks down -Z.
		// Scale the ray so that its Z depth is exactly nearDist/farDist.
		const float invForwardDepth =
			1.0f / (-rayView.z);

		glm::vec3 nearView =
			rayView * (nearDist * invForwardDepth);

		glm::vec3 farView =
			rayView * (farDist * invForwardDepth);

		corners[i + 0] =
			glm::vec3(invView * glm::vec4(nearView, 1.0f));

		corners[i + 4] =
			glm::vec3(invView * glm::vec4(farView, 1.0f));
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
	int shadowMapRes,
	float* outDepthRange
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
	float minAlongLight = FLT_MAX;

	for (const glm::vec3& corner : corners)
	{
		minAlongLight = std::min(
			minAlongLight,
			glm::dot(corner - center, lightDir)
		);
	}

	float lightBackoff =
		-minAlongLight + zPadding;

	glm::vec3 lightPosition =
		center - lightDir * lightBackoff;

	outView = glm::lookAt(
		lightPosition,
		center,
		ChooseSafeUpForLookAt(lightDir)
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
	// Snap the light view's translation column (the light-space position of
	// the world origin) onto the texel grid. The slice cannot do this job:
	// the view is glued to the frustum-slice center, so every light-space
	// quantity derived from the slice (bounds, center, their deltas) is
	// invariant under pure camera translation, and quantizing any of them
	// cannot stop the texel grid from sliding. Rounding the view origin
	// instead makes the world-space texel grid jump in whole-texel steps as
	// the camera translates, which removes the shadow-edge shimmer.
	//
	if (enableTexelSnap && shadowMapRes > 0)
	{
		// Provisional texel sizes from the tight fit.
		const float unitsPerTexelX0 =
			(maxX - minX) / static_cast<float>(shadowMapRes);

		const float unitsPerTexelY0 =
			(maxY - minY) / static_cast<float>(shadowMapRes);

		// Grow the window two texels per axis: the quantized view leaves
		// the slice up to one texel off-center, so the tight fit alone
		// could clip a texel-wide strip at the edge.
		minX -= 2.0f * unitsPerTexelX0;
		maxX += 2.0f * unitsPerTexelX0;
		minY -= 2.0f * unitsPerTexelY0;
		maxY += 2.0f * unitsPerTexelY0;

		// Texel sizes of the grown window; quantization must use the final
		// size so each grid step is exactly one texel.
		const float unitsPerTexelX =
			(maxX - minX) / static_cast<float>(shadowMapRes);

		const float unitsPerTexelY =
			(maxY - minY) / static_cast<float>(shadowMapRes);

		const float snappedX =
			std::floor(outView[3][0] / unitsPerTexelX) * unitsPerTexelX;

		const float snappedY =
			std::floor(outView[3][1] / unitsPerTexelY) * unitsPerTexelY;

		const float dx = snappedX - outView[3][0];
		const float dy = snappedY - outView[3][1];

		// Move the light-space coordinate system itself. Z is untouched
		// (delta.z = 0), so the depth fit below is unaffected.
		outView =
			glm::translate(
				glm::mat4(1.0f),
				glm::vec3(dx, dy, 0.0f)
			) *
			outView;

		// Deliberately do NOT shift minX/maxX/minY/maxY along with the
		// view: the ortho offset exactly cancels a matching view shift
		// (ortho(l + d, r + d) = ortho(l, r) * T(-d)), which is what made
		// the previous center-snapping implementation a no-op.
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

	if (outDepthRange != nullptr)
		*outDepthRange = zFar - zNear;

	// Debufging fit
	//glm::mat4 lightVP =
	//	outProj * outView;

	//for (const glm::vec3& corner : corners)
	//{
	//	glm::vec4 clip =
	//		lightVP * glm::vec4(corner, 1.0f);

	//	glm::vec3 ndc =
	//		glm::vec3(clip) / clip.w;

	//	printf(
	//		"NDC: %f %f %f\n",
	//		ndc.x,
	//		ndc.y,
	//		ndc.z
	//	);
	//}
}

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

glm::mat4 smvk::GetSpotLightVP(
	const glm::vec3& position,
	const glm::vec3& direction,
	float outerAngle,
	float range,
	float nearPlane,
	float aspect
)
{
	glm::vec3 dir = direction;
	if (glm::dot(dir, dir) < 1e-6f)
		dir = glm::vec3(0.0f, -1.0f, 0.0f);
	dir = glm::normalize(dir);

	glm::vec3 up(0.0f, 1.0f, 0.0f);
	if (glm::abs(glm::dot(dir, up)) > 0.99f)
		up = glm::vec3(0.0f, 0.0f, 1.0f);

	const glm::mat4 view = glm::lookAt(position, position + dir, up);
	const float farPlane = std::max(range, nearPlane + 0.01f);
	// Clamp FOV so extreme outer angles can't produce a degenerate matrix.
	const float fov = glm::clamp(outerAngle * 2.0f, 0.05f, glm::radians(170.0f));
	const glm::mat4 proj = glm::perspectiveRH_ZO(fov, aspect, nearPlane, farPlane);
	// NOTE: no Y-flip (same convention as BuildDirectionalLight): the shadow
	// pass and the sampler share this exact matrix.
	return proj * view;
}
