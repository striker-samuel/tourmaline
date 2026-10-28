// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The project build orchestrator (see build_pipeline.h). Stage tools are
// resolved beside THIS executable (the build root — every tool shares
// RUNTIME_OUTPUT_DIRECTORY) and run as subprocesses with exit-code
// propagation. The build SETTINGS (stage flags) live in the project's
// .tourmaline/build-settings.txt — persisted, never duplicated in a
// script; missing keys keep the defaults below.

#include <project_pipeline/build_pipeline.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

#include <Engine/Asset/tour.h>

#include <project_pipeline/import_pipeline.h>
#include <project_pipeline/project.h>

namespace smvk::project {

namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h> // GetModuleFileNameA (the stage-tool resolution)
#else
#include <unistd.h> // readlink (/proc/self/exe), fork, execv, _exit
#include <sys/wait.h> // waitpid + the WIFEXITED/WEXITSTATUS macros
#endif

// ---- build settings (persisted per project) -------------------------------

struct BuildSettings {
	bool cubemap = true;          // the capture bake (Cook --cubemap)
	bool lightmap = false;         // deprecated path (off)
	// (The legacy .sggi world-bake knobs — giBackend/giMaxProbes — are
	// REMOVED (stage 3 of the octree retirement completed the stage-2
	// retirement): old projects' settings files keep loading, their
	// giBackend=/giMaxProbes= lines hit the unknown-key skip below.)
};

BuildSettings LoadBuildSettings(const Project& p) {
	BuildSettings s;
	const fs::path path = fs::path(p.DerivedDataDir())
		/ "build-settings.txt";
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return s;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = line.substr(0, eq);
		const std::string val = line.substr(eq + 1);
		if (key == "cubemap")
			s.cubemap = val != "off";
		else if (key == "lightmap")
			s.lightmap = val == "on";
		// (giBackend/giMaxProbes skip: retired with the .sggi bake.)
	}
	return s;
}

// ---- subprocess helper ------------------------------------------------------

fs::path ExePath(const char* name) {
#ifdef _WIN32
	char buf[1024] = {};
	GetModuleFileNameA(nullptr, buf, DWORD(sizeof(buf)));
	return fs::path(buf).parent_path() / name;
#else
	char buf[4096] = {};
	const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n > 0)
		buf[n] = '\0';
	return fs::path(buf).parent_path() / name;
#endif
}

// Runs a stage EXE with an argument list. NO SHELL anywhere: Windows
// drives CreateProcessW with a properly-quoted command line (cmd.exe's
// quote-stripping rules mangle `std::system` lines that lead with a
// quoted program path), POSIX uses fork + execv with a real argv. The
// child inherits the console (stage output streams through). Returns the
// child's exit code, or -1 + err on a launch/wait failure.
int RunStage(const char* name, const std::vector<std::string>& args,
	std::string& err) {
	const std::string exe = ExePath(name).generic_string();
#ifdef _WIN32
	// Command-line quoting rule (CreateProcess): double quotes wrap
	// arguments, embedded quotes double up. No argument here carries a
	// quote character, so plain wrapping suffices.
	std::string line;
	for (const std::string& a : args) {
		if (!line.empty())
			line += ' ';
		if (a.find(' ') != std::string::npos || a.empty())
			line += "\"" + a + "\"";
		else
			line += a;
	}
	std::wstring wexe(exe.begin(), exe.end());
	// The command line INCLUDES the program as argv[0] (the CRT parses
	// GetCommandLine; omitting it shifts every stage flag by one).
	std::wstring wline = wexe;
	wline += L' ';
	std::wstring wargs(line.begin(), line.end());
	wline += wargs;
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(wexe.c_str(), wline.data(), nullptr, nullptr,
			TRUE, 0, nullptr, nullptr, &si, &pi)) {
		err = "failed to launch " + exe + " (CreateProcessW error "
			+ std::to_string(GetLastError()) + ")";
		return -1;
	}
	CloseHandle(pi.hThread);
	HANDLE job = pi.hProcess;
	const DWORD wait = WaitForSingleObject(job, INFINITE);
	DWORD code = static_cast<DWORD>(-1);
	if (wait == WAIT_FAILED) {
		err = "wait failed for " + exe;
		CloseHandle(job);
		return -1;
	}
	if (!GetExitCodeProcess(job, &code))
		err = "exit-code query failed for " + exe;
	CloseHandle(job);
	return err.empty() ? int(code) : -1;
#else
	std::vector<char*> argv;
	argv.push_back(const_cast<char*>(exe.c_str()));
	for (const std::string& a : args)
		argv.push_back(const_cast<char*>(a.c_str()));
	argv.push_back(nullptr);
	const pid_t pid = fork();
	if (pid == 0) {
		execv(exe.c_str(), argv.data());
		_exit(127);
	}
	if (pid < 0) {
		err = "fork failed for " + exe;
		return -1;
	}
	int status = 0;
	if (waitpid(pid, &status, 0) < 0) {
		err = "wait failed for " + exe;
		return -1;
	}
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	err = exe + " terminated abnormally";
	return -1;
#endif
}

} // namespace

bool BuildProject(const Project& project, const BuildOptions& options,
	BuildReport& out, std::string& err) {
	err.clear();
	out = BuildReport{};
	if (!project.Valid()) {
		err = "invalid project (open/init first)";
		return false;
	}
	if (options.outputPak.empty()) {
		err = "--output <pak> is required";
		return false;
	}
	std::error_code ec;

	// ---- 1) resolve the scene tour ---------------------------------------
	fs::path tourAbs;
	if (!options.scene.empty()) {
		tourAbs = fs::path(project.root) / fs::path(options.scene);
		if (!fs::is_regular_file(tourAbs, ec)) {
			// A relative-to-CWD escape hatch (absolute or as-given).
			tourAbs = fs::path(options.scene);
			if (!fs::is_regular_file(tourAbs, ec)) {
				err = "scene tour not found: " + options.scene;
				return false;
			}
		}
	}
	else {
		const fs::path scenes =
			fs::path(project.root) / "content" / "scenes";
		std::set<fs::path> tours;
		if (fs::is_directory(scenes, ec))
			for (const fs::directory_entry& e :
					fs::directory_iterator(scenes, ec))
				if (e.is_regular_file(ec)
						&& e.path().extension() == ".tour")
					tours.insert(e.path());
		if (tours.size() != 1) {
			std::ostringstream os;
			os << "expected exactly one scene tour under content/scenes ("
				<< tours.size() << " found";
			for (const fs::path& t : tours)
				os << "; '" << t.filename().generic_string() << "'";
			os << ") — pass --scene <tour>";
			err = os.str();
			return false;
		}
		tourAbs = *tours.begin();
	}
	out.scene = project.RelativeTo(tourAbs.generic_string());
	if (out.scene.empty())
		out.scene = tourAbs.lexically_normal().generic_string();

	// ---- 2) the tour's imported .geometry (root reference #0) ----------
	smvk::tour::Doc doc;
	std::string tLoadErr;
	std::vector<std::string> tWarn;
	if (!smvk::tour::LoadTour(tourAbs.generic_string(), doc, tLoadErr,
			&tWarn)) {
		err = "tour load failed: " + tLoadErr;
		return false;
	}
	if (doc.references.empty()) {
		err = "the scene tour has no root reference (nothing to build)";
		return false;
	}
	const std::string refSource = doc.references[0].source;
	// The reference must be PROJECT-RELATIVE (the canonical form — an
	// absolute path both escapes the project and breaks the mesh-URI
	// machinery the pak serializer reads: NormalizeAssetUri rejects
	// drive-letter absolutes, silently dropping the LOD chains +
	// meshlet registrations).
	if (fs::path(refSource).is_absolute()) {
		err = "the tour's root reference must be project-relative: '"
			+ refSource + "' (absolute references break mesh URI"
			" registration — re-point it at the project's imported"
			" .geometry)";
		return false;
	}
	const fs::path projectRoot(project.root);
	const fs::path artifactAbs = projectRoot / fs::path(refSource);
	if (!fs::is_regular_file(artifactAbs, ec)) {
		err = "the tour's referenced artifact is missing: '" + refSource
			+ "' (import the source first: Project.exe import --project "
			+ project.root + " --source <glTF>)";
		return false;
	}
	out.artifact = refSource;

	// ---- 3) import freshness (reimport stale; unchanged = no-op) --------
	// The source glTF/GLB beside the imported artifact (same stem, same
	// directory — the project's copy-in convention).
	for (const char* ext : { ".gltf", ".glb" }) {
		const fs::path srcAbs =
			fs::path(artifactAbs).replace_extension(ext);
		if (!fs::is_regular_file(srcAbs, ec))
			continue;
		if (smvk::assets::MeshArtifactStale(srcAbs,
				GeometryImportSettings{})) {
			ImportRequest req;
			req.project = project;
			// Prefer the EXTERNAL original when it still exists (external
			// edits flow in through the normal import); the project copy
			// stands alone otherwise.
			req.source = srcAbs;
			req.settings = GeometryImportSettings{}; // the stored settings
			ImportResult result;
			std::string iErr;
			if (!ImportProjectAsset(req, result, iErr)) {
				err = "reimport failed: " + iErr;
				return false;
			}
			out.reimported = result.derived;
			if (out.reimported)
				std::printf("[build] reimported: %s\n",
					result.geometry.generic_string().c_str());
		}
		break; // one family per artifact
	}

	// ---- 4) Cook (scene-dependent bakes) ---------------------------------
	const BuildSettings settings = LoadBuildSettings(project);
	{
		const std::vector<std::string> a = {
			project.root,
			"--tour", tourAbs.generic_string(),
			"--cubemap", settings.cubemap ? "on" : "off",
			"--lightmap", settings.lightmap ? "on" : "off",
		};
		const int rc = RunStage("Cook.exe", a, err);
		if (rc != 0) {
			err = err.empty()
				? ("the Cook stage failed (exit " + std::to_string(rc)
					+ ")")
				: ("the Cook stage failed: " + err);
			return false;
		}
	}

	// ---- 5) GiBaker (the authored GI volumes only) -----------------------
	{
		// The legacy .sggi world bake is RETIRED from the schedule (stage
		// 2 of tourmaline_octree_retirement_plan.md): NO --out, no
		// --backend, no --max-probes — a bare run is GI-VOLUME-ONLY (the
		// world bake is gated on the explicit --out; see GiBaker's
		// outExplicit contract). The runtime's ambient rides the baked
		// .givolume set + the sky IBL fallback (stage 1 stopped loading
		// the octree); the volume-only GiBaker pass refreshes any
		// authored/convex volumes the Cook's in-process bake did not
		// already stamp fresh. Old projects keep their existing .sggi
		// files untouched — nothing reads them at runtime anymore (the
		// pak's world-field gate no-ops without a giWorldSource record,
		// which only a --out bake ever wrote).
		const std::vector<std::string> a = {
			"--content-dir", project.root,
			"--tour", tourAbs.generic_string(),
		};
		const int rc = RunStage("GiBaker.exe", a, err);
		if (rc != 0) {
			err = err.empty()
				? ("the GiBaker stage failed (exit " + std::to_string(rc)
					+ ")")
				: ("the GiBaker stage failed: " + err);
			return false;
		}
	}

	// ---- 6) Pak (assembly + the bake-freshness gate + verify) ------------
	fs::path outPak(options.outputPak);
	if (outPak.is_relative())
		outPak = projectRoot / outPak;
	{
		std::error_code mkEc;
		fs::create_directories(outPak.parent_path(), mkEc);
		const std::vector<std::string> a = {
			project.root,
			refSource,
			"--tour", tourAbs.generic_string(),
			"--out-pak", outPak.generic_string(),
			"--verify", "on",
		};
		const int rc = RunStage("Pak.exe", a, err);
		if (rc != 0) {
			err = err.empty()
				? ("the Pak stage failed (exit " + std::to_string(rc)
					+ "; a stale bake was refused, or an asset failed"
					" — see the stage output above)")
				: ("the Pak stage failed: " + err);
			// A failed build never replaces the last valid pak: the pak
			// stage writes its output only on success (verify on), so
			// an existing output from a prior build stands.
			return false;
		}
	}
	if (!fs::is_regular_file(outPak, ec)) {
		err = "the Pak stage reported success but the output is missing: "
			+ outPak.generic_string();
		return false;
	}
	out.pak = outPak.lexically_normal().generic_string();
	return true;
}

} // namespace smvk::project
