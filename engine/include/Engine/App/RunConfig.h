// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Central run configuration: ALL TOURMALINE_* env vars, parsed once.
//
// Layering (environments are authored content):
//   1. Scene seed (base): PushVolumetricEnv/PushPostfxEnv copy the authored
//      SceneEnv into renderer state on every scene load/rebind.
//   2. Headless pins (override): this snapshot. WorldRenderer stores it at
//      Configure and re-applies it after EVERY seed (scene load/rebind
//      would otherwise stomp a pin). Parse() validates each pin once and
//      warns; invalid pins are left unset so the authored scene wins.
//   3. UI session (live): the demo App's panels/keys stay live after pins.
//      Exposing settings in the demo App is fine — the values remain
//      authored content (session overrides, never written back).
//
// Problem this fixes: ~60 TOURMALINE_* knobs were read via scattered
// std::getenv with no types, no ranges, no startup log — typos silently
// no-op'd, A/B runs were not reproducible, and re-reads after a seed could
// disagree mid-run. This header is the single declaration of every knob;
// Parse() reads the environment once, validates, logs one line, and warns
// on unknown TOURMALINE_* names.
//
// Migration discipline (god objects stay intact):
//   - New code takes `const RunConfig&` (or the nested slab it needs).
//   - Authored pins (below) are consumed from the Configure snapshot via
//     ApplyHeadless*Overrides — never getenv after startup.
//   - Device/debug knobs keep their getenv until migrated; semantics are
//     preserved exactly: bool = atoi!=0 unless noted "exact 0", enums
//     accept the same spellings (both AA spellings).

#include <cstdint>
#include <optional>
#include <string>

namespace engine {

// Validated authored-content pins (layer 2 of the layering above).
// Parse() maps the raw spellings once; invalid values warn and stay unset
// so the authored scene wins. Nullopt = unset = scene seed wins.
enum class OnOffPin { Off, On };
enum class AaPin { Off, Msaa2x, Smaa1x, SmaaT2x };
enum class AoPin { None, Ssao, Sao };

struct RunConfig {
	// ---- camera (WorldRenderer.cpp Configure) ----
	std::optional<std::string> camFwd; // "x,y,z", wins over yaw
	std::optional<std::string> camPos; // "x,y,z"
	std::optional<float> camYawDeg;

	// ---- antialiasing / filtering ----
	// aa: off|msaa2x|msaa|smaa1x|smaat2x|t2x (both historical spellings accepted)
	std::optional<AaPin> aa;
	bool a2c = false;
	// textureFilter: trilinear|2x|4x|8x|16x
	std::optional<std::string> textureFilter;
	bool terrainPinned = false;
	bool terrain = false;

	// ---- postfx (authored pins: snapshot, re-applied after every seed) ----
	// ssr: 1|on|0|off (unset = authored ssrEnabled wins)
	std::optional<OnOffPin> ssr;
	// bloomThreshold: 0..16000 pre-exposure HDR (unset = authored wins)
	std::optional<float> bloomThreshold;
	// bloomIntensity: 0..4 tonemap add scale (unset = authored wins)
	std::optional<float> bloomIntensity;
	// ao: 0|off|none|ssao|sao (unset = authored aoMode wins)
	std::optional<AoPin> ao;
	// autoExposure: 0 = manual, else auto (unset = authored/host pin wins).
	// Applied AFTER the host m_autoExposurePin, so an explicit env pin is
	// the more deliberate override and wins.
	std::optional<bool> autoExposure;
	// cubemap: false = kill (analytic-sky fallback); true = keep (same as
	// unset — the kill is the only pin). Unset = authored cubemapEnabled wins.
	std::optional<bool> cubemap;
	// dlss: off|ultraperf|perf|balanced|quality|ultraquality|dlaa
	std::optional<std::string> dlss;
	float renderScale = 0.0f; // <=0 = default
	bool spdSet = false;
	bool spd = false;

	// ---- volumetric (authored pins: snapshot, re-applied after every seed) ----
	// volumetric: 1|on|0|off (unset = authored fog tier wins)
	std::optional<OnOffPin> volumetric;
	// volumetricSteps: 4..128 march steps (unset = authored wins)
	std::optional<int> volumetricSteps;
	// volumetricDensity: 0..2 extinction (unset = authored wins)
	std::optional<float> volumetricDensity;
	// volumetricCvol: fog-GI cvol walk on/off (unset = authored wins)
	std::optional<OnOffPin> volumetricCvol;

	// ---- shadows ----
	// ds: 1|on|0|off (empty = default)
	std::optional<std::string> ds;
	int dsDump = -1;
	int dsViz = -1; // 0..7, Debug DS_DEBUG_VIZ only
	bool evsmSet = false;
	bool evsm = false; // default OFF (PCF)
	float evsmSoft = -1.0f;
	bool sdsm = false;
	int evsmDump = -1;
	int capture = -1; // frame index, must be < --bench
	bool decalsSet = false;
	bool decals = true;
	int decalAutosplat = -1;

	// ---- GI / ambient ----
	// ambient: all|lightmaps|lm|probes|skyibl|ibl|off|none
	std::optional<std::string> ambient;
	// giLegacy: 1|on|0|off overlap with ambient
	std::optional<std::string> gi;
	int giDebug = -1; // 0..4
	int giDebugLevel = -1; // 0..7
	bool giPoints = false;
	bool giOcclude = false;
	bool cvolKilled = false; // CVOL=0 demotes FULL->Octree
	int cubemapDebug = -1; // 0..2
	bool iblKilled = false; // IBL=0 mutes ambient

	// ---- geometry / culling ----
	bool hizKilled = false;
	bool lodKilled = false; // LOD=0 pins LOD0
	bool lodFullKilled = false;
	bool shaderLodKilled = false;
	bool splitKilled = false; // SPLIT=0 legacy prepass flow
	bool wireframe = false;
	uint64_t cullFlags = 0;
	bool cullFlagsSet = false;
	bool freezeCull = false;
	bool meshSet = false; // renderer-level pin (WorldRenderer.cpp:909)
	bool mesh = true;
	bool streamingKilled = false; // STREAMING=0 full-resident

	// ---- capture / audit ----
	int audit = -1;
	std::optional<std::string> debugView;
	float hitchLogMs = 0.0f; // 0 = off
	bool materialCensus = false;
	int ssrStencilView = 0; // 0 = off, 1..3 armed (needs SSR_DUMP)
	int ssrDump = -1;

	// ---- device / backend (impl.cpp) ----
	bool hostCopyKilled = false; // exact "0"
	bool meshDeviceKilled = false; // exact "0", device-level
	bool aftermathKilled = false; // exact "0"
	std::optional<std::string> serialize; // csv of all|split|main|uploads
	std::optional<std::string> barrierTrace; // frame N or all
	bool asyncComputeKilled = false; // "0"|"off"
	std::optional<std::string> pipelineCache; // unset=default path, off|0=null
	bool noValidation = false;
	bool traceWaits = false;

	// ---- trace ----
	bool texstreamTrace = false; // any of 4 presence sites

	// ---- editor ----
	bool editorVpQuad = false;
	bool editorMeshlets = true;
	bool editorLods = true;
	bool editorShaderLods = false; // far-shading derivation opt-in (default off)
	std::optional<std::string> editorOpenAt;
	std::optional<std::string> editorOpenAt2;

	// ---- terrain (tiled heightfield; see Rendergraph/terrain_*.h) ----
	// hfshadow: -1 unset | 0|off | 1|near (shadowmap receive only) |
	//           2|full (receive + raymarched distant self-shadow)
	int terrainHfShadow = -1;
	// hfshadowSteps: 4..64 heightfield raymarch steps (unset = default 16)
	std::optional<int> terrainHfShadowSteps;

	// Parse the environment once. Never throws; bad values warn to stderr
	// and fall back to defaults (same as the historical per-site behavior).
	static RunConfig Parse();
	// One startup log line (selected non-defaults) + full dump when
	// TOURMALINE_TRACE-equivalent verbosity is wanted. Call once from main.
	void Log() const;
	// Warn for any TOURMALINE_* in the environment this struct does not know.
	// Call once from main after Parse.
	static void WarnUnknown();
};

// Compat helpers with the exact historical semantics, so migrated call sites
// read identically to the old getenv blocks.
bool EnvBoolAtoi(const char* v); // atoi(v)!=0
bool EnvExactZeroKill(const char* v); // v=="0"
bool ParseVec3(const char* v, float& x, float& y, float& z);

} // namespace engine
