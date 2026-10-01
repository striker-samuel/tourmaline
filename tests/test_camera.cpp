// camera_tests: CPU-side regression tests for the finite reversed-Z main
// view projection + the shared degenerate-lookAt resolution
// (Engine/Camera/camera.h). No GPU required: pure matrix math.
//
// What it pins down (mirrors the app config: 45° fovY, 0.01 near,
// 100000 far):
//   1. depth mapping: near plane -> 1, far plane -> 0 (reversed-Z).
//   2. agreement with the infinite form inside [near, far] (the switch
//      must be visually silent: ~1e-7 relative by construction).
//   3. beyond-far cullability: past-far geometry lands on the NDC z<0
//      side; in-front-of-near lands past 1 (frustum planes functional,
//      unlike the degenerate infinite rows).
//   4. SafeLookAt/ResolveLookAtUp (the SINGLE shared degenerate framing):
//      straight-down/up pick a deterministic horizontal basis, the
//      editor's truncated-pitch Top camera equals the exact straight-down
//      framing, and the positioner's quat path agrees with the mat4 one.

#include <algorithm>
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

// Max abs component difference of two 4x4 matrices.
float MatMaxDiff(const glm::mat4& a, const glm::mat4& b) {
	float m = 0.0f;
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			m = std::max(m, std::abs(a[c][r] - b[c][r]));
	return m;
}

void TestSafeLookAt() {
	// The SHARED degenerate-lookAt resolution (ResolveLookAtUp/SafeLookAt):
	// every camera framing that can look straight down/up resolves through
	// it, so all consumers of one (eye, target) pair agree on the screen
	// basis (the editor's overlay camera + the hosted drive paths picking
	// their own fallbacks rotated the editor's wire boxes 90 degrees
	// against the scene in the straight-down top view).
	const glm::vec3 upPref(0.0f, 1.0f, 0.0f);

	// Non-degenerate framing: SafeLookAt == plain lookAt with the
	// preferred up (byte-identical basis).
	const glm::vec3 eye(3.0f, 4.0f, 5.0f);
	const glm::vec3 tgt(0.0f, 1.0f, 0.0f);
	Check(MatMaxDiff(SafeLookAt(eye, tgt), glm::lookAt(eye, tgt, upPref))
			< 1e-6f, "camera.safelookat.nondeg-plain-agrees");

	// Straight DOWN (eye above target): right = +X, screen-up = -Z
	// ("north up"), deterministically. (View-matrix axes live in the ROWS
	// of the column-major mat4: row 0 = right, row 1 = screen-up.)
	{
		const glm::mat4 v = SafeLookAt(glm::vec3(0.0f, 25.0f, 0.0f),
			glm::vec3(0.0f), upPref);
		const glm::vec3 right(v[0][0], v[1][0], v[2][0]);
		const glm::vec3 up(v[0][1], v[1][1], v[2][1]);
		bool ok = glm::dot(right, glm::vec3(1, 0, 0)) > 0.999f
			&& glm::dot(up, glm::vec3(0, 0, -1)) > 0.999f;
		Check(ok, "camera.safelookat.straight-down-basis");
	}
	// Straight UP (eye below target): screen-up = +Z.
	{
		const glm::mat4 v = SafeLookAt(glm::vec3(0.0f, -25.0f, 0.0f),
			glm::vec3(0.0f), upPref);
		const glm::vec3 up(v[0][1], v[1][1], v[2][1]);
		Check(glm::dot(up, glm::vec3(0, 0, 1)) > 0.999f,
			"camera.safelookat.straight-up-basis");
	}
	// The editor's Top camera (pitch truncated near -pi/2, yaw 0) resolves
	// to the SAME basis as the exact straight-down framing — the old direct
	// lookAt built its basis from the pitch residue (right=+X/up=-Z) while
	// the drive paths' target nudge picked (right=+Z/up=+X): the 90-degree
	// rotation. Both must be identical now.
	{
		const float pitch = -1.5707963f; // SetupDefaultCameras' Top value
		const float yaw = 0.0f;
		const glm::vec3 topEye = glm::vec3(0.0f)
			+ 25.0f * glm::vec3(
				std::cos(pitch) * std::sin(yaw),
				-std::sin(pitch),
				std::cos(pitch) * std::cos(yaw));
		const glm::mat4 a = SafeLookAt(topEye, glm::vec3(0.0f), upPref);
		const glm::mat4 b = SafeLookAt(glm::vec3(0.0f, 25.0f, 0.0f),
			glm::vec3(0.0f), upPref);
		Check(MatMaxDiff(a, b) < 1e-4f,
			"camera.safelookat.topcam-equals-exact-straight-down");
	}
	// The hosted drive paths feed the resolved up into the POSITIONER's
	// lookAt (quat orientation -> mat4_cast view): it must agree with the
	// mat4 SafeLookAt for the same (eye, target, resolved up) — straight
	// down AND a normal framing.
	{
		CameraPositioner_FirstPerson p;
		const glm::vec3 eyes[2] = {
			glm::vec3(0.0f, 25.0f, 0.0f), // straight down
			glm::vec3(3.0f, 4.0f, 5.0f),  // normal
		};
		const glm::vec3 tgts[2] = {
			glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
		};
		bool okDown = true, okNorm = true;
		for (int i = 0; i < 2; ++i) {
			p.lookAt(eyes[i], tgts[i], ResolveLookAtUp(eyes[i], tgts[i]));
			const float diff = MatMaxDiff(p.getViewMatrix(),
				SafeLookAt(eyes[i], tgts[i]));
			if (i == 0)
				okDown = diff < 1e-4f;
			else
				okNorm = diff < 1e-4f;
		}
		Check(okDown, "camera.safelookat.positioner-agrees-straight-down");
		Check(okNorm, "camera.safelookat.positioner-agrees-normal");
	}
}

} // namespace

int main() {
	TestFiniteMapping();
	TestMatchesInfiniteInRange();
	TestSafeLookAt();
	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
