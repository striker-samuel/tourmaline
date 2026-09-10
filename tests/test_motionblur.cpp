// motionblur_tests: CPU-side contract test for the depth-aware velocity
// gather (engine/shaders/motionblur.frag.hlsl).
//
// What it pins down:
//   1. Thin-foreground preservation: a 3px near bar under large velocity
//      keeps its color with background rejection, while the legacy
//      accept-all average erases it into the background (the reported
//      corner pop-in: "not visible but load next frame").
//   2. No over-rejection: far/background centers accept every tap exactly
//      like the legacy path (including sky depth 0), and nearer taps over
//      a background center are still accepted (bleed in the safe direction).
//   3. Interior blur unchanged: away from depth edges the new gather
//      bit-matches the legacy average.
//
// The port uses point taps (the shader bilinear-samples HDR and Loads
// depth texel-exact); the property under test is tap ACCEPTANCE, which is
// identical under either reconstruction.
//
// Hand-rolled runner (no test framework dependency).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

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

struct Image {
	uint32_t w = 0, h = 0;
	std::vector<float> px; // HDR red channel (scalar test scene)
	std::vector<float> depth; // reversed-Z depth (1 = near, 0 = far)
};

// Legacy gather: plain centered box average (pre-fix shader behavior).
float OldGather(const Image& img, float cx, float cy, float vx, float vy,
	uint32_t n, float maxRadius) {
	const float len = std::sqrt(vx * vx + vy * vy);
	if (len < 0.5f || n == 1 || maxRadius <= 0.0f)
		return img.px[size_t(cy) * img.w + size_t(cx)];
	const float maxLen = std::min(maxRadius, len);
	const float dx = vx / std::max(len, 1e-5f) * maxLen;
	const float dy = vy / std::max(len, 1e-5f) * maxLen;
	float acc = 0.0f;
	for (uint32_t i = 0; i < n; ++i) {
		const float t = float(i) / float(n - 1) - 0.5f;
		const int tx = std::clamp(int(cx + dx * t), 0, int(img.w) - 1);
		const int ty = std::clamp(int(cy + dy * t), 0, int(img.h) - 1);
		acc += img.px[size_t(ty) * img.w + size_t(tx)];
	}
	return acc / float(n);
}

// Fixed gather: background taps (farther than center, reversed-Z) rejected.
// Mirrors motionblur.frag.hlsl (depthTextureIndex valid path).
float NewGather(const Image& img, float cx, float cy, float vx, float vy,
	uint32_t n, float maxRadius) {
	const float len = std::sqrt(vx * vx + vy * vy);
	if (len < 0.5f || n == 1 || maxRadius <= 0.0f)
		return img.px[size_t(cy) * img.w + size_t(cx)];
	const float maxLen = std::min(maxRadius, len);
	const float dx = vx / std::max(len, 1e-5f) * maxLen;
	const float dy = vy / std::max(len, 1e-5f) * maxLen;
	const float dc = img.depth[size_t(cy) * img.w + size_t(cx)];
	const float tol = std::max(1e-6f, std::abs(dc) * 2e-3f);
	float acc = 0.0f, wsum = 0.0f;
	for (uint32_t i = 0; i < n; ++i) {
		const float t = float(i) / float(n - 1) - 0.5f;
		const int tx = std::clamp(int(cx + dx * t), 0, int(img.w) - 1);
		const int ty = std::clamp(int(cy + dy * t), 0, int(img.h) - 1);
		if (img.depth[size_t(ty) * img.w + size_t(tx)] < dc - tol)
			continue; // background tap over a nearer center: skip
		acc += img.px[size_t(ty) * img.w + size_t(tx)];
		wsum += 1.0f;
	}
	return acc / std::max(wsum, 1.0f);
}

void TestThinForegroundPreserved() {
	Image img;
	img.w = img.h = 64;
	img.px.assign(64 * 64, 0.02f);
	img.depth.assign(64 * 64, 0.05f);
	for (uint32_t y = 0; y < 64; ++y)
		for (uint32_t x = 31; x <= 33; ++x) {
			img.px[size_t(y) * 64 + x] = 2.0f;
			img.depth[size_t(y) * 64 + x] = 0.9f;
		}
	constexpr uint32_t kTaps = 12;
	constexpr float kRadius = 24.0f;
	// Fast horizontal motion: taps span +-4px around the bar pixel.
	const float oldV = OldGather(img, 32, 32, 8.0f, 0.0f, kTaps, kRadius);
	const float newV = NewGather(img, 32, 32, 8.0f, 0.0f, kTaps, kRadius);
#ifndef NDEBUG
	std::cout << "[motionblur] bar-center old=" << oldV << " new=" << newV << "\n";
#endif
	Check(oldV < 0.7f, "motionblur.legacy-erases-thin-bar");
	Check(newV > 1.5f, "motionblur.depthaware-preserves-thin-bar");
}

void TestBackgroundUnchanged() {
	Image img;
	img.w = img.h = 64;
	img.px.assign(64 * 64, 0.02f);
	img.depth.assign(64 * 64, 0.05f);
	for (uint32_t y = 0; y < 64; ++y)
		for (uint32_t x = 31; x <= 33; ++x) {
			img.px[size_t(y) * 64 + x] = 2.0f;
			img.depth[size_t(y) * 64 + x] = 0.9f;
		}
	constexpr uint32_t kTaps = 12;
	constexpr float kRadius = 24.0f;
	// Far from the bar (tap span +-4px never reaches it): identical.
	const float oldV = OldGather(img, 8, 32, 8.0f, 0.0f, kTaps, kRadius);
	const float newV = NewGather(img, 8, 32, 8.0f, 0.0f, kTaps, kRadius);
	Check(std::abs(oldV - 0.02f) < 1e-6f, "motionblur.legacy-bg-baseline");
	Check(std::abs(newV - oldV) < 1e-6f, "motionblur.depthaware-bg-identical");
	// Sky center (depth 0): every tap accepted, matches legacy.
	img.depth[size_t(32) * 64 + 8] = 0.0f;
	const float skyOld = OldGather(img, 8, 32, 8.0f, 0.0f, kTaps, kRadius);
	const float skyNew = NewGather(img, 8, 32, 8.0f, 0.0f, kTaps, kRadius);
	Check(std::abs(skyNew - skyOld) < 1e-6f, "motionblur.depthaware-sky-identical");
}

void TestBleedDirectionAsymmetric() {
	Image img;
	img.w = img.h = 64;
	img.px.assign(64 * 64, 0.02f);
	img.depth.assign(64 * 64, 0.05f);
	for (uint32_t y = 0; y < 64; ++y)
		for (uint32_t x = 0; x < 32; ++x) {
			img.px[size_t(y) * 64 + x] = 1.0f;
			img.depth[size_t(y) * 64 + x] = 0.8f;
		}
	constexpr uint32_t kTaps = 12;
	constexpr float kRadius = 24.0f;
	// Just inside the near half (x=30, taps span +-2px): far-half taps are
	// rejected, so the result stays near-half bright (legacy mixes dark).
	const float edgeNew = NewGather(img, 30, 32, 4.0f, 0.0f, kTaps, kRadius);
	const float edgeOld = OldGather(img, 30, 32, 4.0f, 0.0f, kTaps, kRadius);
	Check(edgeNew > 0.9f, "motionblur.depthaware-edge-preserved");
	Check(edgeOld < edgeNew, "motionblur.legacy-edge-diluted");
	// Just inside the far half (x=34): nearer taps are still accepted
	// (safe-direction bleed), matching the legacy average exactly.
	const float bgNew = NewGather(img, 34, 32, 4.0f, 0.0f, kTaps, kRadius);
	const float bgOld = OldGather(img, 34, 32, 4.0f, 0.0f, kTaps, kRadius);
	Check(std::abs(bgNew - bgOld) < 1e-6f, "motionblur.depthaware-far-side-identical");
}

} // namespace

int main() {
	TestThinForegroundPreserved();
	TestBackgroundUnchanged();
	TestBleedDirectionAsymmetric();
	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
