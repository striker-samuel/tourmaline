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

namespace editor {

void EditorApp::CookToPak() {
	if (m_cookRunning || !m_hasGltfModel) {
		if (!m_hasGltfModel)
			Log("Cook: no glTF loaded.");
		return;
	}
	// Persist the command log (+ cook block) first; the cooker consumes it.
	if (!SaveTour()) {
		Log("Cook: tour save failed, aborting.");
		return;
	}
	const std::string tourPath = TourPath();
	Log("Cook: tour saved (" + std::to_string(m_historyCursor) + " commands).");
	// Split the source path into --content-dir + --in-vfs for the cooker.
	std::string contentDir, inVfs;
	{
		std::string posix = m_gltfPath;
		for (char& c : posix)
			if (c == '\\')
				c = '/';
		const std::string marker = "content/models/";
		const size_t at = posix.find(marker);
		if (at != std::string::npos) {
			contentDir = posix.substr(0, at + 8); // keep "content/"
			inVfs = posix.substr(at + 8); // "models/..."
		}
		else {
			contentDir = std::filesystem::path(m_gltfPath).parent_path().string();
			inVfs = std::filesystem::path(m_gltfPath).filename().string();
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
			std::ostringstream cmd;
			cmd << "\"" << cooker.string() << "\""
				<< " --content-dir \"" << contentDir << "\""
				<< " --in-vfs \"" << inVfs << "\""
				<< " --out-pak \"" << cook.outPak << "\""
				<< " --tour \"" << tourPath << "\""
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
