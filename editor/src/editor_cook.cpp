// Cook flow: save the .tour command log, then run PakCooker --tour on a
// worker thread (full bake support; the cooker replays the tour onto a
// fresh import — deterministic source + tour in, pak out).

#include "editor_app.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

// Shared snapshot for the cooker/baker subprocesses: a TEMP coalesced
// tour (the canonical .tour is never overwritten; becomeTarget = false
// keeps the live target, dirty star and autosave untouched) plus the
// definition fingerprint (tour bytes minus the volatile "//Last
// modified:" line — deterministic per definition content).
bool EditorApp::SnapshotTourForProcess(const char* tag,
	std::string& outTempTour, std::string& outDefHash) {
	outTempTour.clear();
	outDefHash.clear();
	namespace fs = std::filesystem;
	fs::path tmp = fs::temp_directory_path() /
		("tourmaline_" + std::string(tag) + "_"
			+ fs::path(m_gltfPath).stem().string() + ".tour");
	if (!SaveTourTo(tmp.generic_string(), false)) {
		Log(std::string(tag) + ": temp tour save failed, aborting.");
		return false;
	}
	outTempTour = tmp.generic_string();
	Log(std::string(tag) + ": temp tour saved '" + outTempTour + "' ("
		+ std::to_string(m_historyCursor) + " commands).");
	std::ifstream tf(outTempTour, std::ios::binary);
	if (tf) {
		std::string text((std::istreambuf_iterator<char>(tf)),
			std::istreambuf_iterator<char>());
		std::string scan = text;
		const size_t lineAt = scan.find("//Last modified:");
		if (lineAt != std::string::npos) {
			const size_t lineEnd = scan.find('\n', lineAt);
			if (lineEnd != std::string::npos)
				scan.erase(lineAt, lineEnd - lineAt + 1);
			else
				scan.erase(lineAt);
		}
		outDefHash = smvk::cache::HashBytes(scan.data(), scan.size());
	}
	return true;
}

void EditorApp::CookToPak() {
	if (m_cookRunning || !m_hasGltfModel) {
		if (!m_hasGltfModel)
			Log(!m_pakPath.empty()
				? "Cook: pak scenes are read-only (open the source glTF to cook)."
				: "Cook: no glTF loaded.");
		return;
	}
	// TEMP coalesced tour snapshot (shared with Bake GI below).
	std::string tempTourPath, defHash;
	if (!SnapshotTourForProcess("Cook", tempTourPath, defHash))
		return;
	// Definition fingerprint: the scene artifact's cache source key.
	m_cookDefHash = defHash;
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
		// No trailing separators (but keep filesystem roots like
		// "F:\" intact): a trailing backslash would escape the closing
		// quote when this rides the cooker command line (..."F:\proj\"
		// is a broken argument).
		while (contentDir.size() > 3 && (contentDir.back() == '\\'
			|| contentDir.back() == '/'))
			contentDir.pop_back();
	}
	CookSettings cook = m_cook;
	cook.contentDir = contentDir;
	cook.inVfs = inVfs;
	// NOTE: no GI stage here by design (on-demand Bake GI only) —
	// cooks stay fast and GI-free; unbaked scenes render sky IBL.
	// Auto-generate output path if empty: content/paks/<stem>.pak
	if (cook.outPak.empty() && !m_gltfPath.empty()) {
		std::filesystem::path p(m_gltfPath);
		cook.outPak = ("content/paks/" + p.stem().string() + ".pak");
	}
	m_cookRunning = true;
	Log("Cook started -> " + cook.outPak);
	auto feed = std::make_shared<CookFeed>();
	m_cookFeed = feed;
	// Cache acceleration (opt-in, never required): when the scratch is
	// online, the cooker consults + feeds the same artifact registry —
	// hit = hash-validated copy from scratch, miss = transcode + stage.
	// The EFFECTIVE store root (project-owned when a project is open),
	// never the global pref. Captured by value (the worker lambda has
	// no this).
	const std::string scratchDir =
		m_store ? m_store->Root() : std::string();
	m_cookFuture = std::async(std::launch::async,
		[cook, feed, scratchDir, tempTourPath = std::move(tempTourPath), contentDir = std::move(contentDir), inVfs = std::move(inVfs)]() -> CookResult {
			CookResult r{};
			// Live feed: each pipe line is pushed to the shared feed
			// (mutex-guarded); PollCookProcess drains it on the main
			// thread so the Console shows progress while the cook
			// runs. r.log carries only the tail (exit codes) since
			// the body was already streamed.
			auto push = [feed](const std::string& line) {
				std::lock_guard<std::mutex> lk(feed->mutex);
				feed->lines.push_back(line);
			};
			auto pushPipe = [&push](FILE* pipe) {
				std::array<char, 512> buf{};
				while (std::fgets(buf.data(), int(buf.size()), pipe)) {
					std::string line(buf.data());
					while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
						line.pop_back();
					push(line);
				}
			};
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
cmd << " --out-pak " << QuoteArg(cook.outPak);
// Cache acceleration flag (captured by value above; empty = off).
if (!scratchDir.empty())
	cmd << " --scratch-dir " << QuoteArg(scratchDir);
cmd << " 2>&1";
			push("cook: " + cmd.str());
#ifdef _WIN32
			FILE* pipe = _popen(cmd.str().c_str(), "r");
#else
			FILE* pipe = popen(cmd.str().c_str(), "r");
#endif
			if (!pipe) {
				r.log = "cook: failed to spawn PakCooker\n";
				return r;
			}
			pushPipe(pipe);
#ifdef _WIN32
			const int rc = _pclose(pipe);
#else
			const int rc = pclose(pipe);
#endif
			r.ok = (rc == 0);
			r.log = "cook: PakCooker exit=" + std::to_string(rc) + "\n";
			// Cleanup temp tour file.
			std::error_code ec;
			std::filesystem::remove(tempTourPath, ec);
			return r;
		});
}

void EditorApp::PollCookProcess() {
	if (!m_cookRunning)
		return;
	// Drain the live pipe feed every frame so the Console shows
	// progress while the cook runs (stuck vs. working is visible).
	auto drainFeed = [this]() {
		std::shared_ptr<CookFeed> feed = m_cookFeed;
		if (!feed)
			return;
		std::vector<std::string> ready;
		{
			std::lock_guard<std::mutex> lk(feed->mutex);
			ready.swap(feed->lines);
		}
		for (auto& line : ready)
			Log(line);
	};
	drainFeed();
	if (!m_cookFuture.valid())
		return;
	if (m_cookFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return;
	CookResult r = m_cookFuture.get();
	m_cookRunning = false;
	// Late lines that arrived between the drain above and completion.
	drainFeed();
	// Tail only (body was already streamed live).
	std::istringstream ss(r.log);
	std::string line;
	while (std::getline(ss, line)) {
		if (!line.empty())
			Log(line);
	}
	Log(r.ok ? "Cook finished OK." : "Cook FAILED (see log above).");
	// The cook is the PRODUCT step (a pak for the game/App — Run in
	// App loads it): it never touches the viewport. The LIVE view
	// (the hosted renderer over the editor's own world) is unaffected.
	// The cook dialog shows its own progress; close it on finish
	// (DrawCookDialog consumes the flag inside the begin block).
	m_cookJustFinished = true;
}

// ---- Bake GI on demand (never part of the cook) --------------------
// GiBaker consumes the SAME definition-tour snapshot a cook would
// (root reference + nodes), keyed to the cook's OUT-PAK STEM so the
// sidecars land exactly where the editor/App loaders look:
// gi/<stem>_*.cvol + .cvmanifest, and gi/<stem>.sggi for the world
// field. Missing sidecars are NOT an error anywhere down the line —
// every GI/volume/cubemap/lightmap loader falls back to the analytic
// sky IBL (see DegradeAmbientForMissingSources in app).

void EditorApp::BakeGI() {
	if (m_bakeRunning)
		return;
	if (m_cookRunning) {
		Log("Bake GI: a cook is running — wait for it to finish.");
		return;
	}
	if (!m_world || !m_hasGltfModel) {
		Log("Bake GI: open a source scene first (imports land ready to "
			"cook; cooking is the separate product step).");
		return;
	}
	// What to bake comes from the same flags the cook dialog shows.
	const bool giHasCvols = !m_world->scene.cvolForNode.empty();
	const bool wantCvols = m_cook.giCvol && giHasCvols;
	const bool wantWorld = m_cook.giWorld;
	if (!wantCvols && !wantWorld) {
		Log("Bake GI: nothing requested — enable World field and/or "
			"Convex volumes in the cook dialog first.");
		return;
	}
	// Sidecars key to the cook target's stem; derive the project (or
	// legacy) default when the out pak was never set.
	if (m_cook.outPak.empty() && !m_gltfPath.empty()) {
		const std::string stem =
			std::filesystem::path(m_gltfPath).stem().string();
		std::string def = ProjectCookOutPak(stem);
		if (def.empty())
			def = ("content/paks/" + stem + ".pak");
		m_cook.outPak = def;
		std::snprintf(m_cookOutPakBuf, sizeof(m_cookOutPakBuf), "%s",
			def.c_str());
	}
	if (m_cook.outPak.empty()) {
		Log("Bake GI: no cook target — cook once first (Ctrl+Shift+C).");
		return;
	}
	std::string tempTourPath, defHash;
	if (!SnapshotTourForProcess("BakeGI", tempTourPath, defHash))
		return;
	const std::string pakStem =
		std::filesystem::path(m_cook.outPak).stem().string();
	// contentRoot = the "content/" dir itself (sidecars land in gi/
	// next to the pak — exactly what the loaders check).
	const std::string contentRoot = std::filesystem::absolute(
		std::filesystem::path(m_cook.outPak).parent_path() / "..")
		.lexically_normal().string();
	m_giBakePendingHash = defHash; // adopted as m_giBakedHash on success
	m_bakeRunning = true;
	Log("Bake GI started ("
		+ std::string(wantWorld ? "world field" : "")
		+ std::string((wantWorld && wantCvols) ? " + " : "")
		+ std::string(wantCvols ? "convex volumes" : "") + ") -> "
		+ contentRoot + "/gi/" + pakStem + ".*");
	auto feed = std::make_shared<CookFeed>();
	m_bakeFeed = feed;
	m_bakeFuture = std::async(std::launch::async,
		[pakStem, contentRoot, wantWorld, feed, tempTourPath = std::move(tempTourPath)]() -> CookResult {
			CookResult r{};
			auto push = [feed](const std::string& line) {
				std::lock_guard<std::mutex> lk(feed->mutex);
				feed->lines.push_back(line);
			};
			auto pushPipe = [&push](FILE* pipe) {
				std::array<char, 512> buf{};
				while (std::fgets(buf.data(), int(buf.size()), pipe)) {
					std::string line(buf.data());
					while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
						line.pop_back();
					push(line);
				}
			};
			// GiBaker next to Editor.exe.
			char exePath[1024] = {};
#ifdef _WIN32
			DWORD n = GetModuleFileNameA(nullptr, exePath, (DWORD)sizeof(exePath));
			(void)n;
#endif
			std::filesystem::path baker =
				std::filesystem::path(exePath).parent_path() /
				"GiBaker.exe";
			if (!std::filesystem::exists(baker))
				baker = "GiBaker.exe"; // PATH fallback
			std::ostringstream gi;
			gi << QuoteArg(baker.string())
				<< " --content-dir " << QuoteArg(contentRoot)
				<< " --scene " << QuoteArg(pakStem)
				<< " --tour " << QuoteArg(tempTourPath);
			if (wantWorld)
				gi << " --out " << QuoteArg(
					(std::filesystem::path(contentRoot) / "gi" /
						(pakStem + ".sggi")).generic_string());
			gi << " 2>&1";
			push("bake-gi: " + gi.str());
#ifdef _WIN32
			FILE* gpipe = _popen(gi.str().c_str(), "r");
#else
			FILE* gpipe = popen(gi.str().c_str(), "r");
#endif
			if (!gpipe) {
				r.log = "bake-gi: failed to spawn GiBaker\n";
				return r;
			}
			pushPipe(gpipe);
#ifdef _WIN32
			const int grc = _pclose(gpipe);
#else
			const int grc = pclose(gpipe);
#endif
			r.ok = (grc == 0);
			r.log = "bake-gi: GiBaker exit=" + std::to_string(grc) + "\n";
			if (!r.ok)
				r.log += "bake-gi: GI bake FAILED (previous sidecars, "
					"if any, are untouched; unbaked scenes render sky "
					"IBL)\n";
			std::error_code ec;
			std::filesystem::remove(tempTourPath, ec);
			return r;
		});
}

void EditorApp::PollBakeProcess() {
	if (!m_bakeRunning)
		return;
	// Drain the live pipe feed every frame (same pattern as the cook).
	auto drainFeed = [this]() {
		std::shared_ptr<CookFeed> feed = m_bakeFeed;
		if (!feed)
			return;
		std::vector<std::string> ready;
		{
			std::lock_guard<std::mutex> lk(feed->mutex);
			ready.swap(feed->lines);
		}
		for (auto& line : ready)
			Log(line);
	};
	drainFeed();
	if (!m_bakeFuture.valid())
		return;
	if (m_bakeFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return;
	CookResult r = m_bakeFuture.get();
	m_bakeRunning = false;
	drainFeed();
	std::istringstream ss(r.log);
	std::string line;
	while (std::getline(ss, line)) {
		if (!line.empty())
			Log(line);
	}
	if (r.ok) {
		// The sidecars now match the baked definition: record it so a
		// later cook can warn when they go stale.
		m_giBakedHash = m_giBakePendingHash;
		Log("Bake GI finished OK — refreshing the hosted view.");
		RefreshWysiwygPreview();
	}
	else {
		Log("Bake GI FAILED (see log above).");
	}
	m_giBakePendingHash.clear();
}

} // namespace editor
