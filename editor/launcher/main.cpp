// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// THE EDITOR LAUNCHER (the editor/launcher split): a small standalone
// picker that owns PROJECT SELECTION — the recents list (the shared
// editor.prefs), a typed path for a new/existing project (scaffolding
// the Unity-style tree when the marker is missing), and the spawn:
// Editor.exe --project <dir>. The EDITOR itself never picks a project
// (the old blocking in-editor modal is retired with this split) — no
// --project, no editor.
//
// Console by design: a v1 picker (the letters-before-icons school — the
// GUI version can ride the engine's App+ImGui later without changing
// this contract). Zero engine deps: the prefs format + the project
// marker + the scaffold tree are mirrored here from editor_project.cpp
// (kProjMarker/kProjVersion + NewProjectOrOpen's dirs).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h> // readlink, fork, execv
#include <cerrno>
#include <cstring>
#endif

namespace {

namespace fs = std::filesystem;

constexpr const char* kProjMarker = "project.tourproj";
// v2: the required `guid=` identity row (mirrored from
// editor_project.cpp — the marker format is shared contract).
constexpr const char* kProjVersion = "2";

// The shared prefs path (the editor's LoadPrefs/SavePrefs contract:
// <Documents>/tourmalineEditor/editor.prefs). USERPROFILE-based (the
// redirected-Documents edge the editor's SHGetKnownFolderPath handles
// is not worth the COM plumbing here — standard setups match).
std::string PrefsPath() {
#ifdef _WIN32
	const char* home = std::getenv("USERPROFILE");
	if (!home || !*home)
		return {};
	return std::string(home) + "\\Documents\\tourmalineEditor"
		"\\editor.prefs";
#else
	// The editor's non-Windows prefs contract (editor_app.cpp): $HOME
	// + Documents/tourmalineEditor/editor.prefs.
	const char* home = std::getenv("HOME");
	if (!home || !*home)
		return {};
	return (fs::path(home) / "Documents" / "tourmalineEditor"
		/ "editor.prefs").string();
#endif
}

struct Prefs {
	std::vector<std::string> recents; // most-recent-first (the contract)
};

Prefs LoadPrefs(const std::string& path) {
	Prefs p;
	std::ifstream in(path);
	if (!in)
		return p;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		// The [projects] section's recentN= rows (index-ordered).
		if (line.rfind("recent", 0) == 0) {
			const size_t eq = line.find('=');
			if (eq != std::string::npos && eq + 1 < line.size())
				p.recents.push_back(line.substr(eq + 1));
		}
	}
	// NORMALIZE + DEDUPE the loaded rows: one project, one row — the
	// editor's convention is forward slashes (OpenProject/Normalized),
	// but a backslash variant row (an old launcher save) would otherwise
	// double-list the same project. Case-insensitive on the drive letter
	// only (Windows paths are case-preserving past that; the editor's
	// rows are canonical beyond the slash form).
	for (std::string& r : p.recents)
		std::replace(r.begin(), r.end(), '\\', '/');
	p.recents.erase(std::unique(p.recents.begin(), p.recents.end()),
		p.recents.end());
	return p;
}

void SavePrefs(const std::string& path, const Prefs& p) {
	// Rewrite: every line EXCEPT the old [projects] recentN rows pass
	// through verbatim; one fresh [projects] section (recents capped,
	// most-recent-first) replaces the old rows in place (the section
	// header itself stays where it was).
	std::ifstream in(path);
	std::vector<std::string> lines;
	bool hadSection = false;
	if (in) {
		std::string line;
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line == "[projects]")
				hadSection = true;
			if (line.rfind("recent", 0) == 0)
				continue; // the old rows die
			lines.push_back(line);
		}
	}
	if (!hadSection)
		lines.push_back("[projects]");
	std::error_code ec;
	fs::create_directories(fs::path(path).parent_path(), ec);
	std::ofstream out(path, std::ios::trunc);
	if (!out)
		return;
	for (const std::string& l : lines)
		out << l << "\n";
	for (size_t i = 0; i < p.recents.size() && i < 8; ++i)
		out << "recent" << i << "=" << p.recents[i] << "\n";
}

bool IsProjectDir(const std::string& dir) {
	std::error_code ec;
	return !dir.empty() && fs::is_regular_file(fs::path(dir) / kProjMarker, ec);
}

// A zero-dep 32-hex GUID mint (the launcher links nothing — the
// editor's smvk::tour::GenerateUuid shape, satisfied locally: 16
// random bytes from random_device, lowercase hex). Minted ONCE at
// scaffold; every later open just reads the row.
std::string MintGuid() {
	static constexpr char kHex[] = "0123456789abcdef";
	std::string guid;
	guid.reserve(32);
	std::random_device rd;
	for (int i = 0; i < 16; ++i) {
		const unsigned b = rd() & 0xFFu;
		guid.push_back(kHex[b >> 4]);
		guid.push_back(kHex[b & 0xFu]);
	}
	return guid;
}

// NewProjectOrOpen's scaffold, mirrored: the Unity-style tree + the
// marker (name + identity + version). An existing project just opens
// (true) — its identity validity is the EDITOR's gate (OpenProject).
bool ScaffoldProject(const std::string& dir) {
	std::error_code ec;
	fs::path root(dir);
	fs::create_directories(root / "content" / "models", ec);
	fs::create_directories(root / "content" / "scenes", ec);
	fs::create_directories(root / "paks", ec);
	fs::create_directories(root / "gi", ec);
	if (ec) {
		std::cerr << "cannot scaffold '" << dir << "': " << ec.message()
			<< "\n";
		return false;
	}
	if (!IsProjectDir(dir)) {
		std::ofstream out(root / kProjMarker, std::ios::binary
			| std::ios::trunc);
		if (!out) {
			std::cerr << "cannot write " << kProjMarker << " in '" << dir
				<< "'.\n";
			return false;
		}
		out << "# Tourmaline project (name + identity + format version)\n";
		out << "name=" << root.filename().string() << "\n";
		out << "guid=" << MintGuid() << "\n";
		out << "version=" << kProjVersion << "\n";
	}
	return true;
}

void PushRecent(Prefs& p, const std::string& dir) {
	// FORWARD slashes in the rows (the editor's convention — its
	// OpenProject/Normalized writes them that way; a backslash variant
	// would dedupe-miss and double-list the same project).
	std::string norm = dir;
	std::replace(norm.begin(), norm.end(), '\\', '/');
	p.recents.erase(std::remove(p.recents.begin(), p.recents.end(), norm),
		p.recents.end());
	p.recents.erase(std::remove(p.recents.begin(), p.recents.end(), dir),
		p.recents.end());
	p.recents.insert(p.recents.begin(), norm);
	if (p.recents.size() > 8)
		p.recents.resize(8);
}

#ifdef _WIN32
bool LaunchEditor(const std::string& editorExe, const std::string& project) {
	// Resolve the editor exe-relative (the launcher sits next to it in
	// the build tree / an install side by side).
	fs::path exe(fs::path(editorExe).is_absolute()
		? editorExe : fs::absolute(editorExe));
	std::string cmd = "\"" + exe.generic_string() + "\" --project \""
		+ project + "\"";
	STARTUPINFOA si{};
	PROCESS_INFORMATION pi{};
	si.cb = sizeof(si);
	if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
			0, nullptr, nullptr, &si, &pi)) {
		std::cerr << "cannot spawn '" << exe.string() << "' (error "
			<< GetLastError() << ")\n";
		return false;
	}
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return true;
}
#else
// fork + exec (the child keeps the launcher's stdio; the parent exits
// as soon as the child is running, the same detached contract as
// CreateProcessA on Windows).
bool LaunchEditor(const std::string& editorExe, const std::string& project) {
	fs::path exe(fs::path(editorExe).is_absolute()
		? fs::path(editorExe) : fs::absolute(editorExe));
	pid_t pid = fork();
	if (pid < 0) {
		std::cerr << "cannot fork to spawn '" << exe.string() << "'\n";
		return false;
	}
	if (pid == 0) {
		std::string proj = project;
		std::vector<char*> argv;
		argv.push_back(const_cast<char*>(exe.c_str()));
		argv.push_back(const_cast<char*>("--project"));
		argv.push_back(const_cast<char*>(proj.c_str()));
		argv.push_back(nullptr);
		execv(exe.c_str(), argv.data());
		std::cerr << "cannot spawn '" << exe.string() << "': "
			<< std::strerror(errno) << "\n";
		std::_Exit(1);
	}
	// Detached contract (the CreateProcessA one): the parent does NOT
	// babysit the child past exec; the launcher exits and the exec'd
	// editor is reparented to init. A failed exec is reported by the
	// child itself on the inherited stdio.
	return true;
}
#endif

std::string EditorExePath() {
#ifdef _WIN32
	char buf[MAX_PATH] = {};
	if (GetModuleFileNameA(nullptr, buf, MAX_PATH))
		return (fs::path(buf).parent_path() / "Editor.exe").string();
#else
	char buf[1024] = {};
	const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[n] = '\0';
		return (fs::path(buf).parent_path() / "Editor").string();
	}
#endif
	return "Editor.exe";
}

} // namespace

int main() {
	const std::string prefsPath = PrefsPath();
	Prefs prefs = LoadPrefs(prefsPath);

	std::cout << "Tourmaline Editor Launcher\n";
	std::cout << "--------------------------\n";
	if (prefs.recents.empty())
		std::cout << "(no recent projects)\n";
	else {
		// Live recents only (a moved/deleted project drops from the list;
		// the prefs keep the row — the editor's rescan heals it there).
		std::cout << "Recent projects:\n";
		int shown = 0;
		for (size_t i = 0; i < prefs.recents.size(); ++i) {
			if (!IsProjectDir(prefs.recents[i]))
				continue;
			std::cout << "  " << (++shown) << ") "
				<< prefs.recents[i] << "\n";
		}
	}
	std::cout << "Pick a number, or type a project path to open/create:\n> ";

	std::string input;
	if (!std::getline(std::cin, input) || input.empty())
		return 0;

	std::string project;
	if (std::all_of(input.begin(), input.end(), ::isdigit)
			&& !input.empty()) {
		const int idx = std::atoi(input.c_str());
		int live = 0;
		for (const std::string& r : prefs.recents) {
			if (!IsProjectDir(r))
				continue;
			if (++live == idx) {
				project = r;
				break;
			}
		}
		if (project.empty()) {
			std::cerr << "no recent project #" << idx << "\n";
			return 1;
		}
	}
	else {
		// A typed path: trim quotes/space, normalize slashes.
		project = input;
		if (project.size() >= 2 && project.front() == '"'
				&& project.back() == '"')
			project = project.substr(1, project.size() - 2);
		std::replace(project.begin(), project.end(), '/', '\\');
		if (!ScaffoldProject(project)) {
			std::cerr << "project '" << project << "' failed to create.\n";
			return 1;
		}
	}

	PushRecent(prefs, project);
	SavePrefs(prefsPath, prefs);

	std::cout << "Launching editor: " << project << "\n";
	return LaunchEditor(EditorExePath(), project) ? 0 : 1;
}
