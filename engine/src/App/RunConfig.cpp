// Central VULKANTOY_* parser (see App/RunConfig.h). Semantics replicate the
// historical per-site getenv blocks exactly; each entry notes its origin so
// future migrations can delete the old site with confidence.

#include <Engine/App/RunConfig.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// NOTE: no extern for _environ here — <cstdlib> already declares it (as a
// macro over __p__environ on MSVC); redeclaring trips C4273.


namespace engine {
namespace {

const char* Get(const char* name) {
	return std::getenv(name);
}

bool Present(const char* name) {
	return std::getenv(name) != nullptr;
}

int ToInt(const char* v, int fallback) {
	if (!v)
		return fallback;
	return std::atoi(v);
}

float ToFloat(const char* v, float fallback) {
	if (!v)
		return fallback;
	return static_cast<float>(std::strtod(v, nullptr));
}

std::string Trimmed(const char* s) {
	std::string_view v{s ? s : ""};
	while (!v.empty() && (v.front() == ' ' || v.front() == '\t'
		|| v.front() == '\r' || v.front() == '\n'))
		v.remove_prefix(1);
	while (!v.empty() && (v.back() == ' ' || v.back() == '\t'
		|| v.back() == '\r' || v.back() == '\n'))
		v.remove_suffix(1);
	return std::string(v);
}

#if defined(_WIN32)
int StrCaseCmp(const char* a, const char* b) { return _stricmp(a, b); }
#else
int StrCaseCmp(const char* a, const char* b) { return strcasecmp(a, b); }
#endif

bool IsOnOff(const std::string& s, bool& out) {
	if (s == "1" || StrCaseCmp(s.c_str(), "on") == 0) {
		out = true;
		return true;
	}
	if (s == "0" || StrCaseCmp(s.c_str(), "off") == 0) {
		out = false;
		return true;
	}
	return false;
}

} // namespace

bool EnvBoolAtoi(const char* v) {
	return v && std::atoi(v) != 0;
}

bool EnvExactZeroKill(const char* v) {
	return v && std::strcmp(v, "0") == 0;
}

bool ParseVec3(const char* v, float& x, float& y, float& z) {
	return v && std::sscanf(v, "%f,%f,%f", &x, &y, &z) == 3;
}

RunConfig RunConfig::Parse() {
	RunConfig c;
	const char* v = nullptr;

	// Camera (WorldRenderer.cpp:530,545,556).
	if ((v = Get("VULKANTOY_CAM_FWD")) != nullptr)
		c.camFwd = v;
	if ((v = Get("VULKANTOY_CAM_POS")) != nullptr)
		c.camPos = v;
	if ((v = Get("VULKANTOY_CAM_YAW")) != nullptr)
		c.camYawDeg = static_cast<float>(std::strtod(v, nullptr));

	// AA pin (validated once here; the snapshot re-applies after every
	// scene seed). Accepts both historical spellings (msaa2x|msaa,
	// smaat2x|t2x); invalid warns and leaves the authored aaMode winning.
	if ((v = Get("VULKANTOY_AA")) != nullptr) {
		const std::string aa = Trimmed(v);
		if (StrCaseCmp(aa.c_str(), "off") == 0)
			c.aa = AaPin::Off;
		else if (StrCaseCmp(aa.c_str(), "msaa2x") == 0 || StrCaseCmp(aa.c_str(), "msaa") == 0)
			c.aa = AaPin::Msaa2x;
		else if (StrCaseCmp(aa.c_str(), "smaa1x") == 0)
			c.aa = AaPin::Smaa1x;
		else if (StrCaseCmp(aa.c_str(), "smaat2x") == 0 || StrCaseCmp(aa.c_str(), "t2x") == 0)
			c.aa = AaPin::SmaaT2x;
		else
			std::fprintf(stderr, "[RunConfig] bad VULKANTOY_AA (want "
				"off|msaa2x|smaa1x|smaat2x): %s\n", v);
	}
	if ((v = Get("VULKANTOY_A2C")) != nullptr)
		c.a2c = std::atoi(v) != 0;
	if ((v = Get("VULKANTOY_TEXTURE_FILTER")) != nullptr)
		c.textureFilter = v;
	if ((v = Get("VULKANTOY_TERRAIN")) != nullptr) {
		c.terrainPinned = true;
		c.terrain = std::atoi(v) != 0;
	}
	// Tiled-terrain heightfield shadows (terrain_hfield/blocks + shadow).
	// hfshadow: 0|off|1|near|2|full (unset = receive-only default at T2,
	// full once the raymarcher lands at T3). Invalid warns, stays unset.
	if ((v = Get("VULKANTOY_TERRAIN_HFSHADOW")) != nullptr) {
		const std::string hs = Trimmed(v);
		if (hs == "0" || StrCaseCmp(hs.c_str(), "off") == 0)
			c.terrainHfShadow = 0;
		else if (hs == "1" || StrCaseCmp(hs.c_str(), "near") == 0)
			c.terrainHfShadow = 1;
		else if (hs == "2" || StrCaseCmp(hs.c_str(), "full") == 0)
			c.terrainHfShadow = 2;
		else
			std::fprintf(stderr, "[RunConfig] bad VULKANTOY_TERRAIN_HFSHADOW "
				"(want 0|off|1|near|2|full): %s\n", v);
	}
	if ((v = Get("VULKANTOY_TERRAIN_HFSHADOW_STEPS")) != nullptr) {
		const int steps = std::atoi(v);
		if (steps >= 4 && steps <= 64)
			c.terrainHfShadowSteps = steps;
		else
			std::fprintf(stderr, "[RunConfig] VULKANTOY_TERRAIN_HFSHADOW_STEPS "
				"out of range (4..64): %s\n", v);
	}

	// Postfx authored pins (validated once here with the raw value shown;
	// invalid warns and leaves the authored scene winning).
	if ((v = Get("VULKANTOY_SSR")) != nullptr) {
		bool on = true;
		if (IsOnOff(Trimmed(v), on))
			c.ssr = on ? OnOffPin::On : OnOffPin::Off;
		else
			std::fprintf(stderr, "[RunConfig] bad VULKANTOY_SSR (want 1|on|0|off): %s\n", v);
	}
	if ((v = Get("VULKANTOY_BLOOM_THRESHOLD")) != nullptr) {
		const float bt = static_cast<float>(std::strtod(v, nullptr));
		if (bt >= 0.0f && bt <= 16000.0f)
			c.bloomThreshold = bt;
		else
			std::fprintf(stderr, "[RunConfig] VULKANTOY_BLOOM_THRESHOLD out of range "
				"(0..16000): %s\n", v);
	}
	if ((v = Get("VULKANTOY_BLOOM_INTENSITY")) != nullptr) {
		const float bi = static_cast<float>(std::strtod(v, nullptr));
		if (bi >= 0.0f && bi <= 4.0f)
			c.bloomIntensity = bi;
		else
			std::fprintf(stderr, "[RunConfig] VULKANTOY_BLOOM_INTENSITY out of range "
				"(0..4): %s\n", v);
	}
	if ((v = Get("VULKANTOY_AO")) != nullptr) {
		const std::string ao = Trimmed(v);
		if (ao == "0" || StrCaseCmp(ao.c_str(), "off") == 0 || StrCaseCmp(ao.c_str(), "none") == 0)
			c.ao = AoPin::None;
		else if (StrCaseCmp(ao.c_str(), "ssao") == 0)
			c.ao = AoPin::Ssao;
		else if (StrCaseCmp(ao.c_str(), "sao") == 0)
			c.ao = AoPin::Sao;
		else
			std::fprintf(stderr, "[RunConfig] bad VULKANTOY_AO (want off|ssao|sao): %s\n", v);
	}
	if ((v = Get("VULKANTOY_AUTO_EXPOSURE")) != nullptr)
		c.autoExposure = std::atoi(v) != 0;
	if ((v = Get("VULKANTOY_CUBEMAP")) != nullptr)
		c.cubemap = std::atoi(v) != 0;
	if ((v = Get("VULKANTOY_DLSS")) != nullptr)
		c.dlss = v;
	if ((v = Get("VULKANTOY_RENDER_SCALE")) != nullptr)
		c.renderScale = ToFloat(v, 0.0f);
	if ((v = Get("VULKANTOY_SPD")) != nullptr) {
		c.spdSet = true;
		c.spd = std::atoi(v) != 0;
	}

	// Volumetric authored pins (validated once; "scene fog settings lose").
	if ((v = Get("VULKANTOY_VOLUMETRIC")) != nullptr) {
		bool on = true;
		if (IsOnOff(Trimmed(v), on))
			c.volumetric = on ? OnOffPin::On : OnOffPin::Off;
		else
			std::fprintf(stderr, "[RunConfig] bad VULKANTOY_VOLUMETRIC (want 1|on|0|off): %s\n", v);
	}
	if ((v = Get("VULKANTOY_VOLUMETRIC_STEPS")) != nullptr) {
		const int vs = std::atoi(v);
		if (vs >= 4 && vs <= 128)
			c.volumetricSteps = vs;
		else
			std::fprintf(stderr, "[RunConfig] VULKANTOY_VOLUMETRIC_STEPS out of range "
				"(4..128): %s\n", v);
	}
	if ((v = Get("VULKANTOY_VOLUMETRIC_DENSITY")) != nullptr) {
		const float vd = static_cast<float>(std::strtod(v, nullptr));
		if (vd >= 0.0f && vd <= 2.0f)
			c.volumetricDensity = vd;
		else
			std::fprintf(stderr, "[RunConfig] VULKANTOY_VOLUMETRIC_DENSITY out of range "
				"(0..2): %s\n", v);
	}
	if ((v = Get("VULKANTOY_VOLUMETRIC_CVOL")) != nullptr) {
		bool on = true;
		if (IsOnOff(Trimmed(v), on))
			c.volumetricCvol = on ? OnOffPin::On : OnOffPin::Off;
		else
			std::fprintf(stderr, "[RunConfig] bad VULKANTOY_VOLUMETRIC_CVOL "
				"(want 1|on|0|off): %s\n", v);
	}

	// Shadows / capture (WorldRenderer.cpp:925..1000).
	if ((v = Get("VULKANTOY_DS")) != nullptr)
		c.ds = v;
	c.dsDump = ToInt(Get("VULKANTOY_DS_DUMP"), -1);
	c.dsViz = ToInt(Get("VULKANTOY_DS_VIZ"), -1);
	if ((v = Get("VULKANTOY_EVSM")) != nullptr) {
		c.evsmSet = true;
		c.evsm = std::atoi(v) != 0;
	}
	if ((v = Get("VULKANTOY_EVSM_SOFT")) != nullptr)
		c.evsmSoft = ToFloat(v, -1.0f);
	if ((v = Get("VULKANTOY_SDSM")) != nullptr)
		c.sdsm = std::atoi(v) != 0;
	c.evsmDump = ToInt(Get("VULKANTOY_EVSM_DUMP"), -1);
	c.capture = ToInt(Get("VULKANTOY_CAPTURE"), -1);
	if ((v = Get("VULKANTOY_DECALS")) != nullptr) {
		// Renderer visibility (:962): 1|on|0|off. App creation (:217 in
		// PakApp.cpp) uses "0"|"off" kill — both spellings converge here;
		// creation sites treat off/false as kill (see Log/WarnUnknown).
		c.decalsSet = true;
		c.decals = !(std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0
#if defined(_WIN32)
			|| _stricmp(v, "off") == 0
#endif
		);
	}
	c.decalAutosplat = ToInt(Get("VULKANTOY_DECAL_AUTOSPLAT"), -1);

	// GI / ambient (WorldRenderer.cpp:1004..1083, :3170).
	if ((v = Get("VULKANTOY_AMBIENT")) != nullptr)
		c.ambient = v;
	if ((v = Get("VULKANTOY_GI")) != nullptr)
		c.gi = v;
	c.giDebug = ToInt(Get("VULKANTOY_GI_DEBUG"), -1);
	c.giDebugLevel = ToInt(Get("VULKANTOY_GI_DEBUG_LEVEL"), -1);
	if ((v = Get("VULKANTOY_GI_POINTS")) != nullptr)
		c.giPoints = std::atoi(v) != 0;
	if ((v = Get("VULKANTOY_GI_OCCLUDE")) != nullptr)
		c.giOcclude = std::atoi(v) != 0;
	if ((v = Get("VULKANTOY_CVOL")) != nullptr)
		c.cvolKilled = std::atoi(v) == 0;
	c.cubemapDebug = ToInt(Get("VULKANTOY_CUBEMAP_DEBUG"), -1);
	if ((v = Get("VULKANTOY_IBL")) != nullptr)
		c.iblKilled = std::atoi(v) == 0;

	// Geometry / culling (WorldRenderer.cpp:800..848, :909; gfx_filter.cpp:230).
	if ((v = Get("VULKANTOY_HIZ")) != nullptr)
		c.hizKilled = std::atoi(v) == 0;
	if ((v = Get("VULKANTOY_LOD")) != nullptr)
		c.lodKilled = std::atoi(v) == 0;
	if ((v = Get("VULKANTOY_LODFULL")) != nullptr)
		c.lodFullKilled = std::atoi(v) == 0;
	if ((v = Get("VULKANTOY_SHADERLOD")) != nullptr)
		c.shaderLodKilled = std::atoi(v) == 0;
	if ((v = Get("VULKANTOY_SPLIT")) != nullptr)
		c.splitKilled = std::atoi(v) == 0;
	if ((v = Get("VULKANTOY_WIREFRAME")) != nullptr)
		c.wireframe = std::atoi(v) == 1;
	if ((v = Get("VULKANTOY_CULLFLAGS")) != nullptr) {
		c.cullFlagsSet = true;
		c.cullFlags = std::strtoull(v, nullptr, 0);
	}
	if ((v = Get("VULKANTOY_FREEZE_CULL")) != nullptr)
		c.freezeCull = std::atoi(v) != 0;
	if ((v = Get("VULKANTOY_MESH")) != nullptr) {
		c.meshSet = true;
		c.mesh = std::atoi(v) != 0;
	}
	if ((v = Get("VULKANTOY_STREAMING")) != nullptr)
		c.streamingKilled = std::atoi(v) == 0;

	// Capture / audit (WorldRenderer.cpp:853..8817, :11045).
	c.audit = ToInt(Get("VULKANTOY_AUDIT"), -1);
	if ((v = Get("VULKANTOY_DEBUGVIEW")) != nullptr)
		c.debugView = v;
	if ((v = Get("VULKANTOY_HITCH_LOG")) != nullptr)
		c.hitchLogMs = ToFloat(v, 0.0f);
	c.materialCensus = Present("VULKANTOY_MATERIAL_CENSUS");
	if ((v = Get("VULKANTOY_SSR_STENCILVIEW")) != nullptr)
		c.ssrStencilView = std::atoi(v);
	c.ssrDump = ToInt(Get("VULKANTOY_SSR_DUMP"), -1);

	// Device / backend (impl.cpp:1598..8127).
	if ((v = Get("VULKANTOY_HOST_COPY")) != nullptr)
		c.hostCopyKilled = std::strcmp(v, "0") == 0;
	// Backend device kill (impl.cpp:1610) is exact "0" like the renderer
	// pin above; both converge into mesh=false when either fires. The
	// exact-"0" device gate is stricter than atoi — "off" does NOT kill
	// at the device level (historical). Preserve that here.
	if ((v = Get("VULKANTOY_MESH")) != nullptr) {
		if (std::strcmp(v, "0") == 0) {
			c.meshSet = true;
			c.mesh = false;
			c.meshDeviceKilled = true;
		}
	}
	if ((v = Get("VULKANTOY_AFTERMATH")) != nullptr)
		c.aftermathKilled = std::strcmp(v, "0") == 0;
	if ((v = Get("VULKANTOY_SERIALIZE")) != nullptr)
		c.serialize = v;
	if ((v = Get("VULKANTOY_BARRIER_TRACE")) != nullptr)
		c.barrierTrace = v;
	if ((v = Get("VULKANTOY_ASYNC_COMPUTE")) != nullptr)
		c.asyncComputeKilled = std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0;
	if ((v = Get("VULKANTOY_PIPELINE_CACHE")) != nullptr)
		c.pipelineCache = v;
	if ((v = Get("VULKANTOY_NO_VALIDATION")) != nullptr)
		c.noValidation = std::atoi(v) != 0;
	if ((v = Get("VULKANTOY_TRACE_WAITS")) != nullptr)
		c.traceWaits = std::atoi(v) != 0;

	// Trace (resource_manager.cpp:21, texture_streamer.cpp:343,
	// WorldRenderer.cpp:7339, impl.cpp presence sites).
	c.texstreamTrace = Present("VULKANTOY_TEXSTREAM_TRACE");

	// Editor (editor_app.cpp, editor_project.cpp, main.cpp default-write).
	if ((v = Get("VULKANTOY_EDITOR_VP_QUAD")) != nullptr)
		c.editorVpQuad = std::atoi(v) != 0;
	if ((v = Get("VULKANTOY_EDITOR_MESHLETS")) != nullptr)
		c.editorMeshlets = std::atoi(v) != 0;
	if ((v = Get("VULKANTOY_EDITOR_LODS")) != nullptr)
		c.editorLods = std::atoi(v) != 0;
	if ((v = Get("VULKANTOY_EDITOR_SHADERLODS")) != nullptr)
		c.editorShaderLods = std::atoi(v) != 0;
	if ((v = Get("VULKANTOY_EDITOR_OPEN_AT")) != nullptr)
		c.editorOpenAt = v;
	if ((v = Get("VULKANTOY_EDITOR_OPEN_AT2")) != nullptr)
		c.editorOpenAt2 = v;

	return c;
}

void RunConfig::Log() const {
	// One line: only non-defaults, so clean runs log clean.
	std::fprintf(stderr, "[RunConfig]");
	auto aaStr = [](AaPin a) {
		switch (a) {
		case AaPin::Off: return "off";
		case AaPin::Msaa2x: return "msaa2x";
		case AaPin::Smaa1x: return "smaa1x";
		case AaPin::SmaaT2x: return "smaat2x";
		}
		return "?";
	};
	auto onOffStr = [](OnOffPin p) { return p == OnOffPin::On ? "on" : "off"; };
	auto aoStr = [](AoPin a) {
		switch (a) {
		case AoPin::None: return "off";
		case AoPin::Ssao: return "ssao";
		case AoPin::Sao: return "sao";
		}
		return "?";
	};
	auto opt = [](const std::optional<std::string>& o, const char* n) {
		if (o)
			std::fprintf(stderr, " %s=%s", n, o->c_str());
	};
	if (aa)
		std::fprintf(stderr, " AA=%s", aaStr(*aa));
	if (ssr)
		std::fprintf(stderr, " SSR=%s", onOffStr(*ssr));
	if (ao)
		std::fprintf(stderr, " AO=%s", aoStr(*ao));
	if (volumetric)
		std::fprintf(stderr, " VOL=%s", onOffStr(*volumetric));
	if (volumetricSteps)
		std::fprintf(stderr, " VOL_STEPS=%d", *volumetricSteps);
	if (capture >= 0)
		std::fprintf(stderr, " CAPTURE=%d", capture);
	if (ssrDump >= 0)
		std::fprintf(stderr, " SSR_DUMP=%d", ssrDump);
	if (evsmDump >= 0)
		std::fprintf(stderr, " EVSM_DUMP=%d", evsmDump);
	if (camPos)
		std::fprintf(stderr, " CAM_POS=%s", camPos->c_str());
	if (camFwd)
		std::fprintf(stderr, " CAM_FWD=%s", camFwd->c_str());
	if (meshSet && !mesh)
		std::fprintf(stderr, " MESH=0");
	if (wireframe)
		std::fprintf(stderr, " WIREFRAME=1");
	if (hitchLogMs > 0)
		std::fprintf(stderr, " HITCH_LOG=%.1fms", static_cast<double>(hitchLogMs));
	if (renderScale > 0)
		std::fprintf(stderr, " RENDER_SCALE=%.2f", static_cast<double>(renderScale));
	std::fprintf(stderr, "\n");
	std::fflush(stderr);
}

void RunConfig::WarnUnknown() {
	// Known set: every name Parse() reads, plus build-time (CMake/env-only)
	// names that are legitimately unset at runtime, plus the dead #if 0
	// TMP-DIAG names (warn would be noise; they stay out of Parse).
	static const char* kKnown[] = {
		"VULKANTOY_AA", "VULKANTOY_A2C", "VULKANTOY_TEXTURE_FILTER", "VULKANTOY_TERRAIN",
		"VULKANTOY_TERRAIN_HFSHADOW", "VULKANTOY_TERRAIN_HFSHADOW_STEPS",
		"VULKANTOY_CAM_FWD", "VULKANTOY_CAM_YAW", "VULKANTOY_CAM_POS",
		"VULKANTOY_VOLUMETRIC", "VULKANTOY_VOLUMETRIC_STEPS", "VULKANTOY_VOLUMETRIC_DENSITY",
		"VULKANTOY_VOLUMETRIC_CVOL", "VULKANTOY_SSR", "VULKANTOY_BLOOM_THRESHOLD",
		"VULKANTOY_BLOOM_INTENSITY", "VULKANTOY_AO", "VULKANTOY_AUTO_EXPOSURE",
		"VULKANTOY_CUBEMAP", "VULKANTOY_HIZ", "VULKANTOY_LOD", "VULKANTOY_LODFULL",
		"VULKANTOY_SHADERLOD", "VULKANTOY_SPLIT", "VULKANTOY_WIREFRAME",
		"VULKANTOY_CULLFLAGS", "VULKANTOY_FREEZE_CULL", "VULKANTOY_AUDIT",
		"VULKANTOY_DEBUGVIEW", "VULKANTOY_DLSS", "VULKANTOY_MESH",
		"VULKANTOY_RENDER_SCALE", "VULKANTOY_DS", "VULKANTOY_DS_DUMP",
		"VULKANTOY_DS_VIZ", "VULKANTOY_CAPTURE", "VULKANTOY_DECALS",
		"VULKANTOY_DECAL_AUTOSPLAT", "VULKANTOY_EVSM", "VULKANTOY_EVSM_SOFT",
		"VULKANTOY_SDSM", "VULKANTOY_EVSM_DUMP", "VULKANTOY_SSR_DUMP",
		"VULKANTOY_IBL", "VULKANTOY_CUBEMAP_DEBUG", "VULKANTOY_AMBIENT",
		"VULKANTOY_GI", "VULKANTOY_GI_DEBUG", "VULKANTOY_GI_DEBUG_LEVEL",
		"VULKANTOY_GI_POINTS", "VULKANTOY_GI_OCCLUDE", "VULKANTOY_STREAMING",
		"VULKANTOY_CVOL", "VULKANTOY_HITCH_LOG",
		"VULKANTOY_MATERIAL_CENSUS", "VULKANTOY_SSR_STENCILVIEW",
		"VULKANTOY_HOST_COPY", "VULKANTOY_AFTERMATH",
		"VULKANTOY_SERIALIZE", "VULKANTOY_BARRIER_TRACE", "VULKANTOY_ASYNC_COMPUTE",
		"VULKANTOY_PIPELINE_CACHE", "VULKANTOY_NO_VALIDATION",
		"VULKANTOY_TEXSTREAM_TRACE", "VULKANTOY_TRACE_WAITS",
		// One-shot debug dumps (TMP-DIAG family): the filter-chain audit,
		// the allocation-ledger dump, the gfx registration dumps.
		"VULKANTOY_FILTERAUDIT", "VULKANTOY_MEMDUMP",
		"VULKANTOY_DUMPGFX", "VULKANTOY_DUMPBATCH", "VULKANTOY_DUMPMAP",
		"VULKANTOY_DUMPMAT", "VULKANTOY_HIDEGROUP", "VULKANTOY_SHOWGROUP",
		"VULKANTOY_STRICT",
		"VULKANTOY_SPD", "VULKANTOY_EDITOR_VP_QUAD", "VULKANTOY_EDITOR_MESHLETS",
		"VULKANTOY_EDITOR_LODS", "VULKANTOY_EDITOR_SHADERLODS",
		"VULKANTOY_EDITOR_OPEN_AT",
		"VULKANTOY_EDITOR_OPEN_AT2",
		// Build-time / CMake-only (legit in env, not runtime knobs):
		"VULKANTOY_CONTENT_DIR", "VULKANTOY_PCH", "VULKANTOY_OIDN",
		"VULKANTOY_DS_VIZ", "VULKANTOY_SHADER_DEBUG", "VULKANTOY_SHADER_OD_RETRY",
		// Stale doc-only (AGENTS.md): warn to guide migration, not error.
		"VULKANTOY_TRANSPARENCY",
	};
	auto known = [&](const char* n) {
		for (const char* k : kKnown)
			if (std::strcmp(n, k) == 0)
				return true;
		return false;
	};
#if defined(_WIN32)
	for (char** e = _environ; e && *e; ++e) {
		const char* eq = std::strchr(*e, '=');
		if (!eq)
			continue;
		// (*e, eq) would mix char*/const char* and fail ctor deduction —
		// count form is unambiguous.
		std::string name(*e, static_cast<size_t>(eq - *e));
		if (name.rfind("VULKANTOY_", 0) == 0 && !known(name.c_str()))
			std::fprintf(stderr, "[RunConfig] unknown VULKANTOY_* var (typo?): %s\n", name.c_str());
	}
#else
	extern char** environ;
	for (char** e = environ; e && *e; ++e) {
		const char* eq = std::strchr(*e, '=');
		if (!eq)
			continue;
		std::string name(*e, static_cast<size_t>(eq - *e));
		if (name.rfind("VULKANTOY_", 0) == 0 && !known(name.c_str()))
			std::fprintf(stderr, "[RunConfig] unknown VULKANTOY_* var (typo?): %s\n", name.c_str());
	}
#endif
	std::fflush(stderr);
}

} // namespace engine
