// camera_tests: CPU-side regression tests for the finite reversed-Z main
// view projection (Engine/Camera/camera.cpp). No GPU required: pure
// matrix math.
//
// What it pins down (mirrors the app config: 45° fovY, 0.01 near,
// 100000 far):
//   1. depth mapping: near plane -> 1, far plane -> 0 (reversed-Z).
//   2. agreement with the infinite form inside [near, far] (the switch
//      must be visually silent: ~1e-7 relative by construction).
//   3. beyond-far cullability: past-far geometry lands on the NDC z<0
//      side; in-front-of-near lands past 1 (frustum planes functional,
//      unlike the degenerate infinite rows).

#include <cmath>
#include <iostream>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <Engine/Camera/camera.h>

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

// NDC z of an on-axis point at view distance d (camera looks -Z).
float NdcZ(const glm::mat4& proj, float dist) {
	const glm::vec4 clip = proj * glm::vec4(0.0f, 0.0f, -dist, 1.0f);
	return clip.z / clip.w;
}

void TestFiniteMapping() {
	constexpr float kFov = glm::radians(45.0f);
	constexpr float kAspect = 16.0f / 9.0f;
	constexpr float kNear = 0.01f;
	constexpr float kFar = 100000.0f;
	const glm::mat4 proj = MakeReversedZProjRH(kFov, kAspect, kNear, kFar);

	Check(std::abs(NdcZ(proj, kNear) - 1.0f) < 1e-6f, "camera.finite.near-maps-to-one");
	Check(std::abs(NdcZ(proj, kFar)) < 1e-6f, "camera.finite.far-maps-to-zero");
	// Monotonic decrease across the range (spot check ordered pair).
	Check(NdcZ(proj, 1.0f) > NdcZ(proj, 10.0f)
		&& NdcZ(proj, 10.0f) > NdcZ(proj, 1000.0f),
		"camera.finite.monotonic");
	// Beyond far -> NDC z<0 side (frustum-cullable); inside near -> past 1.
	Check(NdcZ(proj, kFar * 2.0f) < 0.0f, "camera.finite.beyond-far-negative");
	Check(NdcZ(proj, kNear * 0.5f) > 1.0f, "camera.finite.inside-near-above-one");
}

void TestMatchesInfiniteInRange() {
	constexpr float kFov = glm::radians(45.0f);
	constexpr float kAspect = 16.0f / 9.0f;
	constexpr float kNear = 0.01f;
	constexpr float kFar = 100000.0f;
	const glm::mat4 finite = MakeReversedZProjRH(kFov, kAspect, kNear, kFar);
	const glm::mat4 infinite = MakeInfReversedZProjRH(kFov, kAspect, kNear);

	// Inside [near, far] the finite form must agree with the infinite form
	// to ~1e-7 relative: switching projections is visually silent, only
	// beyond-far behavior changes (cullable vs clamped).
	const float dists[] = { 0.02f, 1.0f, 10.0f, 100.0f, 10000.0f };
	bool ok = true;
	for (float d : dists) {
		const float a = NdcZ(finite, d);
		const float b = NdcZ(infinite, d);
		if (!(std::abs(a - b) <= 1e-6f * std::max(1.0f, std::abs(b))))
			ok = false;
	}
	Check(ok, "camera.finite.matches-infinite-in-range");
}

} // namespace

int main() {
	TestFiniteMapping();
	TestMatchesInfiniteInRange();
	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
