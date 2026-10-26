// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// light_tests: CPU-side regression tests for the directional-light ortho
// fit + texel snap (Engine/Renderer/light.cpp). No GPU required: the snap
// is pure matrix math.
//
// The texel snap must do two things the previous implementation did not:
//   1. actually change lightVP (the old code shifted the view by delta and
//      then shifted the ortho bounds by the same delta, which cancels
//      exactly: ortho(l+d, r+d) = ortho(l, r) * T(-d)), and
//   2. keep the world-space texel grid quantized under pure camera
//      translation, so a fixed world point's shadow texel coords only
//      move in whole-texel steps (no sub-texel edge shimmer).
// Quantizing the view origin is the only workable lever: the view is glued
// to the frustum-slice center, so every slice-derived light-space quantity
// is translation-invariant and cannot stop the grid from sliding.
// Coverage is asserted too: the grown window must still contain the slice.

#include <array>
#include <cfloat>
#include <cmath>
#include <iostream>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <Engine/Renderer/light.h>
#include <Engine/Renderer/renderview.h> // the contribution view (the culling half of the contract)

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	++g_checks;
	if (cond) {
		std::cout << "PASS " << name << "\n";
	}
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

bool NearInteger(float v, float eps) {
	return std::abs(v - std::round(v)) <= eps;
}

float MaxAbsDiff(const glm::mat4& a, const glm::mat4& b) {
	float m = 0.0f;
	const float* pa = glm::value_ptr(a);
	const float* pb = glm::value_ptr(b);
	for (int k = 0; k < 16; ++k)
		m = (std::max)(m, std::abs(pa[k] - pb[k]));
	return m;
}

// Vulkan-style reverse-Z (near -> 1, far -> 0), ZO depth, Y-flipped like
// the app's cascade fit (world_renderer.cpp flips proj[1][1]).
glm::mat4 MakeReverseZProj(float fovY, float aspect, float n, float f) {
	const float s = 1.0f / std::tan(fovY * 0.5f);
	const float A = n / (f - n);
	const float B = n * f / (f - n);
	glm::mat4 P(0.0f);
	P[0][0] = s / aspect;
	P[1][1] = -s;
	P[2][2] = A;
	P[2][3] = -1.0f; // clip.w = -z_view
	P[3][2] = B;
	return P;
}

// Same ray-based corner extraction the engine uses (reverse-Z near plane
// sits at NDC z = 1); the slice corners double as the coverage probe.
std::array<glm::vec3, 8> SliceCorners(
	const glm::mat4& invProj,
	const glm::mat4& invView,
	float nearDist,
	float farDist)
{
	std::array<glm::vec3, 8> out{};
	const glm::vec2 ndcXY[4] = {
		{ -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f }
	};
	for (int i = 0; i < 4; ++i) {
		glm::vec4 viewPt =
			invProj * glm::vec4(ndcXY[i].x, ndcXY[i].y, 1.0f, 1.0f);
		viewPt /= viewPt.w;
		const glm::vec3 ray(viewPt);
		const float invFwd = 1.0f / (-ray.z);
		out[i] = glm::vec3(
			invView * glm::vec4(ray * (nearDist * invFwd), 1.0f));
		out[i + 4] = glm::vec3(
			invView * glm::vec4(ray * (farDist * invFwd), 1.0f));
	}
	return out;
}

glm::vec2 ShadowTexel(const glm::mat4& vp, const glm::vec3& world, float res) {
	const glm::vec4 clip = vp * glm::vec4(world, 1.0f);
	const glm::vec3 ndc = glm::vec3(clip) / clip.w;
	return (glm::vec2(ndc) * 0.5f + 0.5f) * res;
}

void TestDirectionalTexelSnap() {
	constexpr int kRes = 2048;
	constexpr float kShadowNear = 1.0f;
	constexpr float kShadowFar = 40.0f;
	constexpr float kZPadding = 1.0f;
	const glm::vec3 kLightDir =
		glm::normalize(glm::vec3(-0.35f, -1.0f, -0.22f));
	const glm::vec3 kFwd = glm::normalize(glm::vec3(0.2f, -0.1f, -1.0f));
	const glm::vec3 kCam0(3.21f, 2.5f, 7.4f);
	const glm::vec3 kStep(0.013f, 0.0f, 0.008f);
	constexpr int kSteps = 10;
	// Fixed world receiver inside the slice for every camera step.
	const glm::vec3 kReceiver = kCam0 + kFwd * 12.0f;

	const glm::mat4 proj =
		MakeReverseZProj(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 200.0f);
	const glm::mat4 invProj = glm::inverse(proj);

	bool snapChangesVP = false;
	bool snappedWholeTexels = true;
	bool snappedResnapped = false;
	bool unsnappedDrifts = false;
	bool sliceCovered = true;

	glm::vec2 prevSnapTexel(0.0f);
	glm::vec2 prevNoSnapTexel(0.0f);

	for (int i = 0; i < kSteps; ++i) {
		const glm::vec3 camPos =
			kCam0 + kStep * static_cast<float>(i);
		const glm::mat4 view =
			glm::lookAt(camPos, camPos + kFwd, glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 invView = glm::inverse(view);

		glm::mat4 viewS, projS, viewN, projN;
		smvk::GetDirectionalLightOrthoVP(
			kLightDir, invView, invProj, kShadowNear, kShadowFar,
			viewS, projS, kZPadding, true, kRes);
		smvk::GetDirectionalLightOrthoVP(
			kLightDir, invView, invProj, kShadowNear, kShadowFar,
			viewN, projN, kZPadding, false, kRes);

		const glm::mat4 vpS = projS * viewS;
		const glm::mat4 vpN = projN * viewN;

		// T1: the snap must alter lightVP (the old implementation was an
		// exact no-op: |VP_snap - VP_nosnap| ~ 1e-17).
		if (MaxAbsDiff(vpS, vpN) > 1e-6f)
			snapChangesVP = true;

		const glm::vec2 texelS =
			ShadowTexel(vpS, kReceiver, static_cast<float>(kRes));
		const glm::vec2 texelN =
			ShadowTexel(vpN, kReceiver, static_cast<float>(kRes));

		if (i > 0) {
			const glm::vec2 dS = texelS - prevSnapTexel;
			const glm::vec2 dN = texelN - prevNoSnapTexel;
			// T2: snapped path moves only in whole-texel steps.
			if (!NearInteger(dS.x, 0.01f) || !NearInteger(dS.y, 0.01f))
				snappedWholeTexels = false;
			// The grid must actually re-snap during the walk, otherwise
			// the whole-texel check is vacuous.
			if (std::abs(dS.x) > 0.5f || std::abs(dS.y) > 0.5f)
				snappedResnapped = true;
			// Control: without the snap the same walk slides the
			// receiver sub-texel (what the snap exists to remove).
			if (!NearInteger(dN.x, 0.01f) || !NearInteger(dN.y, 0.01f))
				unsnappedDrifts = true;
		}

		// T3: every slice corner stays inside the snapped light frustum.
		const auto corners =
			SliceCorners(invProj, invView, kShadowNear, kShadowFar);
		for (const glm::vec3& c : corners) {
			const glm::vec4 clip = vpS * glm::vec4(c, 1.0f);
			const glm::vec3 ndc = glm::vec3(clip) / clip.w;
			if (ndc.x < -1.0001f || ndc.x > 1.0001f ||
				ndc.y < -1.0001f || ndc.y > 1.0001f ||
				ndc.z < -0.0001f || ndc.z > 1.0001f)
				sliceCovered = false;
		}

		prevSnapTexel = texelS;
		prevNoSnapTexel = texelN;
	}

	Check(snapChangesVP, "light.texelsnap.changes-lightvp");
	Check(snappedResnapped, "light.texelsnap.resnaps-during-walk");
	Check(snappedWholeTexels, "light.texelsnap.whole-texel-steps");
	Check(unsnappedDrifts, "light.texelsnap.unsnapped-still-drifts");
	Check(sliceCovered, "light.texelsnap.slice-still-covered");
}

// ---------------------------------------------------------------------------
// Cascade depth-padding regression: LARGE-SCENE + LOW-SUN coverage
// (finding: the fixed CASCADE_Z_PADDING = 25 m is heuristic — these pin
// the padded depth range's actual envelope so any future widening/narrowing
// is a deliberate change, not a surprise).
//
// The envelope (from GetDirectionalLightOrthoVP's fit): the light sits
// (slice sunward half-extent + zPadding) sunward of the slice center and
// the ortho depth range spans [zNear ~ 0 (the light's eye plane),
// zFar = 2*sunwardHalf + 2*zPadding] — so a caster point ON the light
// ray through a receiver survives the light's near clip exactly while
// its sunward ray distance t (how far up the ray it sits from the
// receiver) satisfies
//     t <= sunwardHalf + zPadding
// (a caster nearer the sun than the light position falls behind the near
// plane and clips out — the documented "tall casters popping" case). For
// a ground-plane caster of height H with the sun at elevation e,
// t = H / sin(e): LOW SUN shrinks the covered caster height (the fixed
// padding contributes only padding*sin(e) of it), and LARGE scene scales
// grow the covered envelope through the slice's own extent. Both
// directions are pinned below against the measured slice geometry.
// ---------------------------------------------------------------------------
void TestCascadePaddingLargeSceneLowSun() {
	// The engine's WorldRenderer constant (keep in sync — that is the
	// heuristic under test).
	constexpr float kCascadeZPadding = 25.0f;
	constexpr int kRes = 2048;

	struct Fit {
		glm::mat4 vp;
		glm::vec3 sliceCenter;
		// The slice's sunward half-extent along the light axis — exactly
		// -minAlongLight from the fit (the amount the light backs off
		// beyond, plus the padding).
		float sunwardHalf;
	};
	auto fitLight = [&](const glm::vec3& lightDir, const glm::vec3& camPos,
			const glm::vec3& fwd, float shadowNear, float shadowFar,
			float camFar) -> Fit {
		const glm::mat4 proj = MakeReverseZProj(
			glm::radians(60.0f), 16.0f / 9.0f, 0.1f, camFar);
		const glm::mat4 invProj = glm::inverse(proj);
		const glm::mat4 view = glm::lookAt(camPos, camPos + fwd,
			glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 invView = glm::inverse(view);
		glm::mat4 lv, lp;
		smvk::GetDirectionalLightOrthoVP(lightDir, invView, invProj,
			shadowNear, shadowFar, lv, lp, kCascadeZPadding, true, kRes);
		const auto corners =
			SliceCorners(invProj, invView, shadowNear, shadowFar);
		glm::vec3 center(0.0f);
		for (const glm::vec3& c : corners)
			center += c;
		center /= float(corners.size());
		float minAlong = FLT_MAX; // the most sunward corner's offset
		for (const glm::vec3& c : corners)
			minAlong = (std::min)(minAlong, glm::dot(c - center, lightDir));
		return Fit{ lp * lv, center, -minAlong };
	};
	// A caster point is IN the light's clip volume (depth-inclusive: the
	// assertion is about DEPTH) when its NDC z lands in the ZO [0, 1]
	// range with a small epsilon.
	auto casterDepthInside = [](const Fit& f,
			const glm::vec3& caster) -> bool {
		const glm::vec4 clip = f.vp * glm::vec4(caster, 1.0f);
		const glm::vec3 ndc = glm::vec3(clip) / clip.w;
		return ndc.z >= -0.001f && ndc.z <= 1.001f;
	};
	// The sunward ray direction from a receiver (toward the sun).
	auto sunward = [](const glm::vec3& lightDir) {
		return -lightDir; // lightDir points sun->ground
	};

	// ---- LARGE SCENE (finding: unusual scene scales) ----
	// Far clip 2000 m, a 1..400 m receiver slice, sun at ~40 deg
	// elevation. The slice's own extent ALONG THE LIGHT dominates the
	// envelope (the padding ADDS 25 m on top of it): a caster at
	// sunwardHalf + 0.9*padding sunward (just inside the light's eye
	// plane) stays in; 1.5x padding past the eye plane (closer to the
	// sun than the light) clips out.
	{
		const glm::vec3 lightDir = glm::normalize(
			glm::vec3(-0.5f, -0.766f, -0.4f)); // ~40 deg elevation
		const glm::vec3 fwd = glm::normalize(glm::vec3(0.0f, -0.05f, -1.0f));
		const glm::vec3 camPos(50.0f, 25.0f, 300.0f);
		const Fit f = fitLight(lightDir, camPos, fwd, 1.0f, 400.0f, 2000.0f);
		const bool justInside = casterDepthInside(f,
			f.sliceCenter + sunward(lightDir)
			* (f.sunwardHalf + 0.9f * kCascadeZPadding));
		const bool pastLight = casterDepthInside(f,
			f.sliceCenter + sunward(lightDir)
			* (f.sunwardHalf + 1.5f * kCascadeZPadding));
		Check(justInside, "light.padding.large-scene.envelope-edge-in");
		Check(!pastLight, "light.padding.large-scene.past-light-clips");
	}

	// ---- LOW SUN (finding: low-angle lighting) ----
	// Elevation 2 deg: ground-plane casters of height H stand
	// t = H / sin(2 deg) ~ 28.6*H sunward of the receiver. The covered
	// caster height is (sunwardHalf + padding) * sin(e) — the FIXED
	// padding only lifts that by 25*sin(2 deg) ~ 0.87 m of caster height
	// (the documented low-sun shrinkage: if the OUT-side assertion ever
	// flips, the padding widened — make that change deliberately for
	// low-sun scenes, not by accident).
	{
		const float el = glm::radians(2.0f);
		const glm::vec3 lightDir = glm::normalize(
			glm::vec3(-0.64f, -std::sin(el), -0.77f));
		const glm::vec3 fwd = glm::normalize(glm::vec3(0.1f, -0.05f, -1.0f));
		const glm::vec3 camPos(10.0f, 3.0f, 60.0f);
		const Fit f = fitLight(lightDir, camPos, fwd, 1.0f, 120.0f, 800.0f);
		auto groundCaster = [&](float height) {
			// On the light ray through the slice center: t = H / sin(el).
			const float t = height / std::sin(el);
			return f.sliceCenter + sunward(lightDir) * t;
		};
		// The max covered caster height per the envelope formula.
		const float hMax = (f.sunwardHalf + kCascadeZPadding) * std::sin(el);
		Check(casterDepthInside(f, groundCaster(0.8f)),
			"light.padding.low-sun.0.8m-caster-in");
		Check(casterDepthInside(f, groundCaster(hMax * 0.9f)),
			"light.padding.low-sun.envelope-edge-in");
		Check(!casterDepthInside(f, groundCaster(hMax * 1.3f)),
			"light.padding.low-sun.beyond-envelope-clips (documented:"
			" widen the padding deliberately for low-sun scenes)");
	}

	// ---- Slice coverage at the large scale (the base contract) ----
		{
			const glm::vec3 lightDir = glm::normalize(
				glm::vec3(-0.5f, -0.766f, -0.4f));
			const glm::vec3 fwd = glm::normalize(glm::vec3(0.0f, -0.05f, -1.0f));
			const glm::vec3 camPos(50.0f, 25.0f, 300.0f);
			const glm::mat4 proj = MakeReverseZProj(
				glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 2000.0f);
			const glm::mat4 invProj = glm::inverse(proj);
			const glm::mat4 view = glm::lookAt(camPos, camPos + fwd,
				glm::vec3(0.0f, 1.0f, 0.0f));
			const glm::mat4 invView = glm::inverse(view);
			glm::mat4 lv, lp;
			smvk::GetDirectionalLightOrthoVP(lightDir, invView, invProj,
				1.0f, 400.0f, lv, lp, kCascadeZPadding, true, kRes);
			const glm::mat4 vp = lp * lv;
			bool covered = true;
			for (const glm::vec3& c :
					SliceCorners(invProj, invView, 1.0f, 400.0f)) {
				const glm::vec4 clip = vp * glm::vec4(c, 1.0f);
				const glm::vec3 ndc = glm::vec3(clip) / clip.w;
				if (ndc.x < -1.0001f || ndc.x > 1.0001f ||
					ndc.y < -1.0001f || ndc.y > 1.0001f ||
					ndc.z < -0.0001f || ndc.z > 1.0001f)
					covered = false;
			}
			Check(covered, "light.padding.large-scene.slice-covered");
	}
}

// ---------------------------------------------------------------------------
// END-TO-END shadow coverage consistency (the P2): a caster RETAINED by
// the contribution cull (MakeCasterContributionView) must land INSIDE the
// cascade's depth range (GetDirectionalLightOrthoVP with the caster fit)
// — the two systems must agree. The slice-only fit clipped far-sun
// casters at the light's near plane: a tall building up-sun passed the
// cull and its shadow STILL popped. The caster-bounds fit extends the
// depth coverage to the retained casters' light-axis extent, clamped by
// kDirectionalShadowMaxDistance (the explicit pop bound — a caster past
// it clips by decision, not by luck).
// ---------------------------------------------------------------------------
void TestCasterFitEndToEnd() {
	smvk::RenderViewSet views;
	views.NewFrame();
	// Main camera at the origin looking -Z, fov 90, finite reversed-Z far
	// (the fit's receiver slice lives in [shadowNear, shadowFar] along the
	// view axis).
	glm::mat4 proj = MakeReverseZProj(glm::radians(90.0f), 1.0f, 0.1f, 500.0f);
	proj[1][1] *= -1.0f; // Vulkan Y-flip (the engine's main view convention)
	const glm::mat4 view =
		glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f),
			glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::mat4 invView = glm::inverse(view);
	const glm::mat4 invProj = glm::inverse(proj);
	const uint32_t rv = views.AllocateMainView(proj * view, { 800.0f, 600.0f });
	const smvk::RenderView& mainView = views.Get(rv);

	// Oblique sun (~45 deg elevation, up-sun at +x): lightDir points
	// sun->ground.
	const glm::vec3 lightDir =
		glm::normalize(glm::vec3(-0.5f, -0.6f, -0.2f));
	const smvk::RenderView swept =
		smvk::MakeCasterContributionView(mainView, lightDir);

	// The receiver slice the cascade fits.
	constexpr float kShadowNear = 10.0f;
	constexpr float kShadowFar = 80.0f;
	constexpr float kCascadeZPadding = 25.0f; // WorldRenderer's constant
	// The slice center + its sunward half-extent along the light axis
	// (the old coverage bound was sunwardHalf + padding).
	glm::vec3 sliceCenter(0.0f);
	float sunwardHalf = 0.0f;
	{
		const auto corners = SliceCorners(invProj, invView,
			kShadowNear, kShadowFar);
		for (const glm::vec3& c : corners)
			sliceCenter += c;
		sliceCenter /= float(corners.size());
		float minA = FLT_MAX;
		for (const glm::vec3& c : corners)
			minA = (std::min)(minA, glm::dot(c - sliceCenter, lightDir));
		sunwardHalf = -minA;
	}

	// The caster candidates: a 120 m TALL TOWER whose top stands WELL past
	// the old coverage (sunward ray distance far beyond sunwardHalf + 25)
	// while its shadow still lands in the slice (the contribution view
	// keeps it — the precondition of the whole contract).
	smvk::ShadowCasterFitBounds casters;
	casters.min = glm::vec3(130.0f, 0.0f, -40.0f);
	casters.max = glm::vec3(150.0f, 120.0f, -20.0f);
	casters.valid = true;
	const glm::vec3 towerTop(casters.max);
	const glm::vec3 towerBase(casters.min.x, casters.min.y, casters.min.z);

	// 1. The contribution view RETAINS the tower (it can shadow the view —
	//    the culling system keeps it).
	const glm::vec3 towerCenter = (casters.min + casters.max) * 0.5f;
	const float towerRadius = glm::length(casters.max - casters.min) * 0.5f;
	Check(smvk::RenderViewIntersectsSphere(swept, towerCenter, towerRadius),
		"light.casterfit.contribution-retains-tower");

	// One fit + a depth probe.
	auto fitLight = [&](const smvk::ShadowCasterFitBounds* bounds) {
		glm::mat4 lv, lp;
		smvk::GetDirectionalLightOrthoVP(lightDir, invView, invProj,
			kShadowNear, kShadowFar, lv, lp, kCascadeZPadding, true, 2048,
			nullptr, bounds);
		return lp * lv;
	};
	auto depthInside = [](const glm::mat4& vp, const glm::vec3& p) {
		const glm::vec4 clip = vp * glm::vec4(p, 1.0f);
		const glm::vec3 ndc = glm::vec3(clip) / clip.w;
		return ndc.z >= -0.001f && ndc.z <= 1.001f;
	};

	// 2. The DOCUMENTED GAP (the reason the fit exists): the slice-only
	//    fit retains nothing beyond sunwardHalf + padding on the depth
	//    axis — the tower passes the cull and still clips (its shadow
	//    pops). Asserts the tower is genuinely past the old bound.
	const glm::mat4 vpSliceOnly = fitLight(nullptr);
	Check(!depthInside(vpSliceOnly, towerTop),
		"light.casterfit.slice-only-fit-clips-tower (the P2 gap)");

	// 3. THE CONTRACT: with the caster bounds, a contribution-retained
	//    caster is DEPTH-COVERED — both the tower top and base land inside
	//    the light's depth range. The culling volume and the depth fit now
	//    agree.
	const glm::mat4 vpFitted = fitLight(&casters);
	Check(depthInside(vpFitted, towerTop),
		"light.casterfit.retained-tower-top-covered");
	Check(depthInside(vpFitted, towerBase),
		"light.casterfit.retained-tower-base-covered");

	// 4. The receiver side is untouched: every slice corner stays inside
	//    the fitted frustum (the caster extension only widens DEPTH —
	//    never narrows the receiver coverage or the xy window).
	bool sliceCovered = true;
	for (const glm::vec3& c :
			SliceCorners(invProj, invView, kShadowNear, kShadowFar)) {
		const glm::vec4 clip = vpFitted * glm::vec4(c, 1.0f);
		const glm::vec3 ndc = glm::vec3(clip) / clip.w;
		if (ndc.x < -1.0001f || ndc.x > 1.0001f ||
			ndc.y < -1.0001f || ndc.y > 1.0001f ||
			ndc.z < -0.0001f || ndc.z > 1.0001f)
			sliceCovered = false;
	}
	Check(sliceCovered, "light.casterfit.slice-still-covered");

	// 5. The EXPLICIT clamp: a caster beyond sunwardHalf +
	//    kDirectionalShadowMaxDistance clips by DECISION (the documented
	//    pop bound — bounded, not lucky). Place the far tower at 1.5x the
	//    cap sunward of the slice center along the light axis.
	{
		const float farT = sunwardHalf
			+ 1.5f * smvk::kDirectionalShadowMaxDistance;
		const glm::vec3 farTop = sliceCenter - lightDir * farT
			+ glm::vec3(0.0f, 120.0f, 0.0f);
		smvk::ShadowCasterFitBounds farCasters;
		farCasters.min = sliceCenter - lightDir * (farT + 20.0f)
			- glm::vec3(10.0f, 0.0f, 10.0f);
		farCasters.max = sliceCenter - lightDir * (farT - 0.0f)
			+ glm::vec3(10.0f, 120.0f, 10.0f);
		farCasters.valid = true;
		const glm::mat4 vpFar = fitLight(&farCasters);
		Check(!depthInside(vpFar, farTop),
			"light.casterfit.past-max-distance-clips (the explicit bound)");
		// The near slice still covers with the far clamped bounds (the
		// clamp never sacrifices the receiver side).
		bool nearCovered = depthInside(vpFar,
			sliceCenter + glm::vec3(0.0f, 0.0f, -30.0f));
		Check(nearCovered, "light.casterfit.clamped-fit-keeps-receivers");
	}
}

} // namespace

int main() {
	TestDirectionalTexelSnap();
	TestCascadePaddingLargeSceneLowSun();
	TestCasterFitEndToEnd();
	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
