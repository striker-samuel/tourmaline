// streaming_tests: CPU-side tests for texture streaming (no GPU required).
//
// Covers the device-free policy math shared by the streamer and its future
// GPU-feedback consumer:
//   * TextureStreamer::EstimateResidentBytes (BC7 tail accounting)
//   * TextureStreamer::WantedMipForScreenCoverage / WantedMipForDistance
//     (v1 CPU feedback heuristic)
//   * TextureFeedback::kNoReport ABI sentinel
//
// Retarget/budget paths need a Vulkan device and are exercised through the
// app (ImGui stats), not here.
//
// Hand-rolled runner (no test framework dependency): each check prints
// PASS/FAIL, the process exits nonzero on any failure.

#include <cmath>
#include <cstdint>
#include <iostream>

#include <Engine/Renderer/texture_streamer.h>
#include <Engine/Renderer/texture_feedback.h>

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

void TestResidentBytes() {
	using smvk::TextureStreamer;
	// BC7: 16 bytes per 4x4 block.
	Check(TextureStreamer::EstimateResidentBytes(4, 4, 0, 1) == 16u,
		"resident: single 4x4 block is 16B");
	Check(TextureStreamer::EstimateResidentBytes(8, 8, 0, 1) == 64u,
		"resident: 8x8 is 4 blocks = 64B");
	// Non-multiple dims round up to whole blocks: 5x5 -> 2x2 blocks.
	Check(TextureStreamer::EstimateResidentBytes(5, 5, 0, 1) == 64u,
		"resident: 5x5 rounds up to 2x2 blocks");
	// Array layers multiply.
	Check(TextureStreamer::EstimateResidentBytes(4, 4, 0, 1, 6) == 96u,
		"resident: 6 layers multiply bytes");
	// Tail is dominated by its largest level: 256px tail << 4K full.
	const uint64_t full = TextureStreamer::EstimateResidentBytes(4096, 4096, 0, 13);
	const uint64_t tail = TextureStreamer::EstimateResidentBytes(4096, 4096, 4, 9);
	Check(full > 16ull * 1024 * 1024 && full < 24ull * 1024 * 1024,
		"resident: 4K BC7 full chain is ~21MB");
	Check(tail < full / 32,
		"resident: 256px-capped tail is <1/32 of full");
	// Full chain == sum of per-level estimates (partition check).
	uint64_t sum = 0;
	for (uint32_t m = 0; m < 13; ++m)
		sum += TextureStreamer::EstimateResidentBytes(4096, 4096, m, 1);
	Check(sum == full, "resident: chain equals sum of levels");
	// firstMip past the 1x1 level still counts the 1x1 block.
	Check(TextureStreamer::EstimateResidentBytes(1024, 1024, 10, 1) == 16u,
		"resident: 1x1 tail mip is one block");
}

void TestWantedMipCoverage() {
	using smvk::TextureStreamer;
	constexpr uint32_t kMips = 12; // 4K chain
	// 1:1 coverage wants full res.
	Check(TextureStreamer::WantedMipForScreenCoverage(kMips, 4096, 4096.0f, 0.0f) == 0u,
		"wanted: 1:1 coverage asks mip 0");
	// Quarter coverage (each axis /2... area /4 with same aspect) -> mip 2.
	Check(TextureStreamer::WantedMipForScreenCoverage(kMips, 4096, 1024.0f, 0.0f) == 2u,
		"wanted: 1024px coverage of 4K asks mip 2");
	// Sub-texel coverage clamps to the tail.
	Check(TextureStreamer::WantedMipForScreenCoverage(kMips, 4096, 0.5f, 0.0f) == kMips - 1,
		"wanted: tiny coverage clamps to tail mip");
	// Oversampling clamps to mip 0, never negative.
	Check(TextureStreamer::WantedMipForScreenCoverage(kMips, 256, 8192.0f, 0.0f) == 0u,
		"wanted: huge coverage clamps to mip 0");
	// Negative bias sharpens by one.
	Check(TextureStreamer::WantedMipForScreenCoverage(kMips, 4096, 1024.0f, -1.0f) == 1u,
		"wanted: -1 bias sharpens one mip");
	// Degenerate chain: single mip always wants 0.
	Check(TextureStreamer::WantedMipForScreenCoverage(1, 4, 1.0f, 0.0f) == 0u,
		"wanted: single-mip texture always asks mip 0");
}

void TestWantedMipDistance() {
	using smvk::TextureStreamer;
	constexpr uint32_t kMips = 12;
	constexpr float kH = 1080.0f;
	constexpr float kFov = 1.0471976f; // 60 deg
	// Monotonic: farther never asks sharper.
	uint32_t prev = 0;
	bool monotonic = true;
	for (float d = 0.5f; d < 200.0f; d *= 1.5f) {
		const uint32_t m = TextureStreamer::WantedMipForDistance(
			kMips, 4096, d, kH, kFov, 2.0f, 0.0f);
		if (m < prev) monotonic = false;
		prev = m;
	}
	Check(monotonic, "wanted: distance demand is monotonic");
	// Point blank with a 2 m object on a 1080p/60deg view: the 4K texture
	// covers ~810px -> mip ceil(log2(4096/810)) = 3.
	Check(TextureStreamer::WantedMipForDistance(kMips, 4096, 2.0f, kH, kFov, 2.0f, 0.0f) == 3u,
		"wanted: 2m object at 2m asks mip 3");
	// Far away decays to the tail.
	Check(TextureStreamer::WantedMipForDistance(kMips, 4096, 500.0f, kH, kFov, 2.0f, 0.0f) == kMips - 1,
		"wanted: distant object decays to tail");
	// Bigger world size sharpens (more texels on screen).
	const uint32_t small = TextureStreamer::WantedMipForDistance(
		kMips, 4096, 5.0f, kH, kFov, 1.0f, 0.0f);
	const uint32_t big = TextureStreamer::WantedMipForDistance(
		kMips, 4096, 5.0f, kH, kFov, 8.0f, 0.0f);
	Check(big < small, "wanted: larger world size asks sharper");
}

void TestFeedbackAbi() {
	Check(smvk::TextureFeedback::kNoReport == 0xFFFFFFFFu,
		"feedback: no-report sentinel is 0xFFFFFFFF");
}

} // namespace

int main() {
	TestResidentBytes();
	TestWantedMipCoverage();
	TestWantedMipDistance();
	TestFeedbackAbi();
	std::cout << g_checks - g_failures << "/" << g_checks
		<< " streaming checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
