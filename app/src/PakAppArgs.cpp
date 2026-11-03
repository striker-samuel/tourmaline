// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// CLI parsing for the pak-viewer host (the APP's own host layer; fills
// ViewerOptions from argv — see Host.h). Numeric flags go through
// validated parsers: the old atoi/atof silently mapped malformed values
// ("--bench 12x", "--scene-index 1e3oops", an empty "--bench") to 0, and
// a ZERO bench frame budget silently flipped the run to interactive
// mode — a bench script that typo'd its budget would spin forever
// instead of failing.

#include "Host.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

namespace app {
namespace {

void PrintUsage(const char* prog) {
	std::cout << "Usage: " << (prog ? prog : "App")
		<< " --pak <path.pak> (REQUIRED, repeatable)"
		" [--scene <name>] [--scene-index N]\n"
		<< "  " << (prog ? prog : "App")
		<< " --project <path.tapp> (the generated appmanifest launch)\n"
		<< "  [--minimal] [--filter-only] [--with <csv>] [--without <csv>]\n"
		<< "  [--bench <frames>] [--bench_move <m/s>] [--present <fifo|mailbox|immediate>] [--max-fps N] [--disable_control] [--no-dlss]\n"
		<< "  [--capture] [--use_optick] [--use_optick_gpu] [--no-vfs]\n"
		<< "  --pak runs cooked paks — REQUIRED (or --project), there is\n"
		<< "  no default (no entry-point pak = the App refuses to run).\n"
		<< "  ONE --pak runs the legacy single-pak lane: scenes inside\n"
		<< "  are discovered at startup, index 0 loads unless\n"
		<< "  --scene-index picks another (--scene <name> loads by the\n"
		<< "  cooked scene's identity instead). SEVERAL --pak run the\n"
		<< "  multi-pak discovery lane: every pak joins ONE content\n"
		<< "  session (cross-pak content resolves through it — a pak\n"
		<< "  carrying its '#ids' identity table contributes its ids\n"
		<< "  too), the entry scene is --scene's name or the first\n"
		<< "  scene-bearing pak's. The App loads OUR format only:\n"
		<< "  preprocess glTF through the editor (import -> cook).\n"
		<< "  --project launches a recipe build's appmanifest: every listed\n"
		<< "  pak mounts (manifest-relative, listed order) and the entry\n"
		<< "  scene loads by identity — no --scene-index, no source .tour.\n"
		<< "  --disable_control freezes camera input for deterministic benching\n"
		<< "  --bench_move flies forward at constant velocity (default 4 m/s) past warmup\n"
		<< "  --bench also pins fixed manual exposure (no auto-exposure)\n"
		<< "  --bench also forces vsync off (uncapped immediate present; --present wins)\n"
		<< "  --max-fps N caps interactive CPU frames to 1..1000 FPS (omitted = uncapped; ignored by --bench)\n"
		<< "  --input-record <path> writes an engine input tape at exit\n"
		<< "  --input-replay <path> replays engine input + timestep by tick\n"
		<< "  --snapshot-sequence <dir> exports Frame_0001.png ... with embedded state\n"
		<< "  [--snapshot-count N] [--snapshot-keyframe N] [--snapshot-code-id HASH]\n"
		<< "  --no-dlss keeps the Streamline interposer out of the Vulkan chain\n"
		<< "  (RenderDoc captures crash through the SL layer + NGX cubins)\n"
		<< "  --capture is the one-flag RenderDoc launch config: --no-dlss plus\n"
		<< "  low latency (Reflex/PCL, rides the Streamline gate) plus Optick off\n"
		<< "  (wins over --use_optick[_gpu]; TOURMALINE_CAPTURE env is the\n"
		<< "  unrelated PNG-export pin)\n"
		<< "  --use_optick enables the Optick CPU capture session (bench.opt)\n"
		<< "  --use_optick_gpu also brings up Optick GPU timelines (implies --use_optick)\n"
		<< "  --no-vfs mutes the loose VFS discovery mounts: only the pak\n"
		<< "  backend serves scene content (proves the pak is self-contained)\n";
}

// Validated integer parse: the whole token must be a base-10 integer in
// [lo, hi] (an optional single +/- sign, digits only). False on any
// trailing garbage, overflow, or out-of-range value — the caller rejects
// the invocation instead of running with a silently-zeroed flag.
bool ParseInt(const char* s, long long lo, long long hi, long long& out) {
	if (!s || !*s)
		return false;
	errno = 0;
	char* end = nullptr;
	const long long v = std::strtoll(s, &end, 10);
	if (errno == ERANGE || end == s || (end && *end != '\0'))
		return false;
	if (v < lo || v > hi)
		return false;
	out = v;
	return true;
}

// Validated float parse: the whole token must be a finite float in
// [lo, hi] (strtod grammar — "4", "0.5", "1e2"; "inf"/"nan" rejected by
// the finiteness + range check).
bool ParseFloat(const char* s, float lo, float hi, float& out) {
	if (!s || !*s)
		return false;
	errno = 0;
	char* end = nullptr;
	const float v = std::strtof(s, &end);
	if (errno == ERANGE || end == s || (end && *end != '\0'))
		return false;
	if (!(v >= lo && v <= hi)) // NaN fails every comparison: rejected here
		return false;
	out = v;
	return true;
}

// Numeric-flag reject path: loud + usage + failure exit.
bool RejectNumeric(const char* flag, const char* value, const char* want,
		int& exitCode) {
	std::cerr << "Bad value for " << flag << ": '" << (value ? value : "")
		<< "' (" << want << ")\n";
	PrintUsage(nullptr);
	exitCode = EXIT_FAILURE;
	return false;
}

} // namespace

bool ParsePakAppArgs(int argc, const char* argv[], ViewerOptions& opts,
	int& exitCode) {
	// The parser owns ONLY the ViewerOptions object (a fresh default per
	// parse — the CLI surface is stateless). Host-supplied config
	// (RunConfig pins, the extension hooks) lives on PakAppConfig and is
	// NEVER reset, erased, or even read here: the old wholesale
	// `cfg = PakAppConfig{}` reset silently wiped whatever the host had
	// pre-installed (the documented RunConfig wipe bug was the first
	// casualty; the extension hooks went the same way).
	opts = ViewerOptions{};
	for (int i = 1; i < argc; ++i) {
		const char* a = argv[i];
		if ((std::strcmp(a, "--pak") == 0) && i + 1 < argc) {
			opts.pakPaths.emplace_back(argv[++i]);
		}
		else if (std::strncmp(a, "--pak=", 6) == 0) {
			opts.pakPaths.emplace_back(a + 6);
		}
		else if ((std::strcmp(a, "--project") == 0) && i + 1 < argc) {
			opts.project = argv[++i];
		}
		else if (std::strncmp(a, "--project=", 10) == 0) {
			opts.project = a + 10;
		}
		else if ((std::strcmp(a, "--scene") == 0) && i + 1 < argc) {
			opts.sceneName = argv[++i];
		}
		else if (std::strncmp(a, "--scene=", 8) == 0) {
			opts.sceneName = a + 8;
		}
		else if ((std::strcmp(a, "--scene-index") == 0) && i + 1 < argc) {
			const char* v = argv[++i];
			long long n = 0;
			if (!ParseInt(v, 0, 1024 * 1024, n))
				return RejectNumeric("--scene-index", v,
					"a scene index, 0..1048576", exitCode);
			opts.sceneIndex = int(n);
		}
		else if (std::strncmp(a, "--scene-index=", 14) == 0) {
			const char* v = a + 14;
			long long n = 0;
			if (!ParseInt(v, 0, 1024 * 1024, n))
				return RejectNumeric("--scene-index", v,
					"a scene index, 0..1048576", exitCode);
			opts.sceneIndex = int(n);
		}
		else if (std::strcmp(a, "--minimal") == 0) {
			opts.minimal = true;
		}
		else if (std::strcmp(a, "--filter-only") == 0) {
			opts.filterOnly = true;
		}
		else if ((std::strcmp(a, "--with") == 0) && i + 1 < argc) {
			opts.withCsv = argv[++i];
		}
		else if (std::strncmp(a, "--with=", 7) == 0) {
			opts.withCsv = a + 7;
		}
		else if ((std::strcmp(a, "--without") == 0) && i + 1 < argc) {
			opts.withoutCsv = argv[++i];
		}
		else if (std::strncmp(a, "--without=", 10) == 0) {
			opts.withoutCsv = a + 10;
		}
		else if (std::strcmp(a, "--bench") == 0 && i + 1 < argc) {
			const char* v = argv[++i];
			long long n = 0;
			if (!ParseInt(v, 1, 1000000, n))
				return RejectNumeric("--bench", v,
					"a positive frame count, 1..1000000", exitCode);
			opts.benchFrames = int(n);
		}
		else if (std::strncmp(a, "--bench=", 8) == 0) {
			const char* v = a + 8;
			long long n = 0;
			if (!ParseInt(v, 1, 1000000, n))
				return RejectNumeric("--bench", v,
					"a positive frame count, 1..1000000", exitCode);
			opts.benchFrames = int(n);
		}
		else if (std::strcmp(a, "--bench_move") == 0
			|| std::strcmp(a, "--bench-move") == 0) {
			// Bare flag: default flight speed (4 m/s walks the bistro street
			// at a good LOD/streaming churn rate). Optional value follows.
			opts.benchMoveSpeed = 4.0f;
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				const char* v = argv[++i];
				if (!ParseFloat(v, 0.01f, 1000.0f, opts.benchMoveSpeed))
					return RejectNumeric("--bench_move", v,
						"a speed in m/s, 0.01..1000", exitCode);
			}
		}
		else if (std::strncmp(a, "--bench_move=", 14) == 0
			|| std::strncmp(a, "--bench-move=", 13) == 0) {
			const char* v = a + (std::strncmp(a, "--bench_move=", 14) == 0 ? 14 : 13);
			if (!ParseFloat(v, 0.01f, 1000.0f, opts.benchMoveSpeed))
				return RejectNumeric("--bench_move", v,
					"a speed in m/s, 0.01..1000", exitCode);
		}
		else if (std::strcmp(a, "--max-fps") == 0 && i + 1 < argc) {
			const char* v = argv[++i];
			long long n = 0;
			if (!ParseInt(v, 1, 1000, n))
				return RejectNumeric("--max-fps", v,
					"an FPS limit, 1..1000", exitCode);
			opts.maxFps = int(n);
		}
		else if (std::strncmp(a, "--max-fps=", 10) == 0) {
			const char* v = a + 10;
			long long n = 0;
			if (!ParseInt(v, 1, 1000, n))
				return RejectNumeric("--max-fps", v,
					"an FPS limit, 1..1000", exitCode);
			opts.maxFps = int(n);
		}
		else if ((std::strcmp(a, "--present") == 0) && i + 1 < argc) {
			opts.presentMode = argv[++i];
			opts.presentSet = true;
		}
		else if (std::strncmp(a, "--present=", 10) == 0) {
			opts.presentMode = a + 10;
			opts.presentSet = true;
		}
		else if (std::strcmp(a, "--input-record") == 0 && i + 1 < argc) {
			opts.inputRecordPath = argv[++i];
		}
		else if (std::strncmp(a, "--input-record=", 15) == 0) {
			opts.inputRecordPath = a + 15;
		}
		else if (std::strcmp(a, "--input-replay") == 0 && i + 1 < argc) {
			opts.inputReplayPath = argv[++i];
		}
		else if (std::strncmp(a, "--input-replay=", 15) == 0) {
			opts.inputReplayPath = a + 15;
		}
		else if (std::strcmp(a, "--snapshot-sequence") == 0 && i + 1 < argc) {
			opts.snapshotSequenceDir = argv[++i];
		}
		else if (std::strncmp(a, "--snapshot-sequence=", 20) == 0) {
			opts.snapshotSequenceDir = a + 20;
		}
		else if ((std::strcmp(a, "--snapshot-count") == 0 ||
			std::strcmp(a, "--snapshot-keyframe") == 0) && i + 1 < argc) {
			const bool count = std::strcmp(a, "--snapshot-count") == 0;
			const char* v = argv[++i];
			long long parsed = 0;
			if (!ParseInt(v, 1, 1000000, parsed))
				return RejectNumeric(count ? "--snapshot-count" : "--snapshot-keyframe",
					v, "a positive frame count 1..1000000", exitCode);
			if (count) opts.snapshotCount = static_cast<uint32_t>(parsed);
			else opts.snapshotKeyframeInterval = static_cast<uint32_t>(parsed);
		}
		else if (std::strncmp(a, "--snapshot-count=", 17) == 0 ||
			std::strncmp(a, "--snapshot-keyframe=", 20) == 0) {
			const bool count = std::strncmp(a, "--snapshot-count=", 17) == 0;
			const char* v = a + (count ? 17 : 20);
			long long parsed = 0;
			if (!ParseInt(v, 1, 1000000, parsed))
				return RejectNumeric(count ? "--snapshot-count" : "--snapshot-keyframe",
					v, "a positive frame count 1..1000000", exitCode);
			if (count) opts.snapshotCount = static_cast<uint32_t>(parsed);
			else opts.snapshotKeyframeInterval = static_cast<uint32_t>(parsed);
		}
		else if (std::strcmp(a, "--snapshot-code-id") == 0 && i + 1 < argc) {
			opts.snapshotCodeId = argv[++i];
		}
		else if (std::strncmp(a, "--snapshot-code-id=", 19) == 0) {
			opts.snapshotCodeId = a + 19;
		}
		else if (std::strcmp(a, "--disable_control") == 0 || std::strcmp(a, "--disable-control") == 0) {
			opts.disableControl = true;
		}
		else if (std::strcmp(a, "--use_optick") == 0) {
			opts.useOptick = true;
		}
		else if (std::strcmp(a, "--use_optick_gpu") == 0) {
			opts.useOptick = true;
			opts.useOptickGpu = true;
		}
		else if (std::strcmp(a, "--no-dlss") == 0 || std::strcmp(a, "--no_dlss") == 0) {
			opts.noDlss = true;
		}
		else if (std::strcmp(a, "--capture") == 0) {
			opts.capture = true;
		}
		else if (std::strcmp(a, "--no-vfs") == 0 || std::strcmp(a, "--no_vfs") == 0) {
			opts.noVfs = true;
		}
		else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
			PrintUsage(argv[0]);
			exitCode = EXIT_SUCCESS;
			return false;
		}
		else {
			std::cerr << "Unknown argument: " << a << "\n";
			PrintUsage(argv[0]);
			exitCode = EXIT_FAILURE;
			return false;
		}
	}
	if (opts.snapshotCodeId.size() > 256) {
		std::cerr << "--snapshot-code-id exceeds 256 characters.\n";
		exitCode = EXIT_FAILURE;
		return false;
	}
	if (!opts.inputRecordPath.empty() && !opts.inputReplayPath.empty()) {
		std::cerr << "--input-record and --input-replay are mutually exclusive.\n";
		exitCode = EXIT_FAILURE;
		return false;
	}
	// The entry-point gate: --pak (one or more) OR --project (one entry
	// point, no silent defaults). The appmanifest path is exclusive with
	// the pak surface (--pak/--scene/--scene-index): identity-based
	// launch never mixes with index-based.
	if (!opts.project.empty() && !opts.pakPaths.empty()) {
		std::cerr << "--project and --pak are mutually exclusive: the"
			" appmanifest names its own entry point.\n";
		PrintUsage(argv[0]);
		exitCode = EXIT_FAILURE;
		return false;
	}
	if (!opts.project.empty() && (opts.sceneIndex != 0
			|| !opts.sceneName.empty())) {
		std::cerr << "--scene/--scene-index have no effect with"
			" --project: the appmanifest resolves the entry scene by"
			" identity.\n";
		PrintUsage(argv[0]);
		exitCode = EXIT_FAILURE;
		return false;
	}
	// Index vs identity never mixes: --scene (the cooked name) and
	// --scene-index pick the entry scene through different addressing.
	if (!opts.sceneName.empty() && opts.sceneIndex != 0) {
		std::cerr << "--scene and --scene-index are mutually exclusive:"
			" pick the entry scene by identity or by index, never"
			" both.\n";
		PrintUsage(argv[0]);
		exitCode = EXIT_FAILURE;
		return false;
	}
	if (opts.pakPaths.empty() && opts.project.empty()) {
		std::cerr << "--pak <path.pak> (or --project <path.tapp>) is"
			" REQUIRED: no entry point, nothing to run.\n";
		PrintUsage(argv[0]);
		exitCode = EXIT_FAILURE;
		return false;
	}
	// The single-pak legacy lane's mirror field.
	if (!opts.pakPaths.empty())
		opts.pakPath = opts.pakPaths.front();
	exitCode = EXIT_SUCCESS;
	return true;
}

} // namespace app
