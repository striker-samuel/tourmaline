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

#include <Engine/Asset/bake_asset.h>
#include <Engine/Asset/tour.h>
#include <Engine/Renderer/cubemap.h>
#include <Engine/Vfs/vfs.h>

#include <fstream>
#include <optional>

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
	if (!SaveTourTo(tmp.generic_string(), false, true)) {
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

void EditorApp::CookCubemapsNow() {
	// On-demand cubemap refresh: cubemap-only quick bake (skips the
	// minutes-long GiBaker stage). Session-adopts cubemap baking like
	// the cook dialog checkbox (the completion adopter keys on it).
	if (!m_cook.cubemap) {
		m_cook.cubemap = true;
		AutosaveTour();
	}
	CookToPak(true, true);
}

void EditorApp::CookToPak(bool quickBakes, bool cubemapsOnly) {
	// Definition scenes only — with pak opens gone (paks run in the App),
	// every open editor scene is definition-backed; the gate left is
	// content (empty sourceless worlds have nothing to cook — the cooker
	// replays the tour's asset-UUID references itself).
	if (m_cookRunning || SceneEmpty()) {
		if (SceneEmpty())
			Log("Cook: no scene content loaded.");
		return;
	}
	if (m_bakeRunning) {
		Log("Cook: a Bake GI is running — wait for it to finish (the GI "
			"sidecars it writes are the ones this cook embeds).");
		return;
	}
	// TEMP coalesced tour snapshot (shared with Bake GI below).
	std::string tempTourPath, defHash;
	if (!SnapshotTourForProcess("Cook", tempTourPath, defHash))
		return;
	// Definition fingerprint: the scene artifact's cache source key.
	m_cookDefHash = defHash;
	// contentDir = absolute path to the content root (parent of
	// "content/"): for F:\content\models\... it is the drive root.
	// THE CANONICAL HOST PATH drives the marker walk (m_gltfHostPath —
	// the file that actually loaded, GUID-resolved on tour opens): the
	// recorded identity (m_gltfPath) can be a project-RELATIVE
	// "content/models/..." on tour-opened scenes, whose "." parent then
	// resolved against the EDITOR CWD (the build dir) — every bake
	// sidecar the cook stages (gi/*.sggi + *.cvol, textures/cubemaps/
	// <base>[_N].ktx2 + the .cubemanifest) landed next to the exe
	// instead of the project (and the project-relative resolution below
	// never saw them (the "cubemap probe preview not drawn in the
	// editor" report — the probes bake fine, they just surface in the
	// wrong tree). A RELATIVE marker (at 0) resolves against the
	// project root when one is open (the tour records project-space
	// sources; the project owns the content tree), else the CWD like
	// before.
	std::string contentDir, inVfs;
	{
		const std::string identity = !m_gltfHostPath.empty()
			? m_gltfHostPath : m_gltfPath;
		std::string posix = identity;
		for (char& c : posix)
			if (c == '\\')
				c = '/';
		{
			const std::string marker = "content/";
			const size_t at = posix.find(marker);
			if (at != std::string::npos && !identity.empty()) {
				// contentDir = parent of "content/" so VFS mount works.
				std::string parentPath = (at > 0) ? posix.substr(0, at)
					: (m_projectRoot.empty() ? std::string(".")
						: m_projectRoot);
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
	// Auto-generate the output path if empty (session state — also the
	// destination the completion handler + Bake GI derive the sidecar
	// gi/ dir from, so it must be resolved on the MEMBER, not the local
	// copy): the project paks dir when a project is open, else
	// content/paks/<stem>.pak relative to the editor.
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
	cook.outPak = m_cook.outPak;
	// FULL GI bake FROM 0 rides the cook (the pak carries its own GI):
	// GiBaker first — the world-field octree (m_cook.giWorld) + the
	// tour's authored convex volumes, fresh, nothing reused — then
	// PakCooker discovers the fresh sidecars, EMBEDS them into the pak
	// and registers the guid bindings. The completion handler records
	// the bake assets + the tour's bake references (see
	// RecordBakeAssetsFromCook). This is minutes-long on real scenes
	// (the world field) — the on-demand Bake GI remains the quick path.
	const std::string pakStem =
		std::filesystem::path(cook.outPak).stem().string();
	const bool wantWorld = m_cook.giWorld;
	m_cookRunning = true;
	// A new cook clears the last result line (the panel shows it until
	// then) and the face strip (captures refill it as they land).
	m_cookJustFinished = false;
	DropCubeFaces();
	m_cubeTotal = -1;
	m_cubeDone = 0;
	m_cubeRunActive = true;
	// Blocking progress surface (modal task window): the stage flips to
	// the pak half when the drain below sees the cooker stage start.
	TaskWindowBegin("Cook to .pak", cubemapsOnly
		? "Updating cubemaps (capture-only refresh, no pak write)..."
		: quickBakes
		? "Updating bakes (GI + captures, no pak write)..."
		: "Cook stage 1/2: GI bake...");
	// Label honesty: the cook UPDATES the scene's bake in place — GiBaker
	// re-runs it at the stable stem paths (overwrites; RecordBakeAssets-
	// FromCook overwrites the bake guid reference in the current scene's
	// tour and deletes the superseded old bake), then the pak follows.
	// Quick bakes: the same GiBaker stage, then PakCooker --quick-bakes
	// (capture-only refresh — the loose sidecars are the output; no pak
	// is written).
	Log(std::string(quickBakes ? "Update bakes started -> " : "Cook started -> ")
		+ cook.outPak
		+ (cubemapsOnly
			? std::string(" (cubemap captures refreshed, GiBaker skipped; no pak write)")
			: quickBakes
			? std::string(" (GI bake updated in place + cubemap captures "
				"refreshed; no pak write)")
			: std::string(wantWorld
				? " (GI bake updated in place: world field + convex "
					"volumes, then the pak)"
				: " (GI bake updated in place: convex volumes, then the pak)")));
	auto feed = std::make_shared<CookFeed>();
	m_cookFeed = feed;
	// Cache acceleration (opt-in, never required): when the project
	// artifact cache is online, the cooker consults + feeds the same
	// registry — hit = hash-validated copy, miss = transcode + stage.
	// The EFFECTIVE store root (project-owned; empty = cache off).
	// Captured by value (the worker lambda has no this).
	const std::string scratchDir =
		m_store ? m_store->Root() : std::string();
	m_cookFuture = std::async(std::launch::async,
		[cook, feed, scratchDir, pakStem, wantWorld, quickBakes, cubemapsOnly, tempTourPath = std::move(tempTourPath), contentDir = std::move(contentDir), inVfs = std::move(inVfs)]() -> CookResult {
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
			// GiBaker next to Editor.exe.
			char exePath[1024] = {};
#ifdef _WIN32
			DWORD n = GetModuleFileNameA(nullptr, exePath, (DWORD)sizeof(exePath));
			(void)n;
#endif
		// ---- stage 1: the scene's GI bake re-run IN PLACE (GiBaker —
		// the world field when requested + the tour's authored convex
		// volumes; the definition tour is the only input, nothing
		// reused; the STABLE stem paths overwrite, and the completion
		// handler deletes the superseded old bake when the stem
		// changed). The fresh sidecars land under <contentDir>/gi/
		// exactly where stage 2 (PakCooker) discovers + embeds them
		// into the pak. A failed GI bake FAILS the cook (the pak would
		// silently miss its GI otherwise — the exact bug this flow
		// exists to prevent). SKIPPED for cubemap-only refreshes.
		if (!cubemapsOnly)
		{
			{
				std::filesystem::path baker =
					std::filesystem::path(exePath).parent_path() /
					"GiBaker.exe";
				if (!std::filesystem::exists(baker))
					baker = "GiBaker.exe"; // PATH fallback
				std::ostringstream gi;
				gi << QuoteArg(baker.string())
					<< " --content-dir " << QuoteArg(contentDir)
					<< " --scene " << QuoteArg(pakStem)
					<< " --tour " << QuoteArg(tempTourPath);
				// Temp-file bake (same as BakeGI): publish atomically so
				// a failed baker never corrupts the live world field.
				std::string cookFinal, cookTemp;
				if (wantWorld) {
					cookFinal = (std::filesystem::path(contentDir) / "gi" /
						(pakStem + ".sggi")).generic_string();
					cookTemp = cookFinal + ".tmp";
					gi << " --out " << QuoteArg(cookTemp);
				}
				gi << " 2>&1";
				push("cook-gi: " + gi.str());
#ifdef _WIN32
				FILE* gpipe = _popen(gi.str().c_str(), "r");
#else
				FILE* gpipe = popen(gi.str().c_str(), "r");
#endif
				if (!gpipe) {
					r.log = "cook-gi: failed to spawn GiBaker\n";
					std::error_code ec;
					std::filesystem::remove(tempTourPath, ec);
					return r;
				}
				pushPipe(gpipe);
#ifdef _WIN32
				const int grc = _pclose(gpipe);
#else
				const int grc = pclose(gpipe);
#endif
				if (grc != 0) {
					r.ok = false;
					r.log = "cook-gi: GiBaker exit=" + std::to_string(grc)
						+ " — pak cook SKIPPED (the GI bake failed; "
						"previous sidecars, if any, are untouched)\n";
					std::error_code ec;
					std::filesystem::remove(tempTourPath, ec);
					return r;
				}
				push("cook-gi: GiBaker exit=0 (fresh GI sidecars "
					"under " + contentDir + "/gi/)");
				if (wantWorld && !cookTemp.empty()) {
					std::error_code rec;
					std::filesystem::rename(cookTemp, cookFinal, rec);
					if (rec) {
						r.ok = false;
						r.log = "cook-gi: temp publish failed ("
							+ rec.message() + ") — pak cook SKIPPED\n";
						std::error_code ec;
						std::filesystem::remove(tempTourPath, ec);
						return r;
					}
					push("cook-gi: published '" + cookFinal + "'");
				}
			}
		} // stage 1 skipped for cubemap-only refreshes
		// ---- stage 2: Cook.exe then Pak.exe (the import/cook/pak split:
		// cook runs the bakes + writes them back into the artifacts,
		// pak assembles the .pak from the cooked artifacts + the GI
		// sidecar discovery/embed from stage 1). Quick bakes run the
		// Cook capture stage ONLY, no pak write (the loose sidecars it
		// writes are the output).
		auto toolPath = [&](const char* exe) {
			std::filesystem::path p =
				std::filesystem::path(exePath).parent_path() / exe;
			if (!std::filesystem::exists(p))
				p = exe; // PATH fallback
			return p;
		};
		// Positional tool CLI (tool.exe <contentDir> [--tour T | <artifact>]
		// [...]). The tour carries the cook block; CLI wins when both.
		auto baseArgs = [&]() {
			std::ostringstream a;
			a << QuoteArg(contentDir);
			if (!tempTourPath.empty())
				a << " --tour " << QuoteArg(tempTourPath);
			else
				a << " " << QuoteArg(inVfs);
			if (!scratchDir.empty())
				a << " --scratch-dir " << QuoteArg(scratchDir);
			return a.str();
		};
		auto spawnTool = [&](const std::string& exe,
			const std::string& args, const char* tag) -> int {
			std::ostringstream cmd;
			cmd << QuoteArg(exe) << " " << args << " 2>&1";
			push(std::string(tag) + ": ");
			push(cmd.str());
#ifdef _WIN32
			FILE* pipe = _popen(cmd.str().c_str(), "r");
#else
			FILE* pipe = popen(cmd.str().c_str(), "r");
#endif
			if (!pipe) {
				r.log = std::string(tag) + ": failed to spawn "
					+ exe + "\n";
				return 1;
			}
			pushPipe(pipe);
#ifdef _WIN32
			const int rc = _pclose(pipe);
#else
			const int rc = pclose(pipe);
#endif
			return rc;
		};
		int rc = spawnTool(toolPath("Cook.exe").string(),
			baseArgs() + (quickBakes ? " --quick-bakes on" : ""),
			quickBakes ? "update-bakes" : "cook");
		r.ok = (rc == 0);
		r.log = std::string(quickBakes ? "update-bakes: " : "cook: ")
			+ "Cook exit=" + std::to_string(rc) + "\n";
		if (r.ok && !quickBakes) {
			rc = spawnTool(toolPath("Pak.exe").string(),
				baseArgs() + " --out-pak " + QuoteArg(cook.outPak),
				"pak");
			r.ok = (rc == 0);
			r.log += "pak: Pak exit=" + std::to_string(rc) + "\n";
		}
			// Cleanup temp tour file.
			std::error_code ec;
			std::filesystem::remove(tempTourPath, ec);
			return r;
		});
}

void EditorApp::PollCookProcess() {
	if (!m_cookRunning)
		return;
	// Drain the live pipe feed every frame: the Console keeps the
	// persistent record AND the modal task window details live progress
	// (stuck vs. working is visible in both). The pak-stage marker line
	// flips the task stage from the GI half to the pak half.
	auto drainFeed = [this]() {
		std::shared_ptr<CookFeed> feed = m_cookFeed;
		if (!feed)
			return;
		std::vector<std::string> ready;
		{
			std::lock_guard<std::mutex> lk(feed->mutex);
			ready.swap(feed->lines);
		}
		for (auto& line : ready) {
			Log(line);
			TaskWindowPush(line);
			// Full-cook stage markers ("cook: " = the bake half, "pak: " =
			// the assembly half — NOT "cook-gi: ...", NOT the
			// quick-bakes "update-bakes: " marker).
			if (line.compare(0, 6, "cook: ") == 0)
				TaskWindowStage("Cook stage 1/2: bakes...");
			else if (line.compare(0, 5, "pak: ") == 0)
				TaskWindowStage("Cook stage 2/2: pak...");
			// Cubemap capture progress (Cook's [CUBE] lines carry two
			// leading spaces): total first, then one "baked" per
			// finished capture. Fraction bar + text stage.
			else if (m_cubeRunActive
				&& line.compare(0, 19, "  [CUBE] captures=") == 0) {
				m_cubeTotal = std::atoi(line.c_str() + 19);
				m_cubeDone = 0;
				if (m_cubeTotal > 0) {
					TaskWindowStage("Cubemap captures 0/"
						+ std::to_string(m_cubeTotal), 0.0f);
				}
			}
			else if (m_cubeRunActive && m_cubeTotal > 0
				&& line.compare(0, 12, "  [CUBE] baked ") == 0) {
				++m_cubeDone;
				if (m_cubeDone > m_cubeTotal)
					m_cubeDone = m_cubeTotal;
				TaskWindowStage("Cubemap captures "
					+ std::to_string(m_cubeDone) + "/"
					+ std::to_string(m_cubeTotal),
					float(m_cubeDone) / float(m_cubeTotal));
			}
		}
	};
	drainFeed();
	if (!m_cookFuture.valid())
		return;
	if (m_cookFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return;
	CookResult r = m_cookFuture.get();
	m_cookRunning = false;
	m_cubeRunActive = false;
	m_cookLastOk = r.ok;
	// Late lines that arrived between the drain above and completion.
	drainFeed();
	// Tail only (body was already streamed live).
	std::istringstream ss(r.log);
	std::string line;
	while (std::getline(ss, line)) {
		if (!line.empty()) {
			Log(line);
			TaskWindowPush(line);
		}
	}
	Log(r.ok ? "Cook finished OK." : "Cook FAILED (see log above).");
	TaskWindowFinish(r.ok, r.ok ? "Cook finished OK."
		: "Cook FAILED (detail above; Console keeps the full log).");
	if (r.ok) {
		RecordBakeAssetsFromCook(); // bake assets + the tour's bake refs
	}
	// The cook is the PRODUCT step (a pak for the game/App — Run in
	// App loads it): it never touches the viewport.
	// The cook panel shows its own progress + result line (no auto
	// close: the tab stays put, the Console keeps the record).
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
	if (SceneEmpty()) {
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
	// Blocking progress surface (modal task window).
	TaskWindowBegin("Bake GI", "Baking GI ("
		+ std::string(wantWorld ? "world field" : "")
		+ std::string((wantWorld && wantCvols) ? " + " : "")
		+ std::string(wantCvols ? "convex volumes" : "") + ")...");
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
			// Temp-file bake: GiBaker writes gi/<stem>.sggi.tmp, the
			// worker atomically renames to gi/<stem>.sggi on success —
			// a failed bake never leaves a half-written world field.
			std::string bakeFinal, bakeTemp;
			if (wantWorld) {
				bakeFinal = (std::filesystem::path(contentRoot) / "gi" /
					(pakStem + ".sggi")).generic_string();
				bakeTemp = bakeFinal + ".tmp";
				gi << " --out " << QuoteArg(bakeTemp);
			}
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
			else if (wantWorld && !bakeTemp.empty()) {
				// Atomic publish of the temp bake output.
				std::error_code rec;
				std::filesystem::rename(bakeTemp, bakeFinal, rec);
				if (rec) {
					r.ok = false;
					r.log += "bake-gi: temp publish failed (" + rec.message()
						+ ") — bake discarded\n";
				}
				else {
					push("bake-gi: published '" + bakeFinal + "'");
				}
			}
			std::error_code ec;
			std::filesystem::remove(tempTourPath, ec);
			return r;
		});
}

void EditorApp::PollBakeProcess() {
	if (m_bakeRunning)
		return;
	// Drain the live pipe feed every frame (same pattern as the cook):
	// Console record + modal task window detail.
	auto drainFeed = [this]() {
		std::shared_ptr<CookFeed> feed = m_bakeFeed;
		if (!feed)
			return;
		std::vector<std::string> ready;
		{
			std::lock_guard<std::mutex> lk(feed->mutex);
			ready.swap(feed->lines);
		}
		for (auto& line : ready) {
			Log(line);
			TaskWindowPush(line);
		}
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
		if (!line.empty()) {
			Log(line);
			TaskWindowPush(line);
		}
	}
	if (r.ok) {
		// The sidecars now match the baked definition: record it so a
		// later cook can warn when they go stale.
		m_giBakedHash = m_giBakePendingHash;
		Log("Bake GI finished OK — sidecars at their stable paths.");
		TaskWindowFinish(true, "Bake GI finished OK.");
	}
	else {
		Log("Bake GI FAILED (see log above).");
		TaskWindowFinish(false,
			"Bake GI FAILED (detail above; Console keeps the full log).");
	}
	m_giBakePendingHash.clear();
}

} // namespace editor
