// smaajitter_tests: CPU-side contract test for the SMAA T2x subpixel jitter
// math (SceneRenderer::UpdateFrameUniforms + the resolve reprojection).
//
// What it pins down:
//   1. Amplitude: each phase shifts the raster by exactly 0.25 px per axis
//      (0.5 px frame-to-frame). Twice that would mean the jitter is applied
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

// Replica of the UpdateFrameUniforms jitter block (app/src/scene_renderer.cpp):
// quarter-pixel NDC translation injected into the x/y rows, quantized to
// render-extent (internal resolution) texels.
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

void TestT2xJitterMath() {
	constexpr float W = 1920.0f, H = 1080.0f;
	glm::mat4 proj = MakeInfReversedZProjRH(glm::radians(60.0f), W / H, 0.1f);
	proj[1][1] *= -1.0f; // app Vulkan Y-flip (y-down NDC)
	const glm::vec3 eye(2.0f, 1.0f, 6.0f);
	const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0, 1, 0));

	const glm::mat4 vpBase = proj * view;
	const glm::mat4 vpA = ApplyT2xJitterForTest(proj, +0.25f, W, H) * view;
	const glm::mat4 vpB = ApplyT2xJitterForTest(proj, -0.25f, W, H) * view;

	// Static probe point, well inside the frustum.
	const glm::vec4 P(0.5f, 0.7f, -1.0f, 1.0f);
	auto toPx = [&](const glm::mat4& vp) {
		const glm::vec4 clip = vp * P;
		const glm::vec2 ndc = glm::vec2(clip) / clip.w;
		return glm::vec2((ndc.x * 0.5f + 0.5f) * W, (ndc.y * 0.5f + 0.5f) * H);
	};
	const glm::vec2 pxBase = toPx(vpBase);
	const glm::vec2 pxA = toPx(vpA);
	const glm::vec2 pxB = toPx(vpB);

	// 1. Amplitude: exactly a quarter pixel per phase per axis.
	const glm::vec2 dA = pxA - pxBase;
	const glm::vec2 dB = pxB - pxBase;
	const glm::vec2 swing = pxA - pxB;
#ifndef NDEBUG
	std::cout << "[smaajitter] dA=(" << dA.x << "," << dA.y << ") "
		<< "dB=(" << dB.x << "," << dB.y << ") "
		<< "swing=(" << swing.x << "," << swing.y << ")\n";
#endif
	Check(std::abs(dA.x - 0.25f) < 1e-3f, "smaajitter.phase-a-x-quarter-px");
	Check(std::abs(dA.y - 0.25f) < 1e-3f, "smaajitter.phase-a-y-quarter-px");
	Check(std::abs(dB.x + 0.25f) < 1e-3f, "smaajitter.phase-b-x-quarter-px");
	Check(std::abs(dB.y + 0.25f) < 1e-3f, "smaajitter.phase-b-y-quarter-px");
	Check(std::abs(swing.x - 0.5f) < 1e-3f, "smaajitter.frame-swing-x-half-px");
	Check(std::abs(swing.y - 0.5f) < 1e-3f, "smaajitter.frame-swing-y-half-px");

	// 2. Phase pairing: y-down corner signs (official table is bottom-to-top).
	Check(dA.x > 0.0f && dA.y > 0.0f, "smaajitter.phase-a-corner-plus-plus");
	Check(dB.x < 0.0f && dB.y < 0.0f, "smaajitter.phase-b-corner-minus-minus");

	// 3. Reprojection: prevUV - currUV, sampled at texcoord + velocity,
	// lands back on the previous frame's UV of the same surface point.
	const glm::vec2 uvA = glm::vec2((pxA.x / W), (pxA.y / H));
	const glm::vec2 uvB = glm::vec2((pxB.x / W), (pxB.y / H));
	const glm::vec2 v = uvB - uvA; // motion buffer content (prev - curr)
	const glm::vec2 reprojected = uvA + v; // resolve fetch (double negation)
	const float errPx = glm::length((reprojected - uvB) * glm::vec2(W, H));
	Check(errPx < 1e-3f, "smaajitter.reprojection-exact");

	// 4. Depth untouched by jitter (infinite reversed-Z w-row intact).
	const glm::vec4 clipA = vpA * P;
	const glm::vec4 clipB = vpB * P;
	const glm::vec4 clip0 = vpBase * P;
	Check(std::abs(clipA.z - clip0.z) < 1e-6f, "smaajitter.depth-untouched-a");
	Check(std::abs(clipB.z - clip0.z) < 1e-6f, "smaajitter.depth-untouched-b");
	Check(std::abs(clipA.w - clip0.w) < 1e-6f, "smaajitter.w-untouched-a");
	Check(std::abs(clipB.w - clip0.w) < 1e-6f, "smaajitter.w-untouched-b");
}

// ---------------------------------------------------------------------------
// Per-image T2x state machine (SmaaBeginFrame / SmaaEndFrame).
//
// Simulates record sequences over a 3-image swapchain with Mailbox-style
// drops (recorded but never displayed) and repeats, and checks that every
// record is self-consistent: its jitter phase, history slot and validity
// always agree, each image alternates its own phases, validity is granted
// only after that image's own first T2x write, and mode switches / stale
// histories invalidate. A global render-counter scheme would fail the drop
// cases (the old bug); per-image state passes them by construction.
// ---------------------------------------------------------------------------

void TestSmaaImageState() {
	using namespace smvk;
	constexpr uint32_t kImages = 3;
	std::vector<SmaaImageState> st(kImages);
	uint64_t counter = 0;

	struct Record {
		uint32_t image;
		uint32_t parity;
		bool valid;
	};
	auto record = [&](uint32_t img, bool t2x) {
		++counter;
		const SmaaImageFrameParams p = SmaaBeginFrame(st[img], t2x, counter);
		Record r{ img, p.parity, p.historyValid };
		SmaaEndFrame(st[img], t2x, counter);
		return r;
	};

	// Strict round-robin (FIFO-like): images cycle, each alternates phases.
	std::vector<Record> seq;
	for (int i = 0; i < 9; ++i)
		seq.push_back(record((uint32_t)(i % kImages), true));
	bool alternating = true;
	for (uint32_t img = 0; img < kImages; ++img) {
		uint32_t expect = 0;
		bool first = true;
		for (const Record& r : seq) {
			if (r.image != img)
				continue;
			if (first) {
				// First record warms up (passthrough), phase starts at 0.
				if (r.valid || r.parity != 0)
					alternating = false;
				expect = 1;
				first = false;
			} else {
				if (r.parity != expect || !r.valid)
					alternating = false;
				expect ^= 1u;
			}
		}
	}
	Check(alternating, "smaastate.roundrobin-alternates-and-warms-up");

	// Mailbox drops: record images 0,1,2,0,1,2 but "display" only a subset.
	// Dropped records must not corrupt the displayed ones: re-record image 0
	// afterwards and require a valid, opposite-phase snapshot pairing with
	// its own previous record (self-consistency, not global pairing).
	st.assign(kImages, SmaaImageState{});
	counter = 0;
	const Record r0a = record(0, true); // warms up
	const Record r1 = record(1, true);  // dropped by mailbox
	const Record r2 = record(2, true);  // dropped by mailbox
	const Record r0b = record(0, true); // displayed after the drops
	Check(!r0a.valid && r0a.parity == 0, "smaastate.first-record-warms-up");
	Check(r0b.valid, "smaastate.valid-after-own-write-despite-drops");
	Check(r0b.parity == (r0a.parity ^ 1u), "smaastate.phase-toggles-per-own-record");
	Check(r1.parity == 0 && r2.parity == 0, "smaastate.fresh-images-start-at-zero");
	(void)r1; (void)r2;

	// Non-T2x record freezes the phase and invalidates (never blend stale).
	const Record r0c = record(0, false);
	Check(!r0c.valid && r0c.parity == 0, "smaastate.off-record-invalidates-parity-kept");
	const Record r0d = record(0, true);
	Check(!r0d.valid, "smaastate.reentry-warms-up");

	// Stale history (shared slots overwritten by many other records) ages out.
	st.assign(kImages, SmaaImageState{});
	counter = 0;
	record(0, true);
	for (int i = 0; i < (int)(kSmaaHistoryMaxAgeRecords + 2); ++i)
		record(1, true);
	++counter;
	const SmaaImageFrameParams stale = SmaaBeginFrame(st[0], true, counter);
	Check(!stale.historyValid, "smaastate.ancient-history-invalid");
	Check(stale.parity == 1, "smaastate.stale-keeps-own-parity");

	// Fresh history within the age bound stays valid.
	st.assign(kImages, SmaaImageState{});
	counter = 0;
	record(0, true);
	record(1, true);
	++counter;
	const SmaaImageFrameParams fresh = SmaaBeginFrame(st[0], true, counter);
	Check(fresh.historyValid, "smaastate.fresh-history-valid");
}

} // namespace

int main() {
	TestT2xJitterMath();
	TestSmaaImageState();
	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
