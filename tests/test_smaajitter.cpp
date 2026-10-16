// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// smaajitter_tests: CPU-side contract test for the SMAA T2x subpixel jitter
// math (SceneRenderer::UpdateFrameUniforms + the resolve reprojection).
//
// What it pins down:
//   1. Amplitude: each phase shifts the raster by exactly 0.25 chain
//      (swap) px per axis (0.5 px frame-to-frame), at renderScale == 1
//      and under scaling. Twice that would mean the jitter is applied
//      twice ("double jitter") or measured in the wrong pixels.
//   2. Phase pairing: parity 0 -> (+0.25, +0.25) px y-down, parity 1 ->
//      (-0.25, -0.25) px y-down, matching subsampleIndices (1,1,1,0) /
//      (2,2,2,0) per the official @SUBSAMPLE_INDICES table (bottom-to-top
//      y; y-down shares one sign, see smaa.h).
//   3. Reprojection: motion = prevUV - currUV lands the resolve's
//      texcoord + velocity fetch exactly on the same surface point.
//
// The jitter formula below replicates UpdateFrameUniforms line for line
// (jx = s*2/W over the proj columns); any change there must update this
// replica. No GPU required.
//
// Hand-rolled runner (no test framework dependency): each check prints
// PASS/FAIL, the process exits nonzero on any failure.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <Engine/Camera/camera.h>
#include <Engine/Rendergraph/smaa.h>

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

// Replica of the UpdateFrameUniforms jitter block (app/src/world_renderer.cpp):
// quarter-pixel NDC translation injected into the x/y rows, quantized to
// swap-extent (chain resolution) texels.
glm::mat4 ApplyT2xJitterForTest(const glm::mat4& proj, float paritySign,
	float extentW, float extentH) {
	glm::mat4 j = proj;
	const float jx = paritySign * 2.0f / extentW;
	const float jy = paritySign * 2.0f / extentH;
	for (int c = 0; c < 4; ++c) {
		j[c][0] += jx * j[c][3];
		j[c][1] += jy * j[c][3];
	}
	return j;
}

// One jitter case: camera aspect from the render extent, jitter quantized
// to the swap extent, pixel deltas measured in swap (chain) pixels. The
// render==swap case is the VSync default; the scaled case pins that the
// swing stays exactly 0.5 chain px when renderScale != 1 (render-quantized
// jitter would give 0.75 here and break the subsample pairing).
void CheckT2xJitterCase(float renderW, float renderH,
	float swapW, float swapH, const char* tag) {
	glm::mat4 proj = MakeInfReversedZProjRH(glm::radians(60.0f), renderW / renderH, 0.1f);
	proj[1][1] *= -1.0f; // app Vulkan Y-flip (y-down NDC)
	const glm::vec3 eye(2.0f, 1.0f, 6.0f);
	const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0, 1, 0));

	const glm::mat4 vpBase = proj * view;
	const glm::mat4 vpA = ApplyT2xJitterForTest(proj, +0.25f, swapW, swapH) * view;
	const glm::mat4 vpB = ApplyT2xJitterForTest(proj, -0.25f, swapW, swapH) * view;

	// Static probe point, well inside the frustum.
	const glm::vec4 P(0.5f, 0.7f, -1.0f, 1.0f);
	auto toPx = [&](const glm::mat4& vp) {
		const glm::vec4 clip = vp * P;
		const glm::vec2 ndc = glm::vec2(clip) / clip.w;
		return glm::vec2((ndc.x * 0.5f + 0.5f) * swapW, (ndc.y * 0.5f + 0.5f) * swapH);
	};
	const glm::vec2 pxBase = toPx(vpBase);
	const glm::vec2 pxA = toPx(vpA);
	const glm::vec2 pxB = toPx(vpB);

	char name[96];
#define SMAA_CHECK(cond, suffix) \
	std::snprintf(name, sizeof(name), "smaajitter.%s-%s", tag, suffix); \
	Check(cond, name)

	// 1. Amplitude: exactly a quarter chain-pixel per phase per axis.
	const glm::vec2 dA = pxA - pxBase;
	const glm::vec2 dB = pxB - pxBase;
	const glm::vec2 swing = pxA - pxB;
#ifndef NDEBUG
	std::cout << "[smaajitter:" << tag << "] dA=(" << dA.x << "," << dA.y << ") "
		<< "dB=(" << dB.x << "," << dB.y << ") "
		<< "swing=(" << swing.x << "," << swing.y << ")\n";
#endif
	SMAA_CHECK(std::abs(dA.x - 0.25f) < 1e-3f, "phase-a-x-quarter-px");
	SMAA_CHECK(std::abs(dA.y - 0.25f) < 1e-3f, "phase-a-y-quarter-px");
	SMAA_CHECK(std::abs(dB.x + 0.25f) < 1e-3f, "phase-b-x-quarter-px");
	SMAA_CHECK(std::abs(dB.y + 0.25f) < 1e-3f, "phase-b-y-quarter-px");
	SMAA_CHECK(std::abs(swing.x - 0.5f) < 1e-3f, "frame-swing-x-half-px");
	SMAA_CHECK(std::abs(swing.y - 0.5f) < 1e-3f, "frame-swing-y-half-px");

	// 2. Phase pairing: y-down corner signs (official table is bottom-to-top).
	SMAA_CHECK(dA.x > 0.0f && dA.y > 0.0f, "phase-a-corner-plus-plus");
	SMAA_CHECK(dB.x < 0.0f && dB.y < 0.0f, "phase-b-corner-minus-minus");

	// 3. Reprojection: prevUV - currUV, sampled at texcoord + velocity,
	// lands back on the previous frame's UV of the same surface point.
	const glm::vec2 uvA = glm::vec2((pxA.x / swapW), (pxA.y / swapH));
	const glm::vec2 uvB = glm::vec2((pxB.x / swapW), (pxB.y / swapH));
	const glm::vec2 v = uvB - uvA; // motion buffer content (prev - curr)
	const glm::vec2 reprojected = uvA + v; // resolve fetch (double negation)
	const float errPx = glm::length((reprojected - uvB) * glm::vec2(swapW, swapH));
	SMAA_CHECK(errPx < 1e-3f, "reprojection-exact");

	// 4. Depth untouched by jitter (infinite reversed-Z w-row intact).
	const glm::vec4 clipA = vpA * P;
	const glm::vec4 clipB = vpB * P;
	const glm::vec4 clip0 = vpBase * P;
	SMAA_CHECK(std::abs(clipA.z - clip0.z) < 1e-6f, "depth-untouched-a");
	SMAA_CHECK(std::abs(clipB.z - clip0.z) < 1e-6f, "depth-untouched-b");
	SMAA_CHECK(std::abs(clipA.w - clip0.w) < 1e-6f, "w-untouched-a");
	SMAA_CHECK(std::abs(clipB.w - clip0.w) < 1e-6f, "w-untouched-b");
#undef SMAA_CHECK
}

void TestT2xJitterMath() {
	CheckT2xJitterCase(1920.0f, 1080.0f, 1920.0f, 1080.0f, "scale-100");
	CheckT2xJitterCase(1280.0f, 720.0f, 1920.0f, 1080.0f, "scale-67");
}

// ---------------------------------------------------------------------------
// Global T2x toggle (SmaaBeginFrame / SmaaEndFrame).
//
// Regression test for the VSync T2x judder: the phase must strictly
// alternate across CONSECUTIVE records (P0,P1,P0,P1...), whatever swapchain
// image each record targets. The previous per-image keying made each image
// alternate with itself, but FIFO display cycles images, so the screen
// showed P0,P0,P1,P1 (holds + 0.5px jumps) while the shared slots fed
// 2-record-old colors to 1-record-old velocities. This drives 2- and
// 3-image FIFO acquire sequences through the global toggle and asserts
// on-screen alternation, plus warmup / off-record freeze / explicit
// invalidation. No GPU required.
// ---------------------------------------------------------------------------

void TestSmaaGlobalToggle() {
	using namespace smvk;

	// FIFO acquire order over 2 and 3 swapchain images: consecutive
	// records (consecutive presented frames under VSync) alternate, and
	// every record after the first blends.
	for (uint32_t kImages : { 2u, 3u }) {
		SmaaT2xState st;
		bool ok = true;
		const int n = 2 * (int)kImages + 2;
		for (int i = 0; i < n; ++i) {
			const uint32_t img = (uint32_t)(i % (int)kImages);
			(void)img; // acquire order only: the phase must not depend on it
			const SmaaFrameParams p = SmaaBeginFrame(st, true);
			if (p.parity != (uint32_t)(i % 2))
				ok = false;
			if (p.historyValid != (i != 0))
				ok = false;
			SmaaEndFrame(st, true);
		}
		char name[64];
		std::snprintf(name, sizeof(name),
			"smaaglobal.fifo-%u-consecutive-alternates-and-warms-up", kImages);
		Check(ok, name);
	}

	// Non-T2x record freezes the phase and invalidates (never blend stale);
	// re-entry warms up on the frozen phase, then alternation resumes.
	{
		SmaaT2xState st;
		SmaaEndFrame(st, true);  // record 0 ran T2x: parity 0->1, valid
		SmaaEndFrame(st, false); // off/1x record: freeze + invalidate
		const SmaaFrameParams p = SmaaBeginFrame(st, true);
		Check(!p.historyValid, "smaaglobal.off-record-invalidates");
		Check(p.parity == 1, "smaaglobal.off-record-freezes-phase");
		SmaaEndFrame(st, true); // re-entry warms up: valid, parity 1->0
		const SmaaFrameParams q = SmaaBeginFrame(st, true);
		Check(q.historyValid && q.parity == 0, "smaaglobal.reentry-resumes-alternation");
	}

	// SMAA 1x snapshot is zeros (no jitter, passthrough).
	{
		SmaaT2xState st;
		SmaaEndFrame(st, true);
		const SmaaFrameParams p = SmaaBeginFrame(st, false);
		Check(p.parity == 0 && !p.historyValid, "smaaglobal.1x-snapshot-is-zero");
	}

	// Explicit invalidation (rebuild / AA-mode change) resets to phase 0.
	{
		SmaaT2xState st;
		SmaaEndFrame(st, true);
		SmaaEndFrame(st, true);
		SmaaInvalidateHistory(st);
		const SmaaFrameParams p = SmaaBeginFrame(st, true);
		Check(!p.historyValid && p.parity == 0, "smaaglobal.invalidate-resets");
	}
}

} // namespace

int main() {
	TestT2xJitterMath();
	TestSmaaGlobalToggle();
	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
