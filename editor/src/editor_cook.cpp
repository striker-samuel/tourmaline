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
	// Persist the command log (+ cook block) first; the cooker consumes it.
	// SaveTour targets the live path (save-as aware); the cooker replays
	// that same file, so a versioned .000N.tour cooks exactly it.
	if (!SaveTour()) {
		Log("Cook: tour save failed, aborting.");
		return;
	}
	const std::string tourPath = TourTarget();
	Log("Cook: tour saved '" + tourPath + "' (" +
		std::to_string(m_historyCursor) + " commands).");
	// Split the source path into --content-dir + --in-vfs for the cooker.
	std::string contentDir, inVfs;
	{
		std::string posix = m_gltfPath;
		for (char& c : posix)
			if (c == '\\')
				c = '/';
		if (smvk::vfs::HasFlintbasePrefix(posix)) {
			// Scheme sources route through the cooker's Flintbase VFS
			// mount; --content-dir is irrelevant for them.
			contentDir = "content";
			inVfs = posix;
		}
		else {
			const std::string marker = "content/models/";
			const size_t at = posix.find(marker);
			if (at != std::string::npos) {
				contentDir = posix.substr(0, at + 8); // keep "content/"
				inVfs = posix.substr(at + 8); // "models/..."
			}
			else {
				contentDir = std::filesystem::path(m_gltfHostPath).parent_path().string();
				inVfs = std::filesystem::path(m_gltfHostPath).filename().string();
			}
		}
	}
	CookSettings cook = m_cook;
	m_cookRunning = true;
	Log("Cook started -> " + cook.outPak);
	m_cookFuture = std::async(std::launch::async,
		[cook, contentDir, inVfs, tourPath]() -> CookResult {
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
			// Args go through QuoteArg (blanket quoting breaks cmd.exe).
			std::ostringstream cmd;
			cmd << QuoteArg(cooker.string())
				<< " --content-dir " << QuoteArg(contentDir)
				<< " --in-vfs " << QuoteArg(inVfs)
				<< " --out-pak " << QuoteArg(cook.outPak)
				<< " --tour " << QuoteArg(tourPath)
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
			r.log = log.str();
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
}

} // namespace editor
