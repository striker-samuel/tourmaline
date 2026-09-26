// Cook flow: save the .tour command log, then run PakCooker --tour on a
// worker thread (full bake support; the cooker replays the tour onto a
// fresh import — deterministic source + tour in, pak out).

#include "editor_app.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <Engine/Asset/tour.h>
#include <Engine/Vfs/vfs.h>

namespace editor {
namespace {
// cmd.exe (behind _popen) mangles commands that start with a quote:
// a quoted exe path plus quoted args gets its quotes stripped and the
// whole line is then read as the program name ("...' is not recognized").
// Quote only args that actually contain whitespace/quotes.
std::string QuoteArg(const std::string& s) {
	if (s.find_first_of(" \t\"") == std::string::npos)
		return s;
	std::string o = "\"";
	for (char c : s)
		o += (c == '"') ? "\\\"" : std::string(1, c);
	return o + "\"";
}
} // namespace

void EditorApp::CookToPak() {
	if (m_cookRunning || !m_hasGltfModel) {
		if (!m_hasGltfModel)
			Log(!m_pakPath.empty()
				? "Cook: pak scenes are read-only (open the source glTF to cook)."
				: "Cook: no glTF loaded.");
		return;
	}
	// Build a TEMP coalesced tour for cooking (don't overwrite the canonical .tour).
	// The cooker will consume this temp tour; the live .tour stays untouched.
	std::string tempTourPath;
	{
		namespace fs = std::filesystem;
		fs::path tmp = fs::temp_directory_path() /
			("tourmaline_cook_" + fs::path(m_gltfPath).stem().string() + ".tour");
		std::error_code ec;
		if (!SaveTourTo(tmp.generic_string())) {
			Log("Cook: temp tour save failed, aborting.");
			return;
		}
		tempTourPath = tmp.generic_string();
		Log("Cook: temp tour saved '" + tempTourPath + "' (" +
			std::to_string(m_historyCursor) + " commands).");
	}
	// contentDir = absolute path to the content root (parent of "content/").
	// For F:\content\models\... -> F:\
	// For //flintbase/ -> not used (flintbase handles its own resolution).
	std::string contentDir, inVfs;
	{
		std::string posix = m_gltfPath;
		for (char& c : posix)
			if (c == '\\')
				c = '/';
		if (smvk::vfs::HasFlintbasePrefix(posix)) {
			// Flintbase paths are resolved by the cooker's own VFS.
			contentDir = std::filesystem::absolute("content").string();
			inVfs = posix;
		}
		else {
			const std::string marker = "content/";
			const size_t at = posix.find(marker);
			if (at != std::string::npos) {
				// contentDir = parent of "content/" so VFS mount works.
				std::string parentPath = (at > 0) ? posix.substr(0, at) : ".";
				contentDir = std::filesystem::absolute(parentPath).string();
				inVfs = posix.substr(at); // "content/models/..."
			}
			else {
				// Fallback: glTF host path's parent directory.
				contentDir = std::filesystem::absolute(std::filesystem::path(m_gltfHostPath).parent_path()).string();
				inVfs = std::filesystem::path(m_gltfHostPath).filename().string();
			}
		}
	}
	CookSettings cook = m_cook;
	cook.contentDir = contentDir;
	cook.inVfs = inVfs;
	// Does the definition carry authored convex volumes? (Drives the
	// GiBaker stage below — skipped silently when nothing to bake.)
	const bool giHasCvols = m_world
		&& !m_world->scene.cvolForNode.empty();
	// Auto-generate output path if empty: content/paks/<stem>.pak
	if (cook.outPak.empty() && !m_gltfPath.empty()) {
		std::filesystem::path p(m_gltfPath);
		cook.outPak = ("content/paks/" + p.stem().string() + ".pak");
	}
	// contentRoot = the "content/" dir itself (gi/texture outputs land
	// here — exactly what the editor/App loaders check next to the
	// pak); the VFS base (contentDir) is its PARENT for the mount.
	const std::string contentRoot = std::filesystem::absolute(
		std::filesystem::path(cook.outPak).parent_path() / "..")
		.lexically_normal().string();
	m_cookRunning = true;
	Log("Cook started -> " + cook.outPak);
	m_cookFuture = std::async(std::launch::async,
		[cook, giHasCvols, contentRoot, tempTourPath = std::move(tempTourPath), contentDir = std::move(contentDir), inVfs = std::move(inVfs)]() -> CookResult {
			CookResult r{};
			std::ostringstream log;
			// PakCooker next to Editor.exe.
			char exePath[1024] = {};
#ifdef _WIN32
			DWORD n = GetModuleFileNameA(nullptr, exePath, (DWORD)sizeof(exePath));
			(void)n;
#endif
			std::filesystem::path cooker = std::filesystem::path(exePath).parent_path() /
				"PakCooker.exe";
			if (!std::filesystem::exists(cooker))
				cooker = "PakCooker.exe"; // PATH fallback
// Required paths + tour on the CLI; everything else rides the
// tour's cook block (CLI wins when both are given).
// If we have a tour loaded, use it as the PRIMARY input (PakCooker
// resolves its 'source' field as --in-vfs). Else fall back to --in-vfs.
std::ostringstream cmd;
cmd << QuoteArg(cooker.string())
	<< " --content-dir " << QuoteArg(contentDir);
if (!tempTourPath.empty()) {
	cmd << " --tour " << QuoteArg(tempTourPath);
} else {
	cmd << " --in-vfs " << QuoteArg(inVfs);
}
cmd << " --out-pak " << QuoteArg(cook.outPak)
	<< " 2>&1";
			log << "cook: " << cmd.str() << "\n";
#ifdef _WIN32
			FILE* pipe = _popen(cmd.str().c_str(), "r");
#else
			FILE* pipe = popen(cmd.str().c_str(), "r");
#endif
			if (!pipe) {
				r.log = log.str() + "cook: failed to spawn PakCooker\n";
				return r;
			}
			std::array<char, 512> buf{};
			while (std::fgets(buf.data(), int(buf.size()), pipe))
				log << buf.data();
#ifdef _WIN32
			const int rc = _pclose(pipe);
#else
			const int rc = pclose(pipe);
#endif
			log << "cook: PakCooker exit=" << rc << "\n";
			r.ok = (rc == 0);
			// GI BAKE STAGE: the definition's GI bakes as part of the
			// cook — GiBaker consumes the SAME definition tour (root
			// reference + nodes), keyed to the PAK STEM so the
			// sidecars land exactly where the editor/App loaders look
			// next to the pak: gi/<stem>_*.cvol + .cvmanifest, and
			// gi/<stem>.sggi for the world field. Skipped silently
			// when nothing is requested.
			const bool wantCvols = cook.giCvol && giHasCvols;
			const bool wantWorld = cook.giWorld;
			if (r.ok && (wantCvols || wantWorld)) {
				std::filesystem::path baker =
					std::filesystem::path(exePath).parent_path() /
					"GiBaker.exe";
				if (!std::filesystem::exists(baker))
					baker = "GiBaker.exe";
				const std::string pakStem =
					std::filesystem::path(cook.outPak).stem().string();
				std::ostringstream gi;
				gi << QuoteArg(baker.string())
					<< " --content-dir " << QuoteArg(contentRoot)
					<< " --scene " << QuoteArg(pakStem)
					<< " --tour " << QuoteArg(tempTourPath);
				if (wantWorld)
					gi << " --out " << QuoteArg(
						(std::filesystem::path(contentRoot) / "gi" /
							(pakStem + ".sggi")).generic_string());
				log << "cook-gi: " << gi.str() << "\n";
#ifdef _WIN32
				FILE* gpipe = _popen(gi.str().c_str(), "r");
#else
				FILE* gpipe = popen(gi.str().c_str(), "r");
#endif
				if (!gpipe) {
					log << "cook-gi: failed to spawn GiBaker\n";
				}
				else {
					while (std::fgets(buf.data(), int(buf.size()), gpipe))
						log << buf.data();
#ifdef _WIN32
					const int grc = _pclose(gpipe);
#else
					const int grc = pclose(gpipe);
#endif
					log << "cook-gi: GiBaker exit=" << grc << "\n";
					// The pak is the cook's product; a GI bake failure
					// degrades lighting (not the pak) — reported, not
					// fatal.
					if (grc != 0)
						log << "cook-gi: GI bake FAILED (pak is OK; "
							"see log above)\n";
				}
			}
			r.log = log.str();
			// Cleanup temp tour file.
			std::error_code ec;
			std::filesystem::remove(tempTourPath, ec);
			return r;
		});
}

std::string EditorApp::RunArgs() const {
	// App entrypoints are files: the loaded pak verbatim, else the glTF
	// source imported directly. Empty = nothing runnable loaded.
	if (!m_pakPath.empty())
		return "--pak " + QuoteArg(m_pakPath);
	if (m_hasGltfModel && !m_gltfHostPath.empty())
		return "--gltf " + QuoteArg(m_gltfHostPath);
	return {};
}

void EditorApp::RunInApp() {
	if (m_runRunning)
		return;
	const std::string runArgs = RunArgs();
	if (runArgs.empty()) {
		Log("Run: nothing runnable loaded (open a .pak or .gltf first).");
		return;
	}
	// Persist the tour first so no edit is lost (App itself runs the
	// cooked/source scene, not the tour — Cook first to play edits).
	if (m_hasGltfModel)
		SaveTour();
	char exePath[1024] = {};
#ifdef _WIN32
	DWORD n = GetModuleFileNameA(nullptr, exePath, (DWORD)sizeof(exePath));
	(void)n;
#endif
	std::filesystem::path app = std::filesystem::path(exePath).parent_path() /
		"App.exe";
	if (!std::filesystem::exists(app))
		app = "App.exe"; // PATH fallback
	m_runRunning = true;
	Log("Run started: App " + runArgs +
		" (edits need Cook first; close App to finish).");
	m_runFuture = std::async(std::launch::async,
		[app = app.string(), runArgs]() -> CookResult {
			CookResult r{};
			std::ostringstream log;
			std::ostringstream cmd;
			cmd << QuoteArg(app) << " " << runArgs << " 2>&1";
			log << "run: " << cmd.str() << "\n";
#ifdef _WIN32
			FILE* pipe = _popen(cmd.str().c_str(), "r");
#else
			FILE* pipe = popen(cmd.str().c_str(), "r");
#endif
			if (!pipe) {
				r.log = log.str() + "run: failed to spawn App\n";
				return r;
			}
			std::array<char, 512> buf{};
			while (std::fgets(buf.data(), int(buf.size()), pipe))
				log << buf.data();
#ifdef _WIN32
			const int rc = _pclose(pipe);
#else
			const int rc = pclose(pipe);
#endif
			log << "run: App exit=" << rc << "\n";
			r.ok = (rc == 0);
			r.log = log.str();
			return r;
		});
}

void EditorApp::PollRunProcess() {
	if (!m_runRunning)
		return;
	if (!m_runFuture.valid())
		return;
	if (m_runFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return;
	CookResult r = m_runFuture.get();
	m_runRunning = false;
	std::istringstream ss(r.log);
	std::string line;
	while (std::getline(ss, line))
		Log(line);
	Log(r.ok ? "Run finished." : "Run exited with errors (see log above).");
}

void EditorApp::PollCookProcess() {
	if (!m_cookRunning)
		return;
	if (!m_cookFuture.valid())
		return;
	if (m_cookFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return;
	CookResult r = m_cookFuture.get();
	m_cookRunning = false;
	// Fan the captured log out line by line.
	std::istringstream ss(r.log);
	std::string line;
	while (std::getline(ss, line))
		Log(line);
	Log(r.ok ? "Cook finished OK." : "Cook FAILED (see log above).");
	// The cook dialog shows its own progress; close it on finish
	// (DrawCookDialog consumes the flag inside the begin block).
	m_cookJustFinished = true;
}

} // namespace editor
