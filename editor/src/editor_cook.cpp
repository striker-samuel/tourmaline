// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Cook flow: save the .tour command log, then run PakCooker --tour on a
// worker thread (full bake support; the cooker replays the tour onto a
// fresh import � deterministic source + tour in, pak out).

#include "editor_app.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

#include <Engine/Asset/bake_asset.h>
#include <Engine/Asset/tour.h>
#include <Engine/Renderer/cubemap.h>
#include <Engine/Vfs/vfs.h>

#include "gi_baker.h"
#include "cook_tool.h"
#include "pak_tool.h"
#include <tool_log.h>

#include <fstream>
#include <optional>

namespace editor {


// Shared snapshot for the cooker/baker subprocesses: a TEMP coalesced
// tour (the canonical .tour is never overwritten; becomeTarget = false
// keeps the live target, dirty star and autosave untouched) plus the
// definition fingerprint (tour bytes minus the volatile "//Last
// modified:" line � deterministic per definition content).
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
	// minutes-long GiBaker stage). Session-adopts cubemap baking (the
	// completion adopter keys on it): flips the exportSettings node's
	// cubemap switch through the ordinary undoable SetExportAttr path.
	if (!ExportCfg().cubemap) {
		const uint32_t esNode = FindExportSettingsNode();
		if (esNode != UINT32_MAX) {
			PushExportAttr(esNode, "cubemap", 0.0, true, "");
			AutosaveTour();
		}
	}
	CookToPak(true, true);
}

void EditorApp::CookToPak(bool quickBakes, bool cubemapsOnly) {
	// Definition scenes only � with pak opens gone (paks run in the App),
	// every open editor scene is definition-backed; the gate left is
	// content (empty sourceless worlds have nothing to cook � the cooker
	// replays the tour's asset-UUID references itself).
	if (m_cookRunning || SceneEmpty()) {
		if (SceneEmpty())
			Log("Cook: no scene content loaded.");
		return;
	}
	if (m_bakeRunning) {
		Log("Cook: a Bake GI is running � wait for it to finish (the GI "
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
	// THE CANONICAL HOST PATH drives the marker walk (m_gltfHostPath �
	// the file that actually loaded, GUID-resolved on tour opens): the
	// recorded identity (m_gltfPath) can be a project-RELATIVE
	// "content/models/..." on tour-opened scenes, whose "." parent then
	// resolved against the EDITOR CWD (the build dir) � every bake
	// sidecar the cook stages (gi/*.sggi + *.cvol, textures/cubemaps/
	// <base>[_N].ktx2 + the .cubemanifest) landed next to the exe
	// instead of the project (and the project-relative resolution below
	// never saw them (the "cubemap probe preview not drawn in the
	// editor" report � the probes bake fine, they just surface in the
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
	// Auto-generate the output path if empty (session state � also the
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
	// GiBaker first � the world-field octree (ExportCfg().giWorld) + the
	// tour's authored convex volumes, fresh, nothing reused � then
	// PakCooker discovers the fresh sidecars, EMBEDS them into the pak
	// and registers the guid bindings. The completion handler records
	// the bake assets + the tour's bake references (see
	// RecordBakeAssetsFromCook). This is minutes-long on real scenes
	// (the world field) � the on-demand Bake GI remains the quick path.
	const std::string pakStem =
		std::filesystem::path(cook.outPak).stem().string();
	const bool wantWorld = ExportCfg().giWorld;
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
	// Label honesty: the cook UPDATES the scene's bake in place � GiBaker
	// re-runs it at the stable stem paths (overwrites; RecordBakeAssets-
	// FromCook overwrites the bake guid reference in the current scene's
	// tour and deletes the superseded old bake), then the pak follows.
	// Quick bakes: the same GiBaker stage, then PakCooker --quick-bakes
	// (capture-only refresh � the loose sidecars are the output; no pak
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
	// registry � hit = hash-validated copy, miss = transcode + stage.
	// The EFFECTIVE store root (project-owned; empty = cache off).
	// Captured by value (the worker lambda has no this).
	const std::string registryDir =
		m_store ? m_store->Root() : std::string();
	// Fibers job (the editor's single off-main-thread lane � see
	// editor_fibers.h): the old std::async worker verbatim, sharing a
	// result slot with the completion callback that runs ON MAIN via
	// m_fibers.Pump() (the old future.wait_for branch in
	// PollCookProcess, which now only drains the feed).
	auto result = std::make_shared<CookResult>();
	m_fibers.Post(quickBakes ? "update-bakes" : "cook",
		[cook, feed, registryDir, pakStem, wantWorld, quickBakes, cubemapsOnly, tempTourPath = std::move(tempTourPath), contentDir = std::move(contentDir), inVfs = std::move(inVfs), result]() {
			CookResult r{};
			// Live feed: each tool line is pushed to the shared feed
			// (mutex-guarded); PollCookProcess drains it on the main
			// thread so the Console shows progress while the cook
			// runs. r.log carries only the tail (exit codes) since
			// the body was already streamed.
			auto push = [feed](const std::string& line) {
				std::lock_guard<std::mutex> lk(feed->mutex);
				feed->lines.push_back(line);
			};
		// ---- stage 1: the scene's GI bake re-run IN PLACE (GiBaker �
		// the world field when requested + the tour's authored convex
		// volumes; the definition tour is the only input, nothing
		// reused; the STABLE stem paths overwrite, and the completion
		// handler deletes the superseded old bake when the stem
		// changed). The fresh sidecars land under <contentDir>/gi/
		// exactly where stage 2 (PakCooker) discovers + embeds them
		// into the pak. A failed GI bake FAILS the cook (the pak would
		// silently miss its GI otherwise � the exact bug this flow
		// exists to prevent). SKIPPED for cubemap-only refreshes.
		// Runs IN-PROCESS (GiBakerLib): the CLI stays a thin user
		// interface over the same Run. Baker lines stream through the
		// tool line sink into this feed (same lines the pipe carried).
		if (!cubemapsOnly)
		{
			gibaker::Args gargs;
			gargs.contentDir = contentDir;
			gargs.scene = pakStem;
			gargs.tour = tempTourPath;
			std::string cookFinal, cookTemp;
			if (wantWorld) {
				cookFinal = (std::filesystem::path(contentDir) / "gi" /
					(pakStem + ".sggi")).generic_string();
				cookTemp = cookFinal + ".tmp";
				gargs.out = cookTemp;
			}
			// Temp-file bake (same as BakeGI): publish atomically so
			// a failed baker never corrupts the live world field.
			push("cook-gi: GiBaker (in-process)"
				" --scene " + pakStem
				+ (wantWorld ? " --out <tmp>" : ""));
			tool::SetLineSink([&push](const std::string& line, bool) {
				push(line);
			});
			const int grc = gibaker::Run(gargs);
			tool::SetLineSink(nullptr);
			if (grc != 0) {
				r.ok = false;
				r.log = "cook-gi: GiBaker exit=" + std::to_string(grc)
					+ " � pak cook SKIPPED (the GI bake failed; "
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
						+ rec.message() + ") � pak cook SKIPPED\n";
					std::error_code ec;
					std::filesystem::remove(tempTourPath, ec);
					return r;
				}
				push("cook-gi: published '" + cookFinal + "'");
			}
		} // stage 1 skipped for cubemap-only refreshes
		// ---- stage 2: Cook.exe then Pak.exe (the import/cook/pak split:
		// cook runs the bakes + writes them back into the artifacts,
		// pak assembles the .pak from the cooked artifacts + the GI
		// sidecar discovery/embed from stage 1). Quick bakes run the
		// Cook capture stage ONLY, no pak write (the loose sidecars it
		// writes are the output). Both run IN-PROCESS (CookLib/PakLib);
		// the CLIs stay thin user interfaces over the same Runs.
		cooktool::CookArgs cookArgs;
		cookArgs.contentDir = contentDir;
		if (!tempTourPath.empty())
			cookArgs.tour = tempTourPath;
		else {
			cookArgs.inVfsList.push_back(inVfs);
			cookArgs.inVfs = inVfs;
		}
		if (!registryDir.empty())
			cookArgs.registryDir = registryDir;
		cookArgs.quickBakes = quickBakes;
		push("cook: Cook (in-process)"
			+ std::string(quickBakes ? " --quick-bakes" : ""));
		tool::SetLineSink([&push](const std::string& line, bool) {
			push(line);
		});
		int rc = cooktool::Run(cookArgs);
		tool::SetLineSink(nullptr);
		r.ok = (rc == 0);
		r.log = std::string(quickBakes ? "update-bakes: " : "cook: ")
			+ "Cook exit=" + std::to_string(rc) + "\n";
		if (r.ok && !quickBakes) {
			push("pak: Pak (in-process) --out-pak " + cook.outPak);
			paktool::PakArgs pargs;
			pargs.contentDir = contentDir;
			if (!tempTourPath.empty())
				pargs.tour = tempTourPath;
			else {
				pargs.inVfsList.push_back(inVfs);
				pargs.inVfs = inVfs;
			}
			if (!registryDir.empty())
				pargs.registryDir = registryDir;
			pargs.outPak = cook.outPak;
			// Level blob (the playable unit; see Engine/Asset/level.h):
			// every editor cook packs one, keyed by the pak stem � the
			// LEVEL owns the scene + baked data, the scene-stem sidecar
			// association dies at the pak level.
			if (pargs.levelName.empty())
				pargs.levelName = pakStem;
			tool::SetLineSink([&push](const std::string& line, bool) {
				push(line);
			});
			rc = paktool::Run(pargs);
			tool::SetLineSink(nullptr);
			r.ok = (rc == 0);
			r.log += "pak: Pak exit=" + std::to_string(rc) + "\n";
		}
			// Cleanup temp tour file.
			std::error_code ec;
			std::filesystem::remove(tempTourPath, ec);
			*result = r;
		},
		[this, result](const editor::FiberBackend::Completion& c) {
			// Job threw (the tools normally catch their own; this is
			// the fiber-level guard): fail the cook loud.
			if (!c.ok) {
				Log("cook: job error: " + c.error);
				TaskWindowFinish(false,
					"Cook FAILED (job error: " + c.error + ")");
				m_cookRunning = false;
				m_cubeRunActive = false;
				m_cookLastOk = false;
				m_cookJustFinished = true;
				return;
			}
			CookResult r = *result;
			m_cookRunning = false;
			m_cubeRunActive = false;
			m_cookLastOk = r.ok;
			// Late lines that arrived after the last per-frame drain.
			{
				std::shared_ptr<CookFeed> feed = m_cookFeed;
				if (feed) {
					std::vector<std::string> ready;
					{
						std::lock_guard<std::mutex> lk(feed->mutex);
						ready.swap(feed->lines);
					}
					for (auto& line : ready) {
						Log(line);
						TaskWindowPush(line);
					}
				}
			}
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
			// The cook is the PRODUCT step (a pak for the game/App � Run in
			// App loads it): it never touches the viewport.
			// The cook panel shows its own progress + result line (no auto
			// close: the tab stays put, the Console keeps the record).
			m_cookJustFinished = true;
		});
}

void EditorApp::PollCookProcess() {
	if (!m_cookRunning)
		return;
	// Live tool feed drain ONLY (the finish logic rides the fibers
	// completion callback � see CookToPak). The Console keeps the
	// persistent record AND the task window details live progress
	// (stuck vs. working is visible in both). The pak-stage marker line
	// flips the task stage from the GI half to the pak half.
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
		// the assembly half � NOT "cook-gi: ...", NOT the
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
}

// ---- Bake GI on demand (never part of the cook) --------------------
// GiBaker consumes the SAME definition-tour snapshot a cook would
// (root reference + nodes), keyed to the cook's OUT-PAK STEM so the
// sidecars land exactly where the editor/App loaders look:
// gi/<stem>_*.cvol + .cvmanifest, and gi/<stem>.sggi for the world
// field. Missing sidecars are NOT an error anywhere down the line �
// every GI/volume/cubemap/lightmap loader falls back to the analytic
// sky IBL (see DegradeAmbientForMissingSources in app).

void EditorApp::BakeGI() {
	if (m_bakeRunning)
		return;
	if (m_cookRunning) {
		Log("Bake GI: a cook is running � wait for it to finish.");
		return;
	}
	if (SceneEmpty()) {
		Log("Bake GI: open a source scene first (imports land ready to "
			"cook; cooking is the separate product step).");
		return;
	}
	// What to bake comes from the same flags the cook dialog shows.
	const bool giHasCvols = !m_world->scene.cvolForNode.empty();
	const bool wantCvols = ExportCfg().giCvol && giHasCvols;
	const bool wantWorld = ExportCfg().giWorld;
	if (!wantCvols && !wantWorld) {
		Log("Bake GI: nothing requested � enable World field and/or "
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
		Log("Bake GI: no cook target � cook once first (Ctrl+Shift+C).");
		return;
	}
	std::string tempTourPath, defHash;
	if (!SnapshotTourForProcess("BakeGI", tempTourPath, defHash))
		return;
	const std::string pakStem =
		std::filesystem::path(m_cook.outPak).stem().string();
	// contentRoot = the "content/" dir itself (sidecars land in gi/
	// next to the pak � exactly what the loaders check).
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
	// Fibers job (the editor's single off-main-thread lane): the old
	// std::async worker verbatim, sharing a result slot with the
	// completion callback that runs ON MAIN via m_fibers.Pump() (the
	// old future.wait_for branch in PollBakeProcess).
	auto result = std::make_shared<CookResult>();
	m_fibers.Post("bake-gi",
		[pakStem, contentRoot, wantWorld, feed, tempTourPath = std::move(tempTourPath), result]() {
			CookResult r{};
			auto push = [feed](const std::string& line) {
				std::lock_guard<std::mutex> lk(feed->mutex);
				feed->lines.push_back(line);
			};
			// GiBaker runs IN-PROCESS (GiBakerLib): the CLI stays a thin
			// user interface over the same Run. Baker lines stream
			// through the tool line sink into this feed.
			gibaker::Args gargs;
			gargs.contentDir = contentRoot;
			gargs.scene = pakStem;
			gargs.tour = tempTourPath;
			// Temp-file bake: GiBaker writes gi/<stem>.sggi.tmp, the
			// worker atomically renames to gi/<stem>.sggi on success �
			// a failed bake never leaves a half-written world field.
			std::string bakeFinal, bakeTemp;
			if (wantWorld) {
				bakeFinal = (std::filesystem::path(contentRoot) / "gi" /
					(pakStem + ".sggi")).generic_string();
				bakeTemp = bakeFinal + ".tmp";
				gargs.out = bakeTemp;
			}
			push("bake-gi: GiBaker (in-process)"
				" --scene " + pakStem
				+ (wantWorld ? " --out <tmp>" : ""));
			tool::SetLineSink([&push](const std::string& line, bool) {
				push(line);
			});
			const int grc = gibaker::Run(gargs);
			tool::SetLineSink(nullptr);
			// Temp tour snapshot cleanup (was deleted by the spawn
			// path's early returns; single exit now).
			{
				std::error_code ec;
				std::filesystem::remove(tempTourPath, ec);
			}
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
						+ ") � bake discarded\n";
				}
				else {
					push("bake-gi: published '" + bakeFinal + "'");
				}
			}
			std::error_code ec;
			std::filesystem::remove(tempTourPath, ec);
			*result = r;
		},
		[this, result](const editor::FiberBackend::Completion& c) {
			// Job threw (fiber-level guard; GiBaker normally returns
			// codes): fail the bake loud.
			if (!c.ok) {
				Log("bake-gi: job error: " + c.error);
				TaskWindowFinish(false,
					"Bake GI FAILED (job error: " + c.error + ")");
				m_bakeRunning = false;
				m_giBakePendingHash.clear();
				return;
			}
			CookResult r = *result;
			m_bakeRunning = false;
			// Late lines that arrived after the last per-frame drain.
			{
				std::shared_ptr<CookFeed> feed = m_bakeFeed;
				if (feed) {
					std::vector<std::string> ready;
					{
						std::lock_guard<std::mutex> lk(feed->mutex);
						ready.swap(feed->lines);
					}
					for (auto& line : ready) {
						Log(line);
						TaskWindowPush(line);
					}
				}
			}
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
				Log("Bake GI finished OK � sidecars at their stable paths.");
				TaskWindowFinish(true, "Bake GI finished OK.");
			}
			else {
				Log("Bake GI FAILED (see log above).");
				TaskWindowFinish(false,
					"Bake GI FAILED (detail above; Console keeps the full log).");
			}
			m_giBakePendingHash.clear();
		});
}

void EditorApp::PollBakeProcess() {
	if (!m_bakeRunning)
		return;
	// Live tool feed drain ONLY (the finish logic rides the fibers
	// completion callback � see BakeGI). Console record + modal task
	// window detail, same pattern as the cook.
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
}

} // namespace editor
