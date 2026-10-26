// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// pak_host_tests: CPU-only regression tests for the pak-viewer HOST layer
// (app/src): CLI parsing (validated numerics, the --pak gate, the
// dedicated options object), pak path resolution (the exe-relative
// fallback for ordinary relative paths), and corruption handling
// (PakBackend::Open fail-closed on garbage / CRC-torn images). No GPU,
// no window: everything here runs headless in CI.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h> // GetModuleFileNameA (exe-relative resolution test)
#else
#include <unistd.h> // readlink (/proc/self/exe)
#endif

#include "Host.h"
#include "PakPreload.h"

#include <Engine/Asset/pak.h>

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

// ---- CLI parsing (app::ParsePakAppArgs -> app::ViewerOptions) ----

bool ParseOk(std::vector<const char*> argv, app::ViewerOptions& opts) {
	int exitCode = 0;
	return app::ParsePakAppArgs(int(argv.size()), argv.data(), opts, exitCode)
		&& exitCode == 0;
}

bool ParseFails(std::vector<const char*> argv, int& exitCode) {
	app::ViewerOptions opts;
	return !app::ParsePakAppArgs(int(argv.size()), argv.data(), opts,
			exitCode);
}

void TestCliParse() {
	app::ViewerOptions opts;
	Check(ParseOk({ "App", "--pak", "x.pak" }, opts)
		&& opts.pakPath == "x.pak",
		"cli.pak.required-value");
	Check(ParseOk({ "App", "--pak=y.pak", "--scene-index=3" }, opts)
		&& opts.sceneIndex == 3,
		"cli.scene-index.equals-form");
	// The required-flag gate: no --pak, no run.
	{
		int ec = 0;
		Check(ParseFails({ "App" }, ec) && ec != 0,
			"cli.no-pak-refused");
	}
	// Unknown args refuse with a failure exit.
	{
		int ec = 0;
		Check(ParseFails({ "App", "--pak", "x.pak", "--nonsense" }, ec)
			&& ec != 0, "cli.unknown-arg-refused");
	}
	// --help exits SUCCESS (documented usage path).
	{
		int ec = 1;
		app::ViewerOptions opts;
		std::vector<const char*> helpArgv = { "App", "--help" };
		Check(!app::ParsePakAppArgs(int(helpArgv.size()), helpArgv.data(),
				opts, ec) && ec == 0,
			"cli.help-success-exit");
	}
	// The removed --gltf refuses loudly.
	{
		int ec = 0;
		Check(ParseFails({ "App", "--pak", "x.pak", "--gltf" }, ec)
			&& ec != 0, "cli.gltf-removed-refused");
	}
	// ---- Validated numerics (finding #11): malformed values REFUSE the
	// launch instead of silently mapping to zero. The old atoi/atof
	// accepted all of these as 0 — and a zero bench budget silently
	// flipped the run to interactive (a bench script spinning forever).
	{
		int ec = 0;
		Check(ParseFails({ "App", "--pak", "x.pak", "--bench", "12x" }, ec),
			"cli.bench.malformed-refused");
	}
	{
		int ec = 0;
		Check(ParseFails({ "App", "--pak", "x.pak", "--bench=0" }, ec),
			"cli.bench.zero-refused");
	}
	{
		int ec = 0;
		Check(ParseFails({ "App", "--pak", "x.pak",
				"--scene-index=abc" }, ec),
			"cli.scene-index.malformed-refused");
	}
	{
		int ec = 0;
		Check(ParseFails({ "App", "--pak", "x.pak",
				"--scene-index=-1" }, ec),
			"cli.scene-index.negative-refused");
	}
	{
		int ec = 0;
		Check(ParseFails({ "App", "--pak", "x.pak",
				"--bench_move=fast" }, ec),
			"cli.bench_move.malformed-refused");
	}
	{
		int ec = 0;
		Check(ParseFails({ "App", "--pak", "x.pak",
				"--bench_move=99999" }, ec),
			"cli.bench_move.out-of-range-refused");
	}
	// Well-formed values still parse.
	Check(ParseOk({ "App", "--pak", "x.pak", "--bench", "64",
			"--bench_move", "4.5", "--scene-index", "2" }, opts)
		&& opts.benchFrames == 64
		&& (opts.benchMoveSpeed > 4.49f && opts.benchMoveSpeed < 4.51f)
		&& opts.sceneIndex == 2,
		"cli.numerics-well-formed-accepted");
	// ---- The dedicated options object (finding #16): the parser cannot
	// erase host-supplied config. Pre-install an extension hook + a run
	// pin, parse, verify both survive untouched (the old wholesale
	// `cfg = PakAppConfig{}` reset silently wiped them).
	{
		app::PakAppConfig cfg;
		bool hookRan = false;
		cfg.extraElement = [&hookRan](smvk::App&, engine::WorldRenderer&,
				std::shared_ptr<smvk::World*>) {
			hookRan = true;
			return nullptr;
		};
		cfg.extraOwnsCamera = true;
		app::ViewerOptions& optsRef = cfg.cli;
		Check(ParseOk({ "App", "--pak", "x.pak" }, optsRef)
			&& cfg.extraElement && !hookRan
			&& cfg.extraOwnsCamera,
			"cli.parse.preserves-host-config");
	}
}

// ---- Pak path resolution (app::ResolvePakOpenPath) ----

void TestPakPathResolution() {
	namespace fs = std::filesystem;
	// Scratch tree under the temp dir: one "cwd" root and one "exe" root
	// (the TEST BINARY's directory — the exe-relative fallback's base).
	const fs::path tmp = fs::temp_directory_path()
		/ "tourmaline_pak_host_tests";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);
	const fs::path exeDir =
		fs::absolute(fs::path([]() {
#ifdef _WIN32
			char buf[1024] = {};
			GetModuleFileNameA(nullptr, buf, DWORD(sizeof(buf)));
			return std::string(buf);
#else
			char buf[4096] = {};
			const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
			if (n > 0) buf[n] = '\0';
			return std::string(buf);
#endif
		}())).parent_path();
	const fs::path exePakDir = exeDir / "pakhost_test_content";
	fs::create_directories(exePakDir, ec);
	const fs::path exePak = exePakDir / "relative.pak";
	{
		std::ofstream f(exePak, std::ios::binary);
		f << "fake pak bytes";
	}
	// The finding (#12): an ORDINARY relative path whose file exists ONLY
	// beside the executable (not in the CWD) must resolve through the
	// exe-relative fallback. Run with the CWD parked in the scratch dir
	// where the pak does NOT exist.
	const fs::path cwdSave = fs::current_path(ec);
	fs::current_path(tmp, ec);
	{
		const std::string resolved =
			app::ResolvePakOpenPath("pakhost_test_content/relative.pak");
		Check(!resolved.empty()
			&& fs::path(resolved) == fs::absolute(exePak),
			"pakpath.ordinary-relative.exe-relative-fallback");
	}
	// Missing everywhere: empty result (the launch-failure path) — the
	// KickPakPreload future must report the error without throwing
	// (headless launch failures must EXIT, not crash or hang).
	{
		const std::string resolved =
			app::ResolvePakOpenPath("pakhost_test_content/absent.pak");
		Check(resolved.empty(), "pakpath.missing-empty");
		auto fut = app::KickPakPreload("pakhost_test_content/absent.pak");
		const app::PakPreload pre = fut.get();
		Check(!pre.backend && !pre.error.empty(),
			"pakpath.preload-missing-pak.errors-gracefully");
	}
	fs::current_path(cwdSave, ec);
	// Absolute paths resolve as-is (no fallback needed).
	{
		const std::string resolved =
			app::ResolvePakOpenPath(exePak.string());
		Check(!resolved.empty(), "pakpath.absolute-as-is");
	}
	fs::remove_all(tmp, ec);
}

// ---- Corruption handling (smvk::PakBackend::Open fail-closed) ----

void TestPakCorruption() {
	// A minimal VALID pak via PakWriter (a single Scene-kind blob).
	smvk::PakWriter writer;
	Check(writer.AddBlob(smvk::PakAssetKind::Scene, "scene/only",
			{ 1, 2, 3, 4 }), "pakcorruption.writer-adds-blob");
	const std::vector<uint8_t> image = writer.Finalize();
	{
		std::string err;
		auto backend = smvk::PakBackend::Open(
			std::make_shared<std::vector<uint8_t>>(image), "ok", err);
		Check(backend != nullptr, "pakcorruption.valid-pak-opens");
	}
	// Garbage bytes: rejected with a reason.
	{
		std::string err;
		const std::vector<uint8_t> garbage(64, 0xAB);
		auto backend = smvk::PakBackend::Open(
			std::make_shared<std::vector<uint8_t>>(garbage), "bad", err);
		Check(backend == nullptr && !err.empty(),
			"pakcorruption.garbage-rejected");
	}
	// A torn write (CRC mismatch): one byte flipped mid-image.
	{
		std::vector<uint8_t> torn = image;
		torn[torn.size() / 2] ^= 0xFF;
		std::string err;
		auto backend = smvk::PakBackend::Open(
			std::make_shared<std::vector<uint8_t>>(torn), "torn", err);
		Check(backend == nullptr && !err.empty(),
			"pakcorruption.crc-torn-rejected");
	}
	// Truncated image: rejected.
	{
		std::vector<uint8_t> trunc(image.begin(),
			image.begin() + std::ptrdiff_t(image.size() / 2));
		std::string err;
		auto backend = smvk::PakBackend::Open(
			std::make_shared<std::vector<uint8_t>>(trunc), "short", err);
		Check(backend == nullptr && !err.empty(),
			"pakcorruption.truncated-rejected");
	}
}

} // namespace

int main() {
	TestCliParse();
	TestPakPathResolution();
	TestPakCorruption();
	std::cout << "pak_host_tests: " << (g_checks - g_failures) << "/"
		<< g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
