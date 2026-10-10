// exposure_tests: headless unit tests for the auto-exposure CPU math
// (histogram percentile + damped eye adaptation, see
// Engine/Rendergraph/exposure.h). No GPU required: pure math functions
// tested directly. Run via CTest.
//
// Hand-rolled runner (no test framework dependency): each check prints
// PASS/FAIL, the process exits nonzero on any failure.
//
// NOTE (drift risk): this file DUPLICATES ExposureMeter::Percentile-
// Luminance/Adapt instead of including exposure.h (which pulls the whole
// rendergraph). Any change to the real math (e.g. the (bins-1) bin-center
// inversion mirroring the shader's t*(bins-1) binning) must be mirrored
// here by hand — the bin-center expectations below pin that convention.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <array>

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

bool Near(float a, float b, float eps = 1e-5f) {
	return std::fabs(a - b) <= eps * std::max(1.0f, std::fabs(b));
}

constexpr uint32_t kExposureBinCount = 64;

struct ExposureDesc {
	uint32_t bins = kExposureBinCount;
	float minLogLum = -10.0f;
	float maxLogLum = 12.0f;
	float keyValue = 0.18f;
	float percentile = 0.6f;
};

struct ExposureMeter {
	std::array<uint32_t, kExposureBinCount> lastBins{};
	uint32_t lastTotal = 0;
	ExposureDesc desc{};

	float PercentileLuminance() const {
		if (lastTotal == 0)
			return 0.0f;
		const uint32_t bins = desc.bins == 0 ? kExposureBinCount : desc.bins;
		const uint64_t want = static_cast<uint64_t>(
			static_cast<double>(lastTotal) * static_cast<double>(desc.percentile));
		uint64_t accum = 0;
		for (uint32_t i = 0; i < bins; ++i) {
			accum += lastBins[i];
			if (accum >= want) {
				// Mirrors the shader (bin = saturate(t)*(bins-1)): bin i
				// center = (i+0.5)/(bins-1). Must match exposure.h exactly.
				const float t = bins > 1
					? (static_cast<float>(i) + 0.5f) / static_cast<float>(bins - 1)
					: 0.5f;
				const float logLum =
					desc.minLogLum + t * (desc.maxLogLum - desc.minLogLum);
				return exp2f(logLum);
			}
		}
		return exp2f(desc.maxLogLum);
	}

	float Adapt(float currentExposure, float evBias,
		float speedToDark, float speedToBright, float minExposure,
		float maxExposure, float dt) const {
		const float metered = PercentileLuminance();
		if (!(metered > 0.0f) || !(dt > 0.0f))
			return currentExposure;
		float target = desc.keyValue * exp2f(evBias) / metered;
		target = target < minExposure ? minExposure : target;
		target = target > maxExposure ? maxExposure : target;
		const float rate = target > currentExposure ? speedToDark : speedToBright;
		const float blend = 1.0f - expf(-rate * dt);
		return currentExposure + (target - currentExposure) * blend;
	}

	static uint32_t DispatchXFor(uint32_t width, uint32_t sampleStep) {
		const uint32_t tile = 16 * (sampleStep == 0 ? 4 : sampleStep);
		return (width + tile - 1) / tile;
	}
	static uint32_t DispatchYFor(uint32_t height, uint32_t sampleStep) {
		const uint32_t tile = 16 * (sampleStep == 0 ? 4 : sampleStep);
		return (height + tile - 1) / tile;
	}
};

ExposureMeter MakeMeter() {
	ExposureMeter m{};
	m.desc.bins = kExposureBinCount;
	m.desc.minLogLum = -10.0f;
	m.desc.maxLogLum = 12.0f;
	m.desc.keyValue = 0.18f;
	m.desc.percentile = 0.6f;
	return m;
}

void TestPercentileEmpty() {
	ExposureMeter m = MakeMeter();
	m.lastBins.fill(0);
	m.lastTotal = 0;
	Check(m.PercentileLuminance() == 0.0f, "exp.pct.empty");
}

void TestPercentileSingleBin() {
	ExposureMeter m = MakeMeter();
	m.lastBins.fill(0);
	m.lastBins[32] = 1000;
	m.lastTotal = 1000;
	const float lum = m.PercentileLuminance();
	const float want = exp2f(-10.0f + (32.5f / 63.0f) * 22.0f);
	Check(Near(lum, want, 1e-4f), "exp.pct.singlebin");
}

void TestPercentileSplit() {
	ExposureMeter m = MakeMeter();
	m.lastBins.fill(0);
	m.lastBins[0] = 500;
	m.lastBins[63] = 500;
	m.lastTotal = 1000;
	const float lum = m.PercentileLuminance();
	const float wantBright = exp2f(-10.0f + (63.5f / 63.0f) * 22.0f);
	const float wantDark = exp2f(-10.0f + (0.5f / 63.0f) * 22.0f);
	Check(Near(lum, wantBright, 1e-4f) && lum > wantDark * 100.0f, "exp.pct.split60");
	m.desc.percentile = 0.4f;
	Check(Near(m.PercentileLuminance(), wantDark, 1e-4f), "exp.pct.split40");
}

void TestAdaptHoldsWithoutMeasurement() {
	ExposureMeter m = MakeMeter();
	m.lastBins.fill(0);
	m.lastTotal = 0;
	Check(m.Adapt(0.001f, 0.0f, 1.0f, 3.0f, 1e-5f, 0.05f, 1.0f / 60.0f) == 0.001f,
		"exp.adapt.holdempty");
	m.lastBins[32] = 100;
	m.lastTotal = 100;
	Check(m.Adapt(0.001f, 0.0f, 1.0f, 3.0f, 1e-5f, 0.05f, 0.0f) == 0.001f,
		"exp.adapt.holdnodt");
}

void TestAdaptConverges() {
	ExposureMeter m = MakeMeter();
	m.lastBins.fill(0);
	m.lastBins[32] = 10000;
	m.lastTotal = 10000;
	const float lum = m.PercentileLuminance();
	const float target = 0.18f / lum;
	float e = 0.001f;
	Check(target > e, "exp.adapt.setup");
	for (int i = 0; i < 600; ++i)
		e = m.Adapt(e, 0.0f, 1.0f, 3.0f, 1e-5f, 0.05f, 1.0f / 60.0f);
	const float clamped = std::clamp(target, 1e-5f, 0.05f);
	Check(Near(e, clamped, 1e-2f), "exp.adapt.converge");
}

void TestAdaptRatesAndClamp() {
	ExposureMeter m = MakeMeter();
	m.lastBins.fill(0);
	m.lastBins[63] = 10000;
	m.lastTotal = 10000;
	const float dt = 1.0f / 60.0f;
	const float e0 = 0.05f;
	const float e1 = m.Adapt(e0, 0.0f, 1.0f, 3.0f, 1e-5f, 0.05f, dt);
	const float want = e0 + (1e-5f - e0) * (1.0f - std::exp(-3.0f * dt));
	Check(Near(e1, want, 1e-4f), "exp.adapt.fastrate");
	const float lum = m.PercentileLuminance();
	Check(Near(0.18f * 2.0f / lum,
		m.desc.keyValue * exp2f(1.0f) / lum, 1e-6f), "exp.adapt.evbias");
	m.lastBins.fill(0);
	m.lastTotal = 0;
	const float held = m.Adapt(e0, 0.0f, 1.0f, 3.0f, 1e-5f, 0.05f, dt);
	Check(held == e0 && std::isfinite(held), "exp.adapt.finite");
}

void TestDispatchDims() {
	Check(ExposureMeter::DispatchXFor(1920, 4) == (1920 + 63) / 64, "exp.dispatch.x");
	Check(ExposureMeter::DispatchYFor(1080, 4) == (1080 + 63) / 64, "exp.dispatch.y");
	Check(ExposureMeter::DispatchXFor(1920, 0) ==
		ExposureMeter::DispatchXFor(1920, 4), "exp.dispatch.step0");
}

} // namespace

int main() {
	TestPercentileEmpty();
	TestPercentileSingleBin();
	TestPercentileSplit();
	TestAdaptHoldsWithoutMeasurement();
	TestAdaptConverges();
	TestAdaptRatesAndClamp();
	TestDispatchDims();
	std::cout << g_checks - g_failures << "/" << g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}