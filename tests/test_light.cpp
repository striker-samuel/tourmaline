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
#include <cmath>
#include <iostream>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <Engine/Renderer/light.h>

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

} // namespace

int main() {
	TestDirectionalTexelSnap();
	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
