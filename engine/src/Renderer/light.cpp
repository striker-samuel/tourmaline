// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

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
//	//
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
	float* outDepthRange,
	const ShadowCasterFitBounds* casterBounds,
	float maxShadowDistance
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
	float maxAlongLight = -FLT_MAX;

	for (const glm::vec3& corner : corners)
	{
		const float a = glm::dot(corner - center, lightDir);
		minAlongLight = std::min(minAlongLight, a);
		maxAlongLight = std::max(maxAlongLight, a);
	}

	// The slice's own extents along the light axis (from the slice center):
	// sunward (toward the sun, a < 0) and scene-ward (a > 0).
	const float sunwardHalf = -minAlongLight;
	const float sceneHalf = maxAlongLight;

	// ---- Caster-aware depth coverage (the P2 gap) ----
	// The contribution view retains casters arbitrarily far sunward; the
	// slice-only fit put the light (sunwardHalf + zPadding) off the center,
	// so a caster beyond that clipped at the light's near plane and its
	// shadow popped. Fold the casters' extent along the light axis into the
	// backoff + depth range, CLAMPED to slice extent + maxShadowDistance —
	// the explicit pop bound (a world-scale caster AABB cannot explode the
	// range; the clamp is a documented decision, not luck).
	bool useCasters = casterBounds && casterBounds->valid;
	float casterMinA = 0.0f, casterMaxA = 0.0f; // light-axis interval
	if (useCasters)
	{
		const float sunCap = sunwardHalf + maxShadowDistance;
		const float sceneCap = sceneHalf + maxShadowDistance;
		float mnA = FLT_MAX, mxA = -FLT_MAX;
		for (int i = 0; i < 8; ++i)
		{
			const glm::vec3 c(
				(i & 1) ? casterBounds->max.x : casterBounds->min.x,
				(i & 2) ? casterBounds->max.y : casterBounds->min.y,
				(i & 4) ? casterBounds->max.z : casterBounds->min.z);
			const float a = glm::dot(c - center, lightDir);
			mnA = std::min(mnA, a);
			mxA = std::max(mxA, a);
		}
		// Clamp the interval into the coverage window. A degenerate result
		// (the whole AABB past the sun cap) drops the caster fit — the
		// slice-only behavior stands.
		casterMinA = std::max(mnA, -sunCap);
		casterMaxA = std::min(mxA, sceneCap);
		if (casterMaxA < casterMinA)
			useCasters = false;
	}
	const float needSunward = useCasters
		? std::max(sunwardHalf, -casterMinA) : sunwardHalf;
	const float needScene = useCasters
		? std::max(sceneHalf, casterMaxA) : sceneHalf;

	float lightBackoff =
		needSunward + zPadding;

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

	// Z-only extension from the (clamped) caster interval: the depth range
	// must cover the casters the contribution cull retained. Only the
	// light-axis extremes matter — z_view = -(lightBackoff + a) for any
	// point with axis offset a, regardless of its perpendicular position —
	// and the XY bounds above stay slice-fit on purpose: a caster whose
	// shadow can REACH the slice shares its receiver's light-space xy
	// (translation along the light axis moves depth only), so the slice
	// window already contains every relevant caster's xy.
	if (useCasters)
	{
		minBounds.z = std::min(minBounds.z, -(lightBackoff + casterMaxA));
		maxBounds.z = std::max(maxBounds.z, -(lightBackoff + casterMinA));
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
	// This is not the old hardcoded coverage radius.
	// It only expands the calculated frustum bounds slightly so tiny numerical
	// errors don't clip shadows exactly at the edge.
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
	//
	//	glm::vec3 ndc =
	//		glm::vec3(clip) / clip.w;
	//
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

// ---------------------------------------------------------------------------
// Point-light shadow faces (see light.h GetPointShadowFaceVP doc).
//
// Per-face basis derived from the cubemap convention table
// (cubemap.h FaceUvToDirection + cubemap_prefilter DirectionToFaceUv). The
// CONTRACT: NDC.x must equal the face's su(d) and NDC.y its sv(d) - i.e.
// the tile's UV grid IS the cubemap face's UV grid, so the sampling side
// (dominant-axis selection + su/sv solve) and the raster side (this VP)
// land on the same texel. glm::lookAt(pos, pos+fwd, up) derives
// s = cross(fwd, up) (the +NDC.x axis) and u = cross(s, fwd) (the +NDC.y
// axis); passing the table's (fwd, up) satisfies the contract:
//   face 0 +X: su=-z/x, sv=-y/x  -> fwd +X, up -Y  (s -Z, u -Y)
//   face 1 -X: su=-z/x, sv=+y/x  -> fwd -X, up -Y  (s +Z, u -Y; the negated
//        fwd flips the w denominator, so both NDC axes flip vs +X - the
//        up that reproduces the table is still -Y, NOT +Y)
//   face 2 +Y: su=+x/y, sv=+z/y  -> fwd +Y, up +Z  (s +X, u +Z)
//   face 3 -Y: su=-x/y, sv=+z/y  -> fwd -Y, up -Z  (s +X, u -Z)
//   face 4 +Z: su=+x/z, sv=-y/z  -> fwd +Z, up -Y  (s +X, u -Y)
//   face 5 -Z: su=+x/z, sv=+y/z  -> fwd -Z, up -Y  (s -X, u -Y)
// (Verified per-face in test_light.cpp: the tile UV must equal
// DirectionToFaceUv's (u,v) for the same direction - a sign error on any
// face mirrors that face's tile and the parity check catches it.)
//
// Why the basis must be axis-aligned (not any rotated frame): the
// dominant-axis partition only bounds |NDC.x|, |NDC.y| <= 1 when each
// NDC axis isolates ONE non-dominant world component. A frame rotated
// 45 degrees in the face plane would mix both components and push the
// NDC corners out to sqrt(2) - the 90-degree frustum would not cover
// the face's dominant-axis square.
// ---------------------------------------------------------------------------
glm::mat4 smvk::GetPointShadowFaceVP(
	const glm::vec3& position,
	uint32_t face,
	float range,
	float nearPlane
)
{
	static constexpr glm::vec3 kFaceFwd[6] = {
		glm::vec3( 1.0f, 0.0f, 0.0f), // +X
		glm::vec3(-1.0f, 0.0f, 0.0f), // -X
		glm::vec3( 0.0f, 1.0f, 0.0f), // +Y
		glm::vec3( 0.0f,-1.0f, 0.0f), // -Y
		glm::vec3( 0.0f, 0.0f, 1.0f), // +Z
		glm::vec3( 0.0f, 0.0f,-1.0f), // -Z
	};
	static constexpr glm::vec3 kFaceUp[6] = {
		glm::vec3(0.0f,-1.0f, 0.0f), // +X (s -Z, u -Y)
		glm::vec3(0.0f,-1.0f, 0.0f), // -X (s +Z, u -Y) - NOT +Y, see above
		glm::vec3(0.0f, 0.0f, 1.0f), // +Y (s +X, u +Z)
		glm::vec3(0.0f, 0.0f,-1.0f), // -Y (s +X, u -Z)
		glm::vec3(0.0f,-1.0f, 0.0f), // +Z (s +X, u -Y)
		glm::vec3(0.0f,-1.0f, 0.0f), // -Z (s -X, u -Y)
	};
	if (face > 5u)
		face = 0u;

	const glm::vec3 fwd = kFaceFwd[face];
	const glm::vec3 up = kFaceUp[face];
	const glm::mat4 view = glm::lookAt(position, position + fwd, up);
	// 90-degree FOV + unit aspect: tan(45deg) = 1, so a direction whose
	// dominant axis is `fwd` lands at |NDC.x|, |NDC.y| <= 1 exactly (the
	// boundary |su| = 1 / |sv| = 1 is the face seam shared with the
	// neighbor). far = the light's range (cutoff; receivers beyond it are
	// unlit anyway - the falloff window kills them before the shadow test).
	const float farPlane = std::max(range, nearPlane + 0.01f);
	const glm::mat4 proj = glm::perspectiveRH_ZO(
		glm::radians(90.0f), 1.0f, nearPlane, farPlane);
	// No Y-flip, same shared-matrix contract as the spot/cascade paths.
	return proj * view;
}

// LiSPSM (Light Space Perspective Shadow Maps) for spotlights.
// Warps the spot projection to match the camera's perspective frustum,
// providing better texel density in camera/view space.
// Based on LiSPSM (Light Space Perspective Shadow Maps) by Wimmer et al.
// Returns the LiSPSM view-projection matrix for the spot light.
glm::mat4 smvk::GetSpotLightVPLiSPSM(
	const glm::vec3& position,
	const glm::vec3& direction,
	float outerAngle,
	float range,
	float nearPlane,
	float aspect,
	const glm::mat4& cameraView,
	const glm::mat4& cameraProj,
	float cameraNear,
	float cameraFar
)
{
	// Standard spot light VP (without LiSPSM)
	glm::vec3 dir = direction;
	if (glm::dot(dir, dir) < 1e-6f)
		dir = glm::vec3(0.0f, -1.0f, 0.0f);
	dir = glm::normalize(dir);

	glm::vec3 up(0.0f, 1.0f, 0.0f);
	if (glm::abs(glm::dot(dir, up)) > 0.99f)
		up = glm::vec3(0.0f, 0.0f, 1.0f);

	const glm::mat4 lightView = glm::lookAt(position, position + dir, up);
	const float farPlane = std::max(range, nearPlane + 0.01f);
	const float fov = glm::clamp(outerAngle * 2.0f, 0.05f, glm::radians(170.0f));
	const glm::mat4 lightProj = glm::perspectiveRH_ZO(fov, aspect, nearPlane, farPlane);

	// LiSPSM: Warp the light projection to match camera perspective
	// Transform camera frustum corners to light view space
	const glm::mat4 invCameraView = glm::inverse(cameraView);
	const glm::mat4 invCameraProj = glm::inverse(cameraProj);

	// Get camera frustum corners in world space
	std::array<glm::vec3, 8> frustumCorners = GetFrustumCornersWorldSpace_Rays_ReverseZInfFarFriendly(
		invCameraProj, invCameraView, cameraNear, cameraFar);

	// Transform to light view space
	glm::vec3 minBounds(FLT_MAX), maxBounds(-FLT_MAX);
	for (const auto& corner : frustumCorners) {
		glm::vec4 lightSpacePos = lightView * glm::vec4(corner, 1.0f);
		minBounds = glm::min(minBounds, glm::vec3(lightSpacePos));
		maxBounds = glm::max(maxBounds, glm::vec3(lightSpacePos));
	}

	// Compute LiSPSM projection matrix
	// We want to fit the light projection to the camera frustum in light space
	const float lightNear = std::max(minBounds.z, nearPlane);
	const float lightFar = std::min(maxBounds.z, farPlane);

	// Orthographic fit in X/Y, perspective in Z
	const float left = minBounds.x;
	const float right = maxBounds.x;
	const float bottom = minBounds.y;
	const float top = maxBounds.y;

	// Create LiSPSM projection matrix
	glm::mat4 lispProj(1.0f);
	lispProj[0][0] = 2.0f / (right - left);
	lispProj[1][1] = 2.0f / (top - bottom);
	lispProj[2][2] = -1.0f / (lightFar - lightNear);  // Vulkan ZO: near=0, far=1
	lispProj[3][0] = -(right + left) / (right - left);
	lispProj[3][1] = -(top + bottom) / (top - bottom);
	lispProj[3][2] = -lightNear / (lightFar - lightNear);  // ZO: near->0, far->1

	// Note: no Y-flip (same convention as standard shadow): shadow pass and sampler share this matrix
	return lispProj * lightView;
}