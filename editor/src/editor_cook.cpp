// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

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

#include <Engine/App/cluster.h>
#include <Engine/Asset/bake_asset.h>
#include <Engine/Asset/tour.h>
#include <Engine/GI/bake.h>
#include <Engine/GI/cubemap_volume.h>
#include <Engine/Renderer/cubemap.h>
#include <Engine/Scene/scene.h>
#include <Engine/Vfs/vfs.h>
#include <gltf_import/gltf.h>

#include "gi_baker.h"
#include "cook_tool.h"
#include "pak_tool.h"
#include <tool_log.h>

#include <project_pipeline/build_pipeline.h>
#include <project_pipeline/project.h>
#include <project_pipeline/project_recipe.h>
#include <project_pipeline/recipe_resolver.h>

#include <fstream>
#include <optional>

namespace editor {

namespace {

// The EDITOR's stage adapter (the shared executor's invocation seam):
// the LINKED-IN cooktool/gibaker/paktool Runs — the same Runs the CLI
// exes' own mains call — with the tool line sink streaming into the
// cook feed (the job sets the sink around ExecuteBuildPlan). The
// executor owns sequencing; this adapter owns HOW each stage runs.
// That is the whole CLI/editor unification: one plan, two invocation
// mechanisms, zero second cooking implementations.
class EditorStageAdapter final : public smvk::project::StageAdapter {
public:
	int Cook(const smvk::project::CookStageArgs& a,
		std::string&) override {
		cooktool::CookArgs args;
		args.contentDir = a.contentDir;
		args.tour = a.tour;
		args.cubemap = a.cubemap;
		args.lightmap = a.lightmap;
		args.registryDir = a.registryDir;
		return cooktool::Run(args);
	}
	int GiBaker(const smvk::project::GiBakerStageArgs& a,
		std::string&) override {
		gibaker::Args args;
		args.contentDir = a.contentDir;
		args.tour = a.tour;
		return gibaker::Run(args);
	}
	int Pak(const smvk::project::PakStageArgs& a,
		std::string&) override {
		paktool::PakArgs args;
		args.contentDir = a.contentDir;
		args.inVfs = a.inVfs;
		args.inVfsList = a.extraArtifacts;
		args.tour = a.tour;
		args.outPak = a.outPak;
		args.verify = a.verify;
		args.levelName = a.levelName;
		args.registryDir = a.registryDir;
		return paktool::Run(args);
	}
};

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
		Log("Cook: a Bake GI is running — wait for it to finish (the "
			"loose GI bakes it writes are the ones this cook embeds).");
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
		// file the cook stages (gi/*.givolume, textures/cubemaps/
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
	// destination the completion handler + Bake GI derive the bake
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
	// GiBaker — the tour's authored convex volumes, fresh, nothing
	// reused — minting RANDOM artifacts (a fresh guid per bake; the
	// binding record IS the identity, no stem-keyed file name anywhere)
	// and STAMPING the tour's giAsset records (the hard identification;
	// the in-process run stamps the snapshot below — the job reads them
	// back into the editor's live-doc mirror, RecordBakeAssetsFromCook,
	// and the stamp itself deletes the superseded old artifacts BY
	// RECORD). Stage 2 (PakCooker, same snapshot) embeds EXACTLY the
	// bound files + binds them into the pak. (The world-field octree
	// leg is RETIRED — stage 3 of the octree retirement: the ambient
	// rides the authored convex set + the sky IBL.) This is
	// minutes-long on real scenes — the on-demand Bake GI remains the
	// quick path.
	const std::string pakStem =
		std::filesystem::path(cook.outPak).stem().string();
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
	// (capture-only refresh — the loose bake files are the output; no pak
	// is written).
	Log(std::string(quickBakes ? "Update bakes started -> " : "Cook started -> ")
		+ cook.outPak
		+ (cubemapsOnly
			? std::string(" (cubemap captures refreshed, GiBaker skipped; no pak write)")
			: quickBakes
			? std::string(" (GI bake updated in place + cubemap captures "
				"refreshed; no pak write)")
			: std::string(" (GI bake updated in place: convex volumes, "
				"then the pak)")));
	// ---- the unified path's inputs (computed on MAIN, before the post):
	// a PROJECT scene (the authored .tour inside the open project)
	// builds through the SAME resolver + executor the CLI drives — one
	// plan, one sequence, the linked-in Runs as the adapter. Refresh
	// runs (quick bakes / cubemap-only) and EXTERNAL scenes (outside
	// the project — the plan model is project-scoped by design) keep
	// the direct stage calls below.
	bool unifiedBuild = false;
	std::string projectRelTour;
	bool bakeCubemapsFlag = ExportCfg().cubemap;
	{
		if (!quickBakes && !cubemapsOnly && !m_projectRoot.empty()
				&& !m_tourPath.empty()) {
			std::string perr;
			smvk::project::Project proj =
				smvk::project::Project::Open(m_projectRoot, perr);
			if (proj.Valid()) {
				projectRelTour = proj.RelativeTo(
					std::filesystem::absolute(m_tourPath)
						.lexically_normal().generic_string());
				unifiedBuild = !projectRelTour.empty()
					&& projectRelTour.rfind("content/") == 0;
			}
		}
	}
 	auto feed = std::make_shared<CookFeed>();
 	m_cookFeed = feed;
	// Cache acceleration (opt-in, never required): when the project
	// artifact cache is online, the cooker consults + feeds the same
	// registry — hit = hash-validated copy, miss = transcode + stage.
	// The EFFECTIVE store root (project-owned; empty = cache off).
	// Captured by value (the worker lambda has no this).
	const std::string registryDir =
		m_store ? m_store->Root() : std::string();
	// Fibers job (the editor's single off-main-thread lane — see
	// editor_fibers.h): the old std::async worker verbatim, sharing a
	// result slot with the completion callback that runs ON MAIN via
	// m_fibers.Pump() (the old future.wait_for branch in
	// PollCookProcess, which now only drains the feed).
 	auto result = std::make_shared<CookResult>();
 	m_fibers.Post(quickBakes ? "update-bakes" : "cook",
 		[cook, feed, registryDir, pakStem, quickBakes, cubemapsOnly,
 			projectRoot = m_projectRoot, projectRelTour, unifiedBuild,
 			bakeCubemapsFlag, tempTourPath = std::move(tempTourPath), contentDir = std::move(contentDir), inVfs = std::move(inVfs), result]() {
			CookResult r{};
			// Live feed: each tool line is pushed to the shared feed
			// (mutex-guarded); PollCookProcess drains it on the main
			// thread so the Console shows progress while the cook
 			// runs. r.log carries only the tail (exit codes) since
 			// the body was already streamed. err-tagged lines (the
 			// tools' tool::err()) also land in the bounded errLines
 			// tail the completion handler folds into the failure
 			// message.
 			auto push = [feed](const std::string& line, bool err = false) {
 				std::lock_guard<std::mutex> lk(feed->mutex);
 				feed->lines.push_back(line);
 				if (err) {
 					feed->errLines.push_back(line);
 					if (feed->errLines.size() > 8)
 						feed->errLines.erase(feed->errLines.begin());
 				}
 			};
		// ---- THE UNIFIED BUILD (the shared executor) -------------------
		// Project scenes build through the same resolver + executor the
		// CLI drives: the synthesized recipe (this scene, this out pak,
		// the session's bake flags) -> ResolveRecipe -> ExecuteBuildPlan
		// with the EDITOR adapter (the linked-in cooktool/gibaker/
		// paktool Runs — the same Runs the CLI exes' own mains call, so
		// a CLI build and an editor build of the same saved scene are
		// the same stage sequence). The plan RESOLVES against the
		// authored .tour; every STAGE runs against the SNAPSHOT (the
		// authored document is never touched — the unsaved-scene build).
		// The executor also refreshes stale imports (the canonical
		// ImportProjectAsset — in-process like every stage here) and
		// publishes the appmanifest beside the pak.
		if (unifiedBuild)
		{
			smvk::project::Project proj;
			std::string perr;
			proj = smvk::project::Project::Open(projectRoot, perr);
			if (!proj.Valid()) {
				r.ok = false;
				r.log = "cook: project open failed (" + perr + ")\n";
				std::error_code ec;
				std::filesystem::remove(tempTourPath, ec);
				*result = r;
				return;
			}
			smvk::project::ProjectRecipe recipe;
			recipe.version = smvk::project::kRecipeGrammarVersion;
			recipe.projectName = proj.name;
			recipe.entry = projectRelTour;
			recipe.bakeGi = true; // the cook always refreshed the bake
			recipe.bakeCubemaps = bakeCubemapsFlag; // the session's node
			smvk::project::RecipePak rpak;
			rpak.id = "scene";
			rpak.output = std::filesystem::path(cook.outPak)
				.filename().generic_string();
			rpak.scene = projectRelTour;
			recipe.paks.push_back(rpak);
			smvk::project::BuildPlan plan;
			std::string planErr;
			push("cook: resolving the plan (the shared executor)");
			if (!smvk::project::ResolveRecipe(proj, recipe, plan,
					planErr)) {
				r.ok = false;
				r.log = "cook: plan resolve failed (" + planErr + ")\n";
				std::error_code ec;
				std::filesystem::remove(tempTourPath, ec);
				*result = r;
				return;
			}
			smvk::project::BuildPlanOptions opts;
			opts.publishDir = std::filesystem::path(cook.outPak)
				.parent_path().generic_string();
			opts.levelName = pakStem; // the playable-unit convention
			opts.registryDir = registryDir;
			opts.sceneSnapshot = tempTourPath; // EDT-02: never the authored doc
			EditorStageAdapter adapter;
			push("cook: executing the plan (Cook -> GiBaker -> Pak:"
				" verified + policed; import freshness first)");
			push("cook: stage 1/2 (bakes) starting");
			tool::SetLineSink([&push](const std::string& line, bool err) {
				push(line, err);
			});
			smvk::project::BuildPlanReport report;
			const bool ok = smvk::project::ExecuteBuildPlan(proj, plan,
				adapter, opts, report, planErr);
			tool::SetLineSink(nullptr);
			r.ok = ok;
			r.log = ok
				? "cook: plan executed OK ("
					+ std::to_string(report.paks.size()) + " pak(s),"
					+ " appmanifest " + report.manifest + ")\n"
				: "cook: plan failed (" + planErr + ")\n";
			// The stamped records ride the result BEFORE the snapshot
			// dies: GiBaker (a plan stage) stamped the SNAPSHOT tour;
			// read them back so the live-doc mirror follows on success
			// (the bake owner is the one authority on the records).
			if (ok) {
				smvk::tour::Doc stamped;
				std::string lerr;
				std::vector<std::string> lwarn;
				if (smvk::tour::LoadTour(tempTourPath, stamped, lerr,
						&lwarn)) {
					r.giAssets = std::move(stamped.giAssets);
				}
				else {
					push("cook: snapshot record read failed (" + lerr
						+ ") — the live doc keeps its records", true);
				}
			}
			std::error_code ec;
			std::filesystem::remove(tempTourPath, ec);
			push("cook: unified build epilogue — result recorded");
			*result = r;
			return;
		}
		// ---- stage 1: the scene's GI bake (GiBaker — the tour's authored
		// convex volumes; the definition tour is the only input, nothing
		// reused). Every artifact lands at a RANDOM-MINTED path (GiBaker
		// mints each GI volume) and the bake STAMPS the tour's giAsset
		// records — the snapshot file carries them; the job reads them
		// back below so the live doc's mirror follows, and the stamp's
		// supersede list retires the replaced old artifacts BY RECORD
		// (exact; random names can never collide, so a failed bake only
		// ever leaves an unbound orphan — GiBaker removes it itself on a
		// failed stage). A failed GI bake FAILS the cook (the pak would
		// silently miss its GI otherwise — the exact bug this flow
		// exists to prevent). SKIPPED for cubemap-only refreshes.
		// Runs IN-PROCESS (GiBakerLib): the CLI stays a thin user
		// interface over the same Run. Baker lines stream through the
		// tool line sink into this feed (same lines the pipe carried).
		if (!cubemapsOnly)
		{
			gibaker::Args gargs;
			gargs.contentDir = contentDir;
			gargs.tour = tempTourPath;
			push("cook-gi: GiBaker (in-process)");
			tool::SetLineSink([&push](const std::string& line, bool err) {
				push(line, err);
			});
			const int grc = gibaker::Run(gargs);
			tool::SetLineSink(nullptr);
			if (grc != 0) {
				r.ok = false;
				r.log = "cook-gi: GiBaker exit=" + std::to_string(grc)
					+ " — pak cook SKIPPED (the GI bake failed; "
					"previous bake files, if any, are untouched)\n";
				std::error_code ec;
				std::filesystem::remove(tempTourPath, ec);
				// EVERY exit path must record the result: the job runs
				// through std::function<void()> — a `return r;` would
				// DISCARD r into the void and leave *result at its
				// default ("no stage exits recorded"), the exact
				// empty-tail signature the self-describing failure
				// line caught in the field.
				*result = r;
				return;
			}
			push("cook-gi: GiBaker exit=0 (fresh minted GI artifacts "
				"under " + contentDir + "/gi/)");
			// The stamped records ride the result: the bake wrote the
			// giAsset bindings into the SNAPSHOT tour (its stampTour —
			// GiBakerLib, supersede-deletion included), so read them
			// back instead of re-deriving (the bake owner is the one
			// authority on the records; the completion folds them into
			// the live-doc mirror).
			{
				smvk::tour::Doc stamped;
				std::string lerr;
				std::vector<std::string> lwarn;
				if (smvk::tour::LoadTour(tempTourPath, stamped, lerr,
						&lwarn)) {
					r.giAssets = std::move(stamped.giAssets);
				}
				else {
					push("cook-gi: snapshot record read failed ("
						+ lerr + ") — the live doc keeps its records",
						true);
				}
			}
		} // stage 1 skipped for cubemap-only refreshes
		// Breadcrumb: the job is between stage 1 and the Cook stage
		// here. The ghost-exit hunt (a run that logged the GiBaker
		// exit-0 push, then "no stage exits recorded" with no job
		// error — no C++ exception path can do that) needs these
		// brackets to localize the silent exit if it ever recurs.
		push("cook: stage 2 (Cook) starting");
		// ---- stage 2: Cook.exe then Pak.exe (the import/cook/pak split:
		// cook runs the bakes + writes them back into the artifacts,
		// pak assembles the .pak from the cooked artifacts + the GI
		// bake-file discovery/embed from stage 1). Quick bakes run the
		// Cook capture stage ONLY, no pak write (the loose bake files it
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
		tool::SetLineSink([&push](const std::string& line, bool err) {
			push(line, err);
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
			// every editor cook packs one, keyed by the pak stem — the
			// LEVEL owns the scene + baked data, the scene-stem bake-file
			// association dies at the pak level.
			if (pargs.levelName.empty())
				pargs.levelName = pakStem;
			tool::SetLineSink([&push](const std::string& line, bool err) {
				push(line, err);
			});
			rc = paktool::Run(pargs);
			tool::SetLineSink(nullptr);
			r.ok = (rc == 0);
			r.log += "pak: Pak exit=" + std::to_string(rc) + "\n";
		}
			// Cleanup temp tour file.
			std::error_code ec;
			std::filesystem::remove(tempTourPath, ec);
			// Breadcrumb + the result record itself (see the stage-1
			// note: no value-returns — the void job discards them).
			push("cook: job epilogue — result recorded");
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
		// Self-describing failure line (the "see log above" pointer kept
		// getting lost — pasted excerpts drop the console tail above
		// the notice, and every failure exit writes its stage-exit line
		// into r.log): the final line names the stage exits inline
		// (e.g. "cook: Cook exit=1; pak: Pak exit=0") and folds in the
		// last tool::err lines the sink tagged, so the actual tool error
		// rides the notice itself.
		std::string failSummary;
		if (!r.ok) {
			std::istringstream ts(r.log);
			std::string tl;
			while (std::getline(ts, tl))
				if (!tl.empty())
					failSummary += (failSummary.empty() ? "" : "; ") + tl;
			std::shared_ptr<CookFeed> feed = m_cookFeed;
			if (feed) {
				std::lock_guard<std::mutex> lk(feed->mutex);
				for (const std::string& e : feed->errLines)
					failSummary += "\n  " + e;
			}
			if (failSummary.empty())
				failSummary = "no stage exits recorded (job thrown?)";
		}
		Log(r.ok ? "Cook finished OK."
			: "Cook FAILED (" + failSummary + "; detail above).");
		TaskWindowFinish(r.ok, r.ok ? "Cook finished OK."
			: "Cook FAILED (" + failSummary + ")");
			if (r.ok) {
				// The bake's giAsset records into the LIVE-doc mirror
				// (the session state SaveTourTo writes; the bake owner
				// stamped the snapshot — the job read them back). The
				// editor never derives records itself.
				//
				// OWNER'S DECISION (the EDT-04 question, settled):
				// bake bindings are DERIVED state — the bake owner's
				// records of the artifacts it just wrote — so the
				// adoption is ONE ATOMIC, SUCCESS-ONLY assignment and
				// deliberately NOT an undoable edit step. An undo
				// transaction would revert the doc's bindings toward
				// bakes that no longer exist on disk (the stamp's
				// supersede list already retired them by record) — an
				// inconsistent doc pointing at deleted artifacts. The
				// safety that matters is EDT-03's contract, which this
				// fold keeps exactly: on failure nothing changes (the
				// live doc keeps its last valid bindings verbatim, the
				// viewport stands, the last valid outputs stand).
				if (!r.giAssets.empty()) {
					m_giAssets = std::move(r.giAssets);
					Log("Cook: tour giAsset bindings updated ("
						+ std::to_string(m_giAssets.size())
						+ " record(s)) — saved with the scene");
				}
				RecordBakeAssetsFromCook(); // bake assets + the tour's bake refs
			}
			// The cook is the PRODUCT step (a pak for the game/App — Run in
			// App loads it): it never touches the viewport.
			// The cook panel shows its own progress + result line (no auto
			// close: the tab stays put, the Console keeps the record).
			m_cookJustFinished = true;
		});
}

// The inspector's "Optimize grid layout" button (see editor_app.h): the
// LIVE editor world is the input — no tour snapshot, no tour replay, no
// cook, no GI rebake. The geometry snapshot comes from the resident
// clusters' retained CPU uploads (the same mirror the picking paths
// read, so UNSAVED edits and editor-only nodes are included exactly as
// they render), the BVH build + the spacing/phase search run on the
// fibers lane, and the completion commits the chosen layout as ONE
// undoable "optlayout" edit (spacing + phase in a single step). The
// cached irradiance is untouched: the volume is marked stale by the
// command, and the next Bake GI rebakes at the new positions (the
// editor's hot-swap publishes it without a relaunch).
void EditorApp::RunGiLayoutOptimizer(uint32_t node) {
	if (m_giLayoutRunning)
		return;
	if (m_bakeRunning || m_cookRunning) {
		Log("Optimize grid: a cook/bake is running - wait for it.");
		return;
	}
	if (!m_world || node >= m_world->scene.hierarchy.size())
		return;
	const GiVolumeRow* it = FindGiVolume(m_world->scene, node);
	if (!it)
		return;
	const ::SceneGiVolume cv = it->volume;
	// The gates; authored, else the contract default (+-20% around the
	// preferred spacing).
	float low = cv.spacingLow;
	float high = cv.spacingHigh;
	if (!(low > 0.0f)) {
		low = cv.spacing * 0.8f;
		high = cv.spacing * 1.2f;
	}
	low = std::max(low, 0.01f);
	high = std::max(high, low);
	// The volume OBB from the LIVE node transform (the definition bind's
	// own decode: half extents from the basis lengths, the axes
	// normalized — the same math the cook/GiBaker path applies to the
	// replayed node, so the optimizer scores the shape that will bake).
	const glm::mat4& g = m_world->scene.globalTransform[node];
	const glm::vec3 c(g[3]);
	glm::vec3 half;
	glm::mat3 axes;
	for (int i = 0; i < 3; ++i) {
		const float len = glm::length(g[i]);
		half[i] = 0.5f * len;
		axes[i] = (len > 1e-6f) ? glm::vec3(g[i]) / len : glm::vec3(0.0f);
	}
	smvk::gi::GiVolumeDef def;
	def.planes = smvk::gi::GiVolumeBoxPlanesRotated(
		c, axes, half, cv.insideFade, cv.outsideFade);
	def.priority = cv.priority;
	// THE GEOMETRY SNAPSHOT (main thread: it walks the world's node +
	// location tables, which the UI thread owns). The BVH build is the
	// expensive half and runs on the fiber below — the extraction is a
	// plain copy of the already-resident vertex/index data.
	TaskWindowBegin("Optimize grid",
		"Snapshotting scene geometry (live world - no cook)...");
	smvk::gi::GiSceneBuildOptions buildOpts{};
	buildOpts.staticLights = false; // BVH-only consumer: no light harvest
	auto traceScene = std::make_shared<smvk::gi::GiTraceScene>();
	std::string snapErr;
	smvk::gi::ExtractTraceSceneFromWorld(*m_world, buildOpts, *traceScene,
		snapErr);
	if (!snapErr.empty()) {
		Log("Optimize grid failed: " + snapErr);
		TaskWindowFinish(false, ("Optimize grid FAILED (" + snapErr + ")").c_str());
		return;
	}
	if (traceScene->positions.empty()) {
		const std::string e =
			"the live scene has no retained geometry to trace against";
		Log("Optimize grid failed: " + e);
		TaskWindowFinish(false, ("Optimize grid FAILED (" + e + ")").c_str());
		return;
	}
	m_giLayoutRunning = true;
	TaskWindowStage("Optimizing GI probe grid layout (BVH search)...");
	Log("Optimize grid: gates " + std::to_string(low) + ".."
		+ std::to_string(high) + " m (live world snapshot, "
		+ std::to_string(traceScene->positions.size()) + " verts)");
	auto result = std::make_shared<smvk::gi::GiGridLayoutResult>();
	auto okFlag = std::make_shared<bool>(false);
	auto errStr = std::make_shared<std::string>();
	m_fibers.Post("gi-layout-opt",
		[traceScene, def, low, high, result, okFlag, errStr]() {
			bool ok = false;
			std::string err;
			try {
				// The BVH build (the snapshot's Finalize) + the search.
				// Both are pure functions of the moved soup: no live
				// world access on this thread.
				traceScene->Finalize();
				smvk::gi::GiGridLayoutOptions opts{};
				ok = smvk::gi::ChooseGiVolumeGridLayout(
					traceScene->Bvh(), def, low, high, opts, *result, &err);
			} catch (const std::exception& e) {
				err = std::string("layout job: ") + e.what();
			}
			*okFlag = ok;
			*errStr = err;
		},
		[this, node, result, okFlag, errStr](
				const editor::FiberBackend::Completion& c) {
			m_giLayoutRunning = false;
			if (!c.ok) {
				Log("Optimize grid: job error: " + c.error);
				TaskWindowFinish(false,
					"Optimize grid FAILED (job error)");
				return;
			}
			if (!*okFlag) {
				Log("Optimize grid failed: " + *errStr);
				TaskWindowFinish(false,
					("Optimize grid FAILED (" + *errStr + ")").c_str());
				return;
			}
			const smvk::gi::GiGridLayoutResult& r = *result;
			// ONE undoable "optlayout" commit (spacing + phase).
			{
				// beginGiVolumeDrag's pattern, inline (editor_ui.cpp owns
				// the lambda; this is the same pending machinery).
				if (m_pending.active)
					FinalizePending();
				m_pending.active = true;
				m_pending.cmd = EditCmd{};
				m_pending.cmd.verb = EditVerb::SetGiVolumeAttr;
				m_pending.cmd.node = node;
				m_pending.cmd.giVolumeAttr = "optlayout";
				ReadNodeGiVolumeAttr(node, "optlayout",
					m_pending.cmd.beforeVec4, m_pending.cmd.beforeNum,
					m_pending.cmd.beforeText);
				WriteNodeGiVolumeAttr(node, "optlayout",
					glm::vec4(r.phase[0], r.phase[1], r.phase[2], 0.0f),
					double(r.spacing), {});
				m_pending.cmd.vec4 = glm::vec4(r.phase[0], r.phase[1],
					r.phase[2], 0.0f);
				m_pending.cmd.number = double(r.spacing);
				FinalizePending();
			}
			m_giLayoutLast.node = node;
			m_giLayoutLast.valid = true;
			m_giLayoutLast.result = r;
			char buf[256];
			std::snprintf(buf, sizeof(buf),
				"Optimize grid: spacing %.3f m, phase (%.2f,%.2f,%.2f), "
				"dims %ux%ux%u, probes %llu, invalid %llu, "
				"coverage %.1f%% (seeded - rebakes use it)",
				r.spacing, r.phase[0], r.phase[1], r.phase[2],
				r.dims[0], r.dims[1], r.dims[2],
				(unsigned long long)r.probes,
				(unsigned long long)r.invalid, 100.0 * r.coverage);
			Log(buf);
			TaskWindowFinish(true, buf);
		});
}

void EditorApp::PollCookProcess() {
	if (!m_cookRunning)
		return;
	// Live tool feed drain ONLY (the finish logic rides the fibers
	// completion callback — see CookToPak). The Console keeps the
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
}

// ---- Bake GI on demand (never part of the cook) --------------------
// GiBaker consumes the SAME definition-tour snapshot a cook would
// (root reference + nodes), keyed to the cook's OUT-PAK STEM so the
// bake files land exactly where the editor/App loaders look:
// gi/<stem>_*.givolume + .givolumemanifest. Missing bake files are
// NOT an error anywhere down the line — every GI/volume/cubemap/
// lightmap loader falls back to the analytic sky IBL (see
// DegradeAmbientForMissingSources in app). (The gi/<stem>.sggi world
// field is retired with the octree.)

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
	const bool giHasGiVolumes = !m_world->scene.tables.giVolumes.empty();
	const bool wantGiVolumes = ExportCfg().giVolumes && giHasGiVolumes;
	if (!wantGiVolumes) {
		Log("Bake GI: nothing requested — enable GI volumes in the cook "
			"dialog first (the world-field octree bake is retired; "
			"convex volumes + the sky IBL carry the ambient).");
		return;
	}
	// Bake files key to the cook target's stem; derive the project (or
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
	// The bake artifacts: RANDOM-MINTED paths (GiBaker mints each GI
	// volume). The binding record IS the identity — no stem-keyed file
	// name anywhere; the stamp carries the minted paths + retires the
	// superseded old artifacts BY RECORD. contentRoot = the "content/"
	// dir itself (bake files land in gi/ next to the pak — exactly what
	// the loaders check).
	const std::string contentRoot = std::filesystem::absolute(
		std::filesystem::path(m_cook.outPak).parent_path() / "..")
		.lexically_normal().string();
	m_giBakePendingHash = defHash; // adopted as m_giBakedHash on success
	m_bakeRunning = true;
	// Blocking progress surface (modal task window).
	TaskWindowBegin("Bake GI", "Baking GI (GI volumes)...");
	Log("Bake GI started (GI volumes) -> "
		+ contentRoot + "/gi/ (minted random artifacts)");
 	auto feed = std::make_shared<CookFeed>();
 	m_bakeFeed = feed;
	// Fibers job (the editor's single off-main-thread lane): the old
	// std::async worker verbatim, sharing a result slot with the
	// completion callback that runs ON MAIN via m_fibers.Pump() (the old
	// future.wait_for branch in PollBakeProcess).
	auto result = std::make_shared<CookResult>();
	m_fibers.Post("bake-gi",
		[contentRoot, feed, tempTourPath = std::move(tempTourPath), result]() {
			CookResult r{};
			auto push = [feed](const std::string& line, bool err = false) {
				std::lock_guard<std::mutex> lk(feed->mutex);
				feed->lines.push_back(line);
				if (err) {
					feed->errLines.push_back(line);
					if (feed->errLines.size() > 8)
						feed->errLines.erase(feed->errLines.begin());
				}
			};
			// GiBaker runs IN-PROCESS (GiBakerLib): the CLI stays a thin
			// user interface over the same Run. Baker lines stream
			// through the tool line sink into this feed (err lines also
			// land in the bounded errLines tail for the failure report).
		gibaker::Args gargs;
		gargs.contentDir = contentRoot;
		gargs.tour = tempTourPath;
		push("bake-gi: GiBaker (in-process)");
			tool::SetLineSink([&push](const std::string& line, bool err) {
				push(line, err);
			});
		const int grc = gibaker::Run(gargs);
		tool::SetLineSink(nullptr);
		// The stamped records ride the result BEFORE the snapshot dies:
		// the bake wrote the giAsset bindings into the temp tour (its
		// stampTour, supersede-deletion included), so read them back
		// (the bake owner is the one authority; the completion folds
		// them into the live-doc mirror only when the bake succeeded).
		if (grc == 0) {
			smvk::tour::Doc stamped;
			std::string lerr;
			std::vector<std::string> lwarn;
			if (smvk::tour::LoadTour(tempTourPath, stamped, lerr,
					&lwarn)) {
				r.giAssets = std::move(stamped.giAssets);
			}
			else {
				push("bake-gi: snapshot record read failed ("
					+ lerr + ") — the live doc keeps its records", true);
			}
		}
		// Temp tour snapshot cleanup (was deleted by the spawn
		// path's early returns; single exit now).
			{
				std::error_code ec;
				std::filesystem::remove(tempTourPath, ec);
			}
			r.ok = (grc == 0);
			r.log = "bake-gi: GiBaker exit=" + std::to_string(grc) + "\n";
			if (!r.ok)
				r.log += "bake-gi: GI bake FAILED (previous bake "
					"files, if any, are untouched; unbaked scenes render "
					"sky IBL)\n";
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
			// The bake's giAsset records into the LIVE-doc mirror (the
			// bake owner stamped the snapshot; the job read them back).
			// Same OWNER'S DECISION as the cook's fold (see the
			// completion note above): derived state, one atomic
			// success-only assignment, never an undo step — undo would
			// re-point the doc at superseded (deleted) bake artifacts.
			if (!r.giAssets.empty()) {
				m_giAssets = std::move(r.giAssets);
				Log("Bake GI: tour giAsset bindings updated ("
					+ std::to_string(m_giAssets.size())
					+ " record(s)) — saved with the scene");
			}
			// The bake files now match the baked definition: record it so a
			// later cook can warn when they go stale.
			m_giBakedHash = m_giBakePendingHash;
				Log("Bake GI finished OK — bake files at their stable "
				"paths.");
				TaskWindowFinish(true, "Bake GI finished OK.");
			}
		else {
			// Self-describing failure (same contract as the cook's):
			// the GiBaker exit rides the notice itself.
			std::string failSummary;
			std::istringstream ts(r.log);
			std::string tl;
			while (std::getline(ts, tl))
				if (!tl.empty())
					failSummary += (failSummary.empty() ? "" : "; ") + tl;
			{
				std::shared_ptr<CookFeed> feed = m_bakeFeed;
				if (feed) {
					std::lock_guard<std::mutex> lk(feed->mutex);
					for (const std::string& e : feed->errLines)
						failSummary += "\n  " + e;
				}
			}
			if (failSummary.empty())
				failSummary = "no stage exit recorded (job thrown?)";
			Log("Bake GI FAILED (" + failSummary + "; detail above).");
			TaskWindowFinish(false, "Bake GI FAILED (" + failSummary + ")");
		}
			m_giBakePendingHash.clear();
		});
}

void EditorApp::PollBakeProcess() {
	if (!m_bakeRunning)
		return;
	// Live tool feed drain ONLY (the finish logic rides the fibers
	// completion callback — see BakeGI). Console record + modal task
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
