// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The project build orchestrator (see build_pipeline.h). Stage tools are
// resolved beside THIS executable (the build root — every tool shares
// RUNTIME_OUTPUT_DIRECTORY) and run as subprocesses with exit-code
// propagation. The build SETTINGS (stage flags) live in the project's
// .tourmaline/build-settings.txt — persisted, never duplicated in a
// script; missing keys keep the defaults below.

#include <project_pipeline/build_pipeline.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

#include <Engine/Asset/appmanifest.h>
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/pak.h>
#include <Engine/Asset/tour.h>

#include <artifact_cache/assetdb.h>
#include <artifact_cache/artifact_cache.h>

#include <project_pipeline/import_pipeline.h>
#include <project_pipeline/project.h>
#include <project_pipeline/project_catalog.h>

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

// ---- the shared build-plan executor ------------------------------------

namespace {

// The CLI adapter's exe names: the tools share the build root
// (RUNTIME_OUTPUT_DIRECTORY), resolved beside THIS executable — the
// same contract the legacy BuildProject RunStage calls rely on.
// Windows exes carry .exe; the POSIX tool targets do not.
#ifdef _WIN32
std::string StageExe(const char* base) { return std::string(base) + ".exe"; }
#else
std::string StageExe(const char* base) { return base; }
#endif

// The subprocess adapter: flag-for-flag the legacy BuildProject
// invocations (the M2 parity contract — a single-pak plan through this
// adapter IS the legacy build, sequenced by the plan instead of the
// hardcoded steps).
class SubprocessStageAdapter final : public StageAdapter {
public:
	int Cook(const CookStageArgs& a, std::string& err) override {
		return RunStage(StageExe("Cook").c_str(),
			{ a.contentDir, "--tour", a.tour,
				"--cubemap", a.cubemap ? "on" : "off",
				"--lightmap", a.lightmap ? "on" : "off" },
			err);
	}
	int GiBaker(const GiBakerStageArgs& a, std::string& err) override {
		return RunStage(StageExe("GiBaker").c_str(),
			{ "--content-dir", a.contentDir, "--tour", a.tour }, err);
	}
	int Pak(const PakStageArgs& a, std::string& err) override {
		// inVfs + extras are POSITIONAL (root, then artifacts); an
		// asset-only pak has NEITHER (an empty positional would smuggle
		// a "" artifact into the CLI's list).
		std::vector<std::string> args = { a.contentDir };
		if (!a.inVfs.empty()) {
			args.push_back(a.inVfs);
			for (const std::string& extra : a.extraArtifacts)
				args.push_back(extra);
		}
		if (!a.tour.empty()) {
			args.push_back("--tour");
			args.push_back(a.tour);
		}
		for (const std::string& path : a.contentSet) {
			args.push_back("--content-set");
			args.push_back(path);
		}
		for (const std::string& path : a.excludeVfs) {
			args.push_back("--exclude-vfs");
			args.push_back(path);
		}
		args.push_back("--out-pak");
		args.push_back(a.outPak);
		if (!a.levelName.empty()) {
			args.push_back("--level");
			args.push_back(a.levelName);
		}
		if (a.verify) {
			args.push_back("--verify");
			args.push_back("on");
		}
		// (registryDir is the IN-PROCESS adapter's surface — the CLI
		// derives its cache by convention; the editor's explicit store
		// rides the library adapter. Output is byte-identical either
		// way, so CLI-vs-editor parity holds.)
		return RunStage(StageExe("Pak").c_str(), args, err);
	}
};

std::string StageFail(const char* stage, int rc, const std::string& err) {
	return err.empty()
		? ("the " + std::string(stage) + " stage failed (exit "
			+ std::to_string(rc) + ")")
		: ("the " + std::string(stage) + " stage failed: " + err);
}

} // namespace

std::unique_ptr<StageAdapter> MakeSubprocessStageAdapter() {
	return std::make_unique<SubprocessStageAdapter>();
}

bool RefreshPlanImports(const Project& project, const BuildPlan& plan,
	std::set<std::string>* reimportedFamilies, std::string& err) {
	err.clear();
	if (reimportedFamilies)
		reimportedFamilies->clear();
	if (!project.Valid()) {
		err = "invalid project (open/init first)";
		return false;
	}
	std::error_code ec;
	const fs::path projectRoot(project.root);
	// Every family the plan packs: scene artifacts + owned Geometry
	// assets. The legacy path covered the root reference only; the plan
	// walks them all. Stale -> the canonical ImportProjectAsset.
	std::set<std::string> families;
	for (const PlanPak& pak : plan.paks) {
		for (const std::string& a : pak.sceneArtifacts)
			families.insert(a);
		for (const PlanAsset& asset : pak.assets)
			if (asset.kind == PlanAssetKind::Geometry)
				families.insert(asset.path);
	}
	std::set<std::string> reimported;
	for (const std::string& artifact : families) {
		const fs::path artifactAbs = projectRoot / fs::path(artifact);
		// The family's source glTF/GLB: beside a .geometry artifact, or
		// the artifact itself when the tour referenced the raw source.
		std::vector<fs::path> candidates;
		const std::string ext = artifactAbs.extension().generic_string();
		if (ext == ".gltf" || ext == ".glb")
			candidates.push_back(artifactAbs);
		else
			for (const char* e : { ".gltf", ".glb" }) {
				fs::path src = artifactAbs;
				src.replace_extension(e);
				candidates.push_back(src);
			}
		for (const fs::path& srcAbs : candidates) {
			if (!fs::is_regular_file(srcAbs, ec))
				continue;
			if (smvk::assets::MeshArtifactStale(srcAbs,
					GeometryImportSettings{})) {
				ImportRequest req;
				req.project = project;
				req.source = srcAbs;
				req.settings = GeometryImportSettings{};
				ImportResult result;
				std::string iErr;
				if (!ImportProjectAsset(req, result, iErr)) {
					err = "reimport failed: " + iErr;
					return false;
				}
				if (result.derived) {
					reimported.insert(artifact);
					std::printf("[build] reimported: %s\n",
						result.geometry.generic_string().c_str());
				}
			}
			break; // one family per artifact (the first existing source)
		}
	}
	if (reimportedFamilies)
		*reimportedFamilies = std::move(reimported);
	return true;
}

bool ExecuteBuildPlan(const Project& project, const BuildPlan& plan,
	StageAdapter& adapter, const BuildPlanOptions& options,
	BuildPlanReport& out, std::string& err) {
	err.clear();
	out = BuildPlanReport{};
	if (!project.Valid()) {
		err = "invalid project (open/init first)";
		return false;
	}
	if (plan.paks.empty() || plan.entryPak.empty()) {
		err = "the plan is empty (resolve a recipe first)";
		return false;
	}
	std::error_code ec;
	const fs::path projectRoot(project.root);

	// ---- 0) the publish directory --------------------------------------
	fs::path publish = options.publishDir.empty()
		? projectRoot / "build"
		: fs::path(options.publishDir);
	if (publish.is_relative())
		publish = projectRoot / publish;
	fs::create_directories(publish, ec);

	// ---- 1) import freshness (in-process, every family the plan packs) -
	std::set<std::string> reimportedFamilies;
	if (!RefreshPlanImports(project, plan, &reimportedFamilies, err))
		return false;

	// ---- 2) per scene pak: Cook + GiBaker (plan order) ------------------
	// The bake flags come from the RECIPE (the plan), never
	// build-settings.txt — a recipe build is fully described by its
	// recipe. 'bake gi off' skips the GiBaker stage outright; 'bake
	// cubemaps off' passes --cubemap off to Cook.
	for (const PlanPak& pak : plan.paks) {
		if (pak.scene.empty())
			continue; // asset-only pak: no scene bakes
		// The tour the stages run: the authored doc, or the editor's
		// UNSAVED-SCENE SNAPSHOT (the plan resolved against the authored
		// file; the snapshot substitutes for every stage — the authored
		// document is never touched by a cook).
		fs::path tourAbs = options.sceneSnapshot.empty()
			? projectRoot / fs::path(pak.scene)
			: fs::path(options.sceneSnapshot);
		if (!fs::is_regular_file(tourAbs, ec)) {
			err = "[Cook] the scene tour is missing: "
				+ tourAbs.lexically_normal().generic_string();
			return false;
		}
		{
			CookStageArgs stage;
			stage.contentDir = project.root;
			stage.tour = tourAbs.lexically_normal().generic_string();
			stage.cubemap = plan.bakeCubemaps;
			stage.lightmap = false; // the deprecated path stays off
			stage.registryDir = options.registryDir;
			const int rc = adapter.Cook(stage, err);
			if (rc != 0) {
				err = StageFail("Cook", rc, err);
				return false;
			}
		}
		if (plan.bakeGi) {
			GiBakerStageArgs stage;
			stage.contentDir = project.root;
			stage.tour = tourAbs.lexically_normal().generic_string();
			const int rc = adapter.GiBaker(stage, err);
			if (rc != 0) {
				err = StageFail("GiBaker", rc, err);
				return false;
			}
		}
	}

	// ---- 3) per pak: Pak (plan order = dependencies first) --------------
	// The ownership tables the stages + the policing need: which pak owns
	// which VFS-addressable path (the resolver's one-owner invariant).
	std::map<std::string, std::string> vfsOwner; // path -> pak id
	for (const PlanPak& pak : plan.paks)
		for (const PlanAsset& asset : pak.assets)
			if (asset.kind == PlanAssetKind::Texture
					|| asset.kind == PlanAssetKind::BakeFile)
				vfsOwner[asset.path] = pak.id;

	for (const PlanPak& pak : plan.paks) {
		PakStageArgs stage;
		stage.contentDir = project.root;
		stage.outPak = (publish / fs::path(pak.output))
			.lexically_normal().generic_string();
		stage.verify = true;
		if (pak.scene.empty()) {
			// ASSET-ONLY pak (the recipe spec's shared package): the
			// owned VFS assets embed directly as Texture blobs — no
			// scene, no clusters. (The resolver rejects Geometry claims
			// on scene-less paks: meshes are cluster-internal.)
			for (const PlanAsset& asset : pak.assets)
				stage.contentSet.push_back(asset.path);
		}
		else {
			if (pak.sceneArtifacts.empty()) {
				err = "[Pak " + pak.id + "] the scene pak has no root"
					" artifact (a broken plan — re-resolve)";
				return false;
			}
			for (const std::string& artifact : pak.sceneArtifacts)
				if (!fs::is_regular_file(
						projectRoot / fs::path(artifact), ec)) {
					err = "[Pak " + pak.id + "] the artifact is missing: "
						+ artifact;
					return false;
				}
			stage.inVfs = pak.sceneArtifacts[0];
			// Non-root references are INLINED by the tour replay (the
			// packer's tour mode builds ONE cluster from the whole
			// reference walk) — positionals are identity only, so the
			// root artifact is the whole story (legacy parity: BakeStems
			// keys bake discovery on these stems).
			stage.tour = (options.sceneSnapshot.empty()
					? (projectRoot / fs::path(pak.scene))
					: fs::path(options.sceneSnapshot))
				.lexically_normal().generic_string();
			stage.levelName = options.levelName;
			stage.registryDir = options.registryDir;
			// The cross-pak excludes: every VFS-addressable asset owned
			// by a DECLARED dependency pak is not embedded here — the
			// materials keep their refs, the runtime resolves them
			// through the dependency's mounted backend (pak.h's
			// interop contract: PakBackend serves Texture blobs by VFS
			// path).
			std::set<std::string> depIds(pak.depends.begin(),
				pak.depends.end());
			for (const PlanPak& other : plan.paks) {
				if (!depIds.count(other.id))
					continue;
				for (const PlanAsset& asset : other.assets)
					if (asset.kind == PlanAssetKind::Texture
							|| asset.kind == PlanAssetKind::BakeFile)
						stage.excludeVfs.push_back(asset.path);
			}
		}
		std::error_code mkEc;
		fs::create_directories(fs::path(stage.outPak).parent_path(), mkEc);
		const int rc = adapter.Pak(stage, err);
		if (rc != 0) {
			err = StageFail("Pak", rc, err)
				+ "; a stale bake was refused, or an asset failed — see"
					" the stage output above";
			// A failed pak never replaces the last valid one: the stage
			// writes its output only on success (verify on).
			return false;
		}
		if (!fs::is_regular_file(stage.outPak, ec)) {
			err = "[Pak " + pak.id + "] the stage reported success but"
				" the output is missing: " + stage.outPak;
			return false;
		}
		// ---- OWNERSHIP POLICING (RES-04 at the pack boundary) ----------
		// Re-open the written pak and verify its Texture-kind TOC never
		// carries a path another pak owns — with the excludes above a
		// dependency-owned path cannot embed, so a hit here is an
		// undeclared cross-pak reference (the author referenced shared
		// content without the depends edge) or a packer regression.
		// Either way the build fails BEFORE publication, never ships a
		// duplicate.
		{
			std::ifstream in(stage.outPak, std::ios::binary);
			std::vector<uint8_t> image((std::istreambuf_iterator<char>(in)),
				std::istreambuf_iterator<char>());
			std::string perr;
			auto reader = smvk::PakReader::Open(
				std::span<const uint8_t>(image), perr);
			if (!reader) {
				err = "[Pak " + pak.id + "] policing open failed: "
					+ perr;
				return false;
			}
			for (const smvk::PakTocEntry* e :
					reader->BlobsOfKind(smvk::PakAssetKind::Texture)) {
				auto ownerIt = vfsOwner.find(e->path);
				if (ownerIt != vfsOwner.end()
						&& ownerIt->second != pak.id) {
					err = "[Pak " + pak.id + "] embeds '" + e->path
						+ "' owned by [pak " + ownerIt->second + "] —"
							" a duplicated/undeclared cross-pak asset"
							" (add the depends edge or fix the"
							" ownership)";
					return false;
				}
			}
		}
		BuildPlanReport::Pak rep;
		rep.id = pak.id;
		rep.output = stage.outPak;
		rep.scene = pak.scene;
		for (const std::string& a : pak.sceneArtifacts)
			if (reimportedFamilies.count(a)) {
				rep.reimported = true;
				break;
			}
		out.paks.push_back(std::move(rep));
	}

	// ---- 4) publish the appmanifest (LAST, after every pak verified) --
	// The manifest names the entry pak + the entry scene's cooked
	// identity + the ordered pak list (mount order = plan order). Every
	// pak reopened + policed above; only now does the manifest land, so
	// a failure anywhere above leaves the PREVIOUS manifest + its paks
	// standing (the publish-last contract).
	{
		const PlanPak* entryPak = plan.FindPak(plan.entryPak);
		const std::string entryOut = entryPak
			? (publish / fs::path(entryPak->output)).lexically_normal()
				.generic_string()
			: std::string();
		if (entryOut.empty() || !fs::is_regular_file(entryOut, ec)) {
			err = "the entry pak '" + plan.entryPak
				+ "' did not build (cannot publish the manifest)";
			return false;
		}
		// The cooked entry-scene identity: list the entry pak's scenes
		// and record the tour scene's NAME (tour-built paks carry one
		// scene; the runtime resolves by name, never by index).
		std::string entrySceneName;
		{
			std::ifstream in(entryOut, std::ios::binary);
			std::vector<uint8_t> image((std::istreambuf_iterator<char>(in)),
				std::istreambuf_iterator<char>());
			std::string rerr;
			auto reader = smvk::PakReader::Open(
				std::span<const uint8_t>(image), rerr);
			if (!reader) {
				err = "the entry pak failed to reopen for the manifest: "
					+ rerr;
				return false;
			}
			std::string lerr;
			const std::vector<smvk::PakSceneInfo> scenes =
				smvk::ListPakScenes(*reader, &lerr);
			if (scenes.empty()) {
				err = "the entry pak carries no scenes: " + lerr;
				return false;
			}
			entrySceneName = scenes[0].name;
			if (scenes.size() > 1)
				std::printf("[build] entry pak carries %zu scenes;"
					" recording scene[0] '%s' as the entry\n",
					scenes.size(), entrySceneName.c_str());
		}
		if (entrySceneName.empty())
			entrySceneName = "scene"; // the packer's tour-scene name
		smvk::AppManifest manifest;
		manifest.version = smvk::kAppManifestVersion;
		manifest.entryPak = entryPak->output;
		manifest.entryScene = entrySceneName;
		for (const PlanPak& pak : plan.paks)
			manifest.paks.push_back(pak.output);
		// The content-id table (the runtime mirror of the generated
		// project.h — game code resolves stable ids through the
		// apphost's ProjectRuntime without the header). The SAME
		// registry (assets.db — the one identity store) the ids
		// command maintains; ensuring here is idempotent, so a build
		// alone is sufficient and the manifest always carries the
		// current catalog.
		{
			const std::string registryRoot =
				smvk::cache::EnsureRegistryRoot(project.DerivedDataDir());
			smvk::cache::AssetDatabase db;
			if (!registryRoot.empty()
					&& db.Open(registryRoot, project.root, err)) {
				ProjectCatalog catalog;
				std::vector<std::string> deps;
				if (GenerateProjectCatalog(project, plan, db, catalog,
						deps, err)) {
					std::map<std::string, std::string> pakFileById;
					for (const PlanPak& pak : plan.paks)
						pakFileById[pak.id] = pak.output;
					const auto addRows =
						[&](const std::vector<CatalogEntry>& entries,
							const char* kind) {
							for (const CatalogEntry& e : entries) {
								smvk::AppManifestAsset row;
								row.id = e.id;
								row.kind = kind;
								row.pak = pakFileById.count(e.pak)
									? pakFileById[e.pak] : e.pak;
								row.path = e.path;
								manifest.assets.push_back(
									std::move(row));
							}
						};
					addRows(catalog.scenes, "scene");
					addRows(catalog.textures, "texture");
					addRows(catalog.meshes, "mesh");
				}
				else {
					// The manifest's id table is part of the build's
					// product now — a registry failure fails the build.
					err = "the manifest's content-id table failed: "
						+ err;
					return false;
				}
			}
			else {
				err = "the asset db open failed for the content-id"
					" table: " + err;
				return false;
			}
		}
		std::string manifestName = options.manifestName;
		if (manifestName.empty()) {
			manifestName = fs::path(entryPak->output).stem()
				.generic_string();
		}
		const fs::path manifestPath =
			publish / (manifestName + ".tapp");
		std::string mErr;
		if (!smvk::SaveAppManifest(manifestPath.generic_string(),
				manifest, mErr)) {
			err = "the appmanifest publish failed: " + mErr;
			return false;
		}
		out.manifest = manifestPath.lexically_normal().generic_string();
		std::printf("[build] published appmanifest: %s (entry %s:%s,"
			" %zu pak(s))\n", out.manifest.c_str(),
			manifest.entryPak.c_str(), manifest.entryScene.c_str(),
			manifest.paks.size());
	}
	return true;
}

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
