// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// RunConfigTests: central TOURMALINE_* parsing contract.
// Verifies defaults (clean env), typed parsing, and helper semantics.
// Does not depend on GPU. Each case saves/restores the vars it touches so
// ctest stays hermetic when run in one process list.

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <Engine/App/RunConfig.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	if (++g_checks, cond)
		std::cout << "PASS " << name << "\n";
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

#if defined(_WIN32)
void SetVar(const char* n, const char* v) { _putenv_s(n, v); }
void UnsetVar(const char* n) { _putenv_s(n, ""); }
#else
void SetVar(const char* n, const char* v) { setenv(n, v, 1); }
void UnsetVar(const char* n) { unsetenv(n); }
#endif

} // namespace

int main() {
	// Helpers preserve historical semantics exactly.
	Check(engine::EnvBoolAtoi("1") && engine::EnvBoolAtoi("7")
		&& !engine::EnvBoolAtoi("0") && !engine::EnvBoolAtoi(nullptr),
		"runconfig.helper-bool-atoi");
	Check(engine::EnvExactZeroKill("0") && !engine::EnvExactZeroKill("off")
		&& !engine::EnvExactZeroKill(nullptr),
		"runconfig.helper-exact-zero");
	float x = 0, y = 0, z = 0;
	Check(engine::ParseVec3("1.5,-2.0,3.25", x, y, z) && x == 1.5f && y == -2.0f
		&& z == 3.25f,
		"runconfig.helper-vec3");
	Check(!engine::ParseVec3("bogus", x, y, z), "runconfig.helper-vec3-reject");

	// Defaults with a clean slate for the touched vars.
	const char* kTouched[] = {"TOURMALINE_AA", "TOURMALINE_MESH", "TOURMALINE_CAPTURE",
		"TOURMALINE_CAM_YAW", "TOURMALINE_TERRAIN"};
	for (const char* n : kTouched)
		UnsetVar(n);
	{
		engine::RunConfig c = engine::RunConfig::Parse();
		Check(!c.aa.has_value(), "runconfig.default-aa-empty");
		Check(!c.meshSet && c.mesh, "runconfig.default-mesh-on");
		Check(c.capture < 0, "runconfig.default-capture-off");
		Check(!c.terrainPinned, "runconfig.default-terrain-unpinned");
	}

	// Validated pin parsing (authored scene wins invalid values).
	SetVar("TOURMALINE_AA", "msaa2x");
	SetVar("TOURMALINE_MESH", "0");
	SetVar("TOURMALINE_CAPTURE", "48");
	SetVar("TOURMALINE_CAM_YAW", "90.0");
	SetVar("TOURMALINE_TERRAIN", "1");
	SetVar("TOURMALINE_SSR", "off");
	SetVar("TOURMALINE_AO", "ssao");
	SetVar("TOURMALINE_VOLUMETRIC", "1");
	SetVar("TOURMALINE_VOLUMETRIC_STEPS", "64");
	SetVar("TOURMALINE_BLOOM_THRESHOLD", "400");
	SetVar("TOURMALINE_AUTO_EXPOSURE", "0");
	SetVar("TOURMALINE_CUBEMAP", "0");
	{
		engine::RunConfig c = engine::RunConfig::Parse();
		Check(c.aa.has_value() && *c.aa == engine::AaPin::Msaa2x,
			"runconfig.parse-aa");
		Check(c.meshSet && !c.mesh && c.meshDeviceKilled, "runconfig.parse-mesh-kill");
		Check(c.capture == 48, "runconfig.parse-capture");
		Check(c.camYawDeg.has_value() && *c.camYawDeg == 90.0f,
			"runconfig.parse-yaw");
		Check(c.terrainPinned && c.terrain, "runconfig.parse-terrain");
		Check(c.ssr.has_value() && *c.ssr == engine::OnOffPin::Off,
			"runconfig.parse-ssr-pin");
		Check(c.ao.has_value() && *c.ao == engine::AoPin::Ssao,
			"runconfig.parse-ao-pin");
		Check(c.volumetric.has_value() && *c.volumetric == engine::OnOffPin::On,
			"runconfig.parse-vol-pin");
		Check(c.volumetricSteps.has_value() && *c.volumetricSteps == 64,
			"runconfig.parse-vol-steps");
		Check(c.bloomThreshold.has_value() && *c.bloomThreshold == 400.0f,
			"runconfig.parse-bloom-thr");
		Check(c.autoExposure.has_value() && !*c.autoExposure,
			"runconfig.parse-autoexp-pin");
		Check(c.cubemap.has_value() && !*c.cubemap, "runconfig.parse-cubemap-kill");
	}
	for (const char* n : kTouched)
		UnsetVar(n);
	UnsetVar("TOURMALINE_SSR");
	UnsetVar("TOURMALINE_AO");
	UnsetVar("TOURMALINE_VOLUMETRIC");
	UnsetVar("TOURMALINE_VOLUMETRIC_STEPS");
	UnsetVar("TOURMALINE_BLOOM_THRESHOLD");
	UnsetVar("TOURMALINE_AUTO_EXPOSURE");
	UnsetVar("TOURMALINE_CUBEMAP");

	// Invalid pins warn (stderr) and leave the authored scene winning.
	SetVar("TOURMALINE_AA", "bogus");
	SetVar("TOURMALINE_SSR", "maybe");
	SetVar("TOURMALINE_AO", "xx");
	SetVar("TOURMALINE_VOLUMETRIC_STEPS", "7"); // below range... valid? 7 in 4..128!
	{
		engine::RunConfig c = engine::RunConfig::Parse();
		Check(!c.aa.has_value(), "runconfig.invalid-aa-unset");
		Check(!c.ssr.has_value(), "runconfig.invalid-ssr-unset");
		Check(!c.ao.has_value(), "runconfig.invalid-ao-unset");
		Check(c.volumetricSteps.has_value() && *c.volumetricSteps == 7,
			"runconfig.inrange-steps-set");
	}
	UnsetVar("TOURMALINE_AA");
	UnsetVar("TOURMALINE_SSR");
	UnsetVar("TOURMALINE_AO");
	SetVar("TOURMALINE_VOLUMETRIC_STEPS", "2"); // below 4: rejected
	SetVar("TOURMALINE_BLOOM_INTENSITY", "99"); // above 4: rejected
	{
		engine::RunConfig c = engine::RunConfig::Parse();
		Check(!c.volumetricSteps.has_value(), "runconfig.oorange-steps-unset");
		Check(!c.bloomIntensity.has_value(), "runconfig.oorange-bloom-unset");
	}
	UnsetVar("TOURMALINE_VOLUMETRIC_STEPS");
	UnsetVar("TOURMALINE_BLOOM_INTENSITY");
	// Legacy AA spellings still accepted (both call sites unified).
	SetVar("TOURMALINE_AA", "t2x");
	{
		engine::RunConfig c = engine::RunConfig::Parse();
		Check(c.aa.has_value() && *c.aa == engine::AaPin::SmaaT2x,
			"runconfig.parse-aa-legacy-t2x");
	}
	UnsetVar("TOURMALINE_AA");

	// Log + unknown-var warn never crash (output goes to stderr).
	{
		engine::RunConfig c = engine::RunConfig::Parse();
		c.Log();
		engine::RunConfig::WarnUnknown();
		Check(true, "runconfig.log-no-crash");
	}

	// ---- The three pin re-application regressions (the review batch) ----
	// (1) TOURMALINE_AMBIENT with trailing whitespace: the Parse snapshot
	// must TRIM so the re-application path (ApplyHeadlessPostfxOverrides,
	// which reads c.ambient/c.gi verbatim) sees the same value the startup
	// getenv site trims — "baked " must parse IDENTICALLY on both paths.
	SetVar("TOURMALINE_AMBIENT", "baked ");
	{
		engine::RunConfig c = engine::RunConfig::Parse();
		Check(c.ambient.has_value() && *c.ambient == "baked",
			"runconfig.ambient-trimmed (re-application parity)");
	}
	SetVar("TOURMALINE_AMBIENT", " baked");
	{
		engine::RunConfig c = engine::RunConfig::Parse();
		Check(c.ambient.has_value() && *c.ambient == "baked",
			"runconfig.ambient-trimmed-lead");
	}
	UnsetVar("TOURMALINE_AMBIENT");

	// (2) TOURMALINE_GI_VOLUMES=0 sets the kill pin (the re-application
	// path must re-apply it AFTER the render-settings seed — an authored
	// scene with volumes enabled must never silently re-enable them).
	SetVar("TOURMALINE_GI_VOLUMES", "0");
	{
		engine::RunConfig c = engine::RunConfig::Parse();
		Check(c.giVolumesKilled, "runconfig.gi-volumes-kill-pin");
	}
	SetVar("TOURMALINE_GI_VOLUMES", "1");
	{
		engine::RunConfig c = engine::RunConfig::Parse();
		Check(!c.giVolumesKilled, "runconfig.gi-volumes-kill-pin-off");
	}
	UnsetVar("TOURMALINE_GI_VOLUMES");

	// (3) TOURMALINE_GI=0 with TOURMALINE_AMBIENT=off: a valid no-op pair
	// (the legacy pin sees an already-non-baked mode and keeps it). The
	// Parse snapshot stores both; the re-application path's
	// LegacyGiAmbientPin must return the CURRENT mode (not nullopt) for a
	// valid no-op — the false "unknown value" warning was the bug.
	SetVar("TOURMALINE_AMBIENT", "off");
	SetVar("TOURMALINE_GI", "0");
	{
		engine::RunConfig c = engine::RunConfig::Parse();
		Check(c.ambient.has_value() && *c.ambient == "off",
			"runconfig.gi-noop-pair-ambient");
		Check(c.gi.has_value() && *c.gi == "0",
			"runconfig.gi-noop-pair-gi");
	}
	UnsetVar("TOURMALINE_AMBIENT");
	UnsetVar("TOURMALINE_GI");

	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
