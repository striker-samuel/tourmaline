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
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <random>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

#include <Engine/Asset/appmanifest.h>
#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/pak.h>
#include <Engine/Asset/sdf.h>
#include <Engine/Asset/tour.h>

#include <artifact_cache/assetdb.h>
#include <artifact_cache/artifact_cache.h>

#include <gltf_import/source_formats.h>
#include <gltf_import/asset_export.h>

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
		for (const auto& [path, id] : a.soundIds) {
			args.push_back("--sound-id");
			args.push_back(path);
			char buf[32];
			std::snprintf(buf, sizeof(buf), "0x%llX",
				static_cast<unsigned long long>(id));
			args.push_back(buf);
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

// Packing rules are project-wide: if the recipe changes, regenerate
// material assets even when the source model fingerprint is unchanged.
bool RecipePackingMatches(const fs::path& source,
    const MaterialPackingRule& rule) {
    smvk::assets::AssetManifest manifest;
    std::string err;
    if (!smvk::assets::LoadAssetManifest(source, manifest, err))
        return false;
    for (const auto& entry : manifest.entries) {
        if (entry.kind != smvk::assets::AssetKind::Material)
            continue;
        smvk::assets::MaterialAsset mat;
        if (!smvk::assets::LoadMaterialAsset(
                source.parent_path() / entry.file, mat, err))
            return false;
        if (!MaterialPackingRuleMatches(rule, mat))
            return false;
    }
    return true;
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
		// The family's source (a mesh source format — .gltf/.glb/.obj):
		// beside a .geometry artifact, or the artifact itself when the
		// tour referenced the raw source.
		std::vector<fs::path> candidates;
		if (smvk::assets::IsMeshSourceFile(artifactAbs))
			candidates.push_back(artifactAbs);
		else
			for (const char* e : { ".gltf", ".glb", ".obj" }) {
				fs::path src = artifactAbs;
				src.replace_extension(e);
				candidates.push_back(src);
			}
		for (const fs::path& srcAbs : candidates) {
			if (!fs::is_regular_file(srcAbs, ec))
				continue;
			if (smvk::assets::MeshArtifactStale(srcAbs,
					GeometryImportSettings{}) ||
                    !RecipePackingMatches(srcAbs, plan.materialPacking)) {
				ImportRequest req;
				req.project = project;
				req.source = srcAbs;
				req.settings = GeometryImportSettings{};
				req.materialPacking = plan.materialPacking;
				req.force = true;
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

// Mesh-local distance fields are DERIVED from already imported .geometry,
// after any Cook writeback and before pak assembly. Each geometry family
// owns one sibling .geometry.sdf, containing all base primitives (not the
// LOD chain). Re-running the recipe deterministically replaces the sibling.
static bool BakePlanSdfs(const Project& project, const BuildPlan& plan,
	std::string& err) {
	std::set<std::string> families;
	for (const PlanPak& pak : plan.paks)
		for (const PlanAsset& a : pak.assets)
			if (a.kind == PlanAssetKind::Geometry)
				families.insert(a.path);
	for (const PlanPak& pak : plan.paks)
		for (const std::string& p : pak.sceneArtifacts)
			families.insert(p);
	for (const std::string& name : families) {
		const fs::path source = fs::path(project.root) / fs::path(name);
		smvk::assets::MeshAsset mesh;
		if (!smvk::assets::LoadMeshAsset(source, mesh, err)) {
			err = "SDF cannot read '" + name + "': " + err;
			return false;
		}
		// (::tour::asset — the leading :: is required inside smvk::project,
		// where tour.h's `namespace tour = ::tour::asset` alias shadows the
		// global tour namespace).
		::tour::asset::SdfAsset baked;
		for (const auto& geometry : mesh.geometry) {
			if (geometry.indices.empty() || geometry.positions.empty())
				continue;
			std::vector<glm::vec3> positions;
			positions.reserve(geometry.positions.size());
			for (const auto& v : geometry.positions)
				positions.push_back(v.pos);
			::tour::asset::SdfMesh entry;
			entry.fragment = geometry.uriFragment;
			if (!::tour::asset::BakeMeshSdf(positions.data(), positions.size(),
					geometry.indices.data(), geometry.indices.size(),
					::tour::asset::SdfBakeOptions{}, entry.volume, err)) {
				err = "SDF bake '" + name + "#" + entry.fragment + "': " + err;
				return false;
			}
			baked.meshes.push_back(std::move(entry));
		}
		const std::vector<uint8_t> bytes = ::tour::asset::WriteSdfAsset(baked);
		const fs::path dest = source.generic_string() + ".sdf";
		const fs::path temp = dest.generic_string() + ".tmp";
		{
			std::ofstream f(temp, std::ios::binary | std::ios::trunc);
			f.write(reinterpret_cast<const char*>(bytes.data()),
				static_cast<std::streamsize>(bytes.size()));
			f.close();
			if (!f) {
				err = "SDF sidecar write failed: " + temp.generic_string();
				return false;
			}
		}
		std::error_code ec;
		// Windows rename does not replace; only remove the old derived
		// file once the NEW temporary output has been fully written.
		fs::remove(dest, ec);
		ec.clear();
		fs::rename(temp, dest, ec);
		if (ec) {
			err = "SDF sidecar publish failed: " + dest.generic_string()
				+ ": " + ec.message();
			return false;
		}
	}
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
	if (ec) { err = "cannot create publication directory: " + ec.message(); return false; }

	// In-process import, derived-data writes and cooking all share the
	// project registry. Serialize a project build from the FIRST write,
	// not merely the final manifest rename, to prevent mixed generations.
	// The lock intentionally fails closed after a hard crash: recovery is
	// an explicit operator action after verifying no other builder runs.
	struct ProjectBuildLock {
		fs::path directory;
		~ProjectBuildLock() {
			if (directory.empty()) return;
			std::error_code ignored;
			fs::remove(directory, ignored);
		}
	} buildLock;
	{
		const fs::path derived(project.DerivedDataDir());
		std::error_code lockEc;
		fs::create_directories(derived, lockEc);
		if (lockEc) { err = "cannot initialize derived directory: " + lockEc.message(); return false; }
		const fs::path lockDir = derived / ".build.lock";
		if (!fs::create_directory(lockDir, lockEc)) {
			err = "project build already active or stale .build.lock; "
				"verify no builder is running before manual recovery: "
				+ lockDir.generic_string();
			return false;
		}
		buildLock.directory = lockDir;
	}

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

	// ---- 2.25) optional mesh-local SDF bake -----------------------------
	// Sidecars belong to their .geometry family, never to a scene or
	// runtime mutable mesh. The original render meshes stay unchanged.
	if (plan.bakeSdf && !BakePlanSdfs(project, plan, err))
		return false;

	// ---- 2.5) the pre-stage catalog ensure --------------------------------
	// The cooked scenes' audio sections stamp STABLE 0x4xxx ids, so the
	// identity pass runs BEFORE the pak stages (idempotent; on a FIRST
	// build THIS is what mints the .wav sidecars + ids the packer
	// consumes). A plan with no sound assets yields an empty table =
	// the bare-cook semantic (referenced sounds serialize 0).
	// The catalog stays in scope below: the per-pak IDENTITY-TABLE
	// append (the "#ids" registry blob) + the post-publish project.h
	// re-stamp read the same rows, and the project's node schemas ride
	// it (the generated project::types + the content-hash fold — the
	// same collection the ids pass runs).
	std::vector<std::pair<std::string, uint64_t>> soundIds;
	ProjectCatalog catalog;
	{
		const std::string registryRoot =
			smvk::cache::EnsureRegistryRoot(project.DerivedDataDir());
		smvk::cache::AssetDatabase db;
		if (!registryRoot.empty()
				&& db.Open(registryRoot, project.root, err)) {
			std::vector<std::string> deps;
			if (!GenerateProjectCatalog(project, plan, db, catalog,
					deps, err)
				|| !CollectNodeSchemas(project.root, catalog.schemas,
					err)) {
				err = "the pre-stage catalog failed: " + err;
				return false;
			}
			for (const CatalogEntry& e : catalog.sounds)
				soundIds.emplace_back(e.path, e.id);
		}
		else {
			err = "the asset db open failed for the sound-id table: "
				+ err;
			return false;
		}
	}

	// ---- 3) per pak: Pak (plan order = dependencies first) --------------
	// The ownership tables the stages + the policing need: which pak owns
	// which VFS-addressable path (the resolver's one-owner invariant).
	std::map<std::string, std::string> vfsOwner; // path -> pak id
	for (const PlanPak& pak : plan.paks)
		for (const PlanAsset& asset : pak.assets)
			if (asset.kind == PlanAssetKind::Texture
					|| asset.kind == PlanAssetKind::BakeFile
					|| asset.kind == PlanAssetKind::Sound
					|| asset.kind == PlanAssetKind::Shader
					|| asset.kind == PlanAssetKind::AudioGraph
					|| asset.kind == PlanAssetKind::SpriteSet
					|| asset.kind == PlanAssetKind::StateMachine
					|| asset.kind == PlanAssetKind::NavMesh
					|| asset.kind == PlanAssetKind::Shape)
				vfsOwner[asset.path] = pak.id;

	// The staged set: every pak writes under publish/.staging/ with its
	// AUTHORED name (BakeStems keys bake discovery on the out-pak stem —
	// the staging name must stay clean); the destructor removes every
	// un-committed staging file, so ANY early return below leaves the
	// previous generation intact.
	// Each build receives an isolated staging directory. Two builders for
	// distinct manifests must never write the same temporary pak name.
	struct StagedSet {
		std::vector<fs::path> files;
		fs::path directory;
		bool committed = false;
		~StagedSet() {
			std::error_code cleanupEc;
			if (!committed)
				for (const fs::path& p : files)
					fs::remove(p, cleanupEc);
			if (!directory.empty())
				fs::remove_all(directory, cleanupEc);
		}
	} staged;
	const fs::path stagingRoot = publish / ".staging";
	fs::create_directories(stagingRoot, ec);
	if (ec) { err = "cannot create staging root: " + ec.message(); return false; }
	fs::path stagingDir;
	std::random_device random;
	for (int attempt = 0; attempt < 32; ++attempt) {
		const uint64_t nonce = (uint64_t(random()) << 32) | uint64_t(random());
		stagingDir = stagingRoot / ("build-" + std::to_string(nonce));
		ec.clear();
		if (fs::create_directory(stagingDir, ec)) break;
		if (ec && ec != std::errc::file_exists) {
			err = "cannot allocate staging directory: " + ec.message(); return false;
		}
		stagingDir.clear();
	}
	if (stagingDir.empty()) { err = "cannot allocate unique staging directory"; return false; }
	staged.directory = stagingDir;

	// The per-pak reports land in a LOCAL first: they describe PUBLISHED
	// outputs, and a mid-set failure rolls the whole set back — the
	// report must never claim a publication that was undone.
	std::vector<BuildPlanReport::Pak> reps;

	for (const PlanPak& pak : plan.paks) {
		PakStageArgs stage;
		stage.contentDir = project.root;
		// STAGED PUBLICATION (the content-addressed design): every pak
		// writes under .staging/ with its authored name; only after the
		// WHOLE set verified + policed does the publish pass land the
		// files under CONTENT-ADDRESSED, IMMUTABLE names (<stem>-<digest>
		// .pak — a rename never overwrites an existing pak), and the
		// manifest — the SINGLE-FILE commit point — atomically replaces
		// the .tapp last. A failure anywhere leaves the previous
		// manifest + every pak it references fully intact (a half-published
		// set only adds unreferenced immutable files, which the next
		// successful build sweeps).
		const fs::path stagedPath =
			(stagingDir / fs::path(pak.output)).lexically_normal();
		stage.outPak = stagedPath.generic_string();
		staged.files.push_back(stagedPath);
		stage.verify = true;
		// Pack the derived companion alongside its owning geometry.
		// Generated paths never enter authored include ownership/catalogs.
		if (plan.bakeSdf) {
			std::set<std::string> sdfFamilies;
			for (const PlanAsset& asset : pak.assets)
				if (asset.kind == PlanAssetKind::Geometry)
					sdfFamilies.insert(asset.path);
			for (const auto& artifact : pak.sceneArtifacts)
				sdfFamilies.insert(artifact);
			for (const auto& family : sdfFamilies)
				stage.contentSet.push_back(family + ".sdf");
		}
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
			// Authored shape assets are VFS-addressable and may be
			// owned by a scene pak (not just asset-only packages).
			for (const PlanAsset& asset : pak.assets)
				if (asset.kind == PlanAssetKind::Shape)
					stage.contentSet.push_back(asset.path);
			stage.levelName = options.levelName;
			stage.registryDir = options.registryDir;
			// The sound-id table (the pre-stage catalog ensure above):
			// the cooker resolves only the paths its scenes reference;
			// an unresolved referenced path with a NON-empty table
			// fails the stage (never silently omit a sound).
			stage.soundIds = soundIds;
			// The cross-pak excludes: every VFS-addressable asset owned
			// by a DECLARED dependency pak is not embedded here — the
			// materials keep their refs, the runtime resolves them
			// through the dependency's mounted backend (pak.h's
			// interop contract: PakBackend serves Texture/Sound blobs
			// by VFS path).
			std::set<std::string> depIds(pak.depends.begin(),
				pak.depends.end());
			for (const PlanPak& other : plan.paks) {
				if (!depIds.count(other.id))
					continue;
				for (const PlanAsset& asset : other.assets)
					if (asset.kind == PlanAssetKind::Texture
							|| asset.kind == PlanAssetKind::BakeFile
							|| asset.kind == PlanAssetKind::Sound
							|| asset.kind == PlanAssetKind::Shader
							|| asset.kind == PlanAssetKind::AudioGraph
					|| asset.kind == PlanAssetKind::SpriteSet
					|| asset.kind == PlanAssetKind::StateMachine
					|| asset.kind == PlanAssetKind::NavMesh
					|| asset.kind == PlanAssetKind::Shape)
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
		// Re-open the written pak and verify its VFS-addressable TOC
		// (Texture + Sound kinds) never carries a path another pak owns
		// — with the excludes above a dependency-owned path cannot
		// embed, so a hit here is an undeclared cross-pak reference (the
		// author referenced shared content without the depends edge) or
		// a packer regression. Either way the build fails BEFORE
		// publication, never ships a duplicate.
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
			const smvk::PakAssetKind policeKinds[8] = {
				smvk::PakAssetKind::Texture, smvk::PakAssetKind::Sound,
			smvk::PakAssetKind::Shader,
			smvk::PakAssetKind::AudioGraph,
				smvk::PakAssetKind::SpriteSet,
				smvk::PakAssetKind::StateMachine,
				smvk::PakAssetKind::NavMesh,
				smvk::PakAssetKind::Shape };
			for (const smvk::PakAssetKind kind : policeKinds)
			for (const smvk::PakTocEntry* e : reader->BlobsOfKind(kind)) {
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
			// ---- THE PER-PAK IDENTITY TABLE (the "#ids" registry
			// blob) — the consolidated content architecture's
			// discovery lane: the pak carries its OWN identity rows
			// (the .tapp [ids] successor), appended POST-COOK through
			// the append-grow lane. The ids/kinds/paths come from the
			// catalog THIS build generated (one owner per asset — the
			// same policing above), the scene rows' cooked names from
			// the just-written pak itself (ListPakScenes — the join
			// the manifest records; a multi-scene pak leaves the name
			// empty and rides the single-scene inference). STABLE
			// SLOTS are preserved across appends (the compiled plans'
			// addressing keeps working — project_plans.h re-folds from
			// the FINAL paks below, so the layout contract stays
			// consistent), and the publish pass hashes the APPENDED
			// bytes: the content-addressed name covers the identity
			// table too.
			{
				std::vector<::tour::asset::PakAssetIdRow> rows;
				const auto addRows =
					[&](const std::vector<CatalogEntry>& entries)
						-> bool {
					for (const CatalogEntry& e : entries) {
						if (e.pak != pak.id)
							continue;
						::tour::asset::PakAssetIdRow row;
						row.id = e.id;
						row.path = e.path;
						if (!smvk::CatalogKindFromString(e.kind,
								row.kind)) {
							err = "[Pak " + pak.id + "] the catalog"
								" row carries kind '" + e.kind
								+ "' — not a catalog kind";
							return false;
						}
						if (row.kind == smvk::kCatalogKindScene) {
							std::string slerr;
							const std::vector<smvk::PakSceneInfo>
								scenes = smvk::ListPakScenes(*reader,
									&slerr);
							row.sceneName = scenes.size() == 1
								? scenes[0].name : std::string{};
						}
						rows.push_back(std::move(row));
					}
					return true;
				};
				if (!addRows(catalog.scenes)
						|| !addRows(catalog.textures)
						|| !addRows(catalog.meshes)
						|| !addRows(catalog.sounds)
						|| !addRows(catalog.shaders)
						|| !addRows(catalog.gi)
						|| !addRows(catalog.audiographs)
				|| !addRows(catalog.spritesets)
				|| !addRows(catalog.statemachines)
				|| !addRows(catalog.navmeshes))
					return false;
				const std::vector<uint8_t> blob =
					::tour::asset::WriteAssetIdsBlob(rows);
				std::vector<std::pair<std::string,
					std::span<const uint8_t>>> append = {
					{ ::tour::asset::kPakAssetIdsPath,
						std::span<const uint8_t>(blob) },
				};
				std::vector<smvk::PakAppendInfo> appended;
				std::string aerr;
				if (!smvk::AppendPakBlobs(stage.outPak,
						smvk::PakAssetKind::Identity, append,
						appended, aerr)) {
					err = "[Pak " + pak.id + "] the identity-table"
						" append failed: " + aerr;
					return false;
				}
			}
		}
		BuildPlanReport::Pak rep;
		rep.id = pak.id;
		rep.output.clear(); // filled by the publish pass (content-addressed)
		rep.scene = pak.scene;
		for (const std::string& a : pak.sceneArtifacts)
			if (reimportedFamilies.count(a)) {
				rep.reimported = true;
				break;
			}
		reps.push_back(std::move(rep));
	}

	// Serialize the final immutable-name landings and manifest swap.
	// Staging/cooking stays concurrent; a second publisher fails safely
	// and may retry instead of racing the content-addressed rename.
	struct PublishLock {
		fs::path path;
		~PublishLock() {
			if (!path.empty()) {
				std::error_code ignore;
				fs::remove(path, ignore);
			}
		}
	} publishLock;
	{
		const fs::path lockPath = publish / ".publish.lock";
		std::error_code lockEc;
		if (!fs::create_directory(lockPath, lockEc)) {
			err = "a publisher is active (or a stale .publish.lock exists): "
				+ lockPath.generic_string();
			return false;
		}
		publishLock.path = lockPath;
	}

	// ---- the publish pass: every staged pak verified + policed, now
	// the content-addressed landings. Each final name is <stem>-<digest>
	// .pak (FNV-1a 64 over the pak bytes): IMMUTABLE by construction —
	// landing one never overwrites an existing file, so a partial
	// failure mid-pass leaves the previous manifest + every pak it
	// references fully intact (the orphans the failed run added are
	// unreferenced immutable files; the next successful build sweeps
	// them). An unchanged rebuild dedups: the final name already
	// exists, the staged copy is dropped, the published mtime never
	// moves.
	std::vector<fs::path> finalPaths; // plan order
	// The content-address digests (plan order — the post-publish
	// project.h re-stamp reuses them as the pak fingerprints' hashes:
	// the bytes were read exactly once, for the addressing).
	std::vector<std::string> finalDigests;
	{
		std::error_code readEc;
		for (size_t i = 0; i < plan.paks.size(); ++i) {
			const fs::path& stagedPath = staged.files[i];
			std::vector<uint8_t> image;
			{
				std::ifstream in(stagedPath, std::ios::binary);
				image.assign((std::istreambuf_iterator<char>(in)),
					std::istreambuf_iterator<char>());
			}
			if (image.empty() && !fs::is_regular_file(stagedPath, readEc)) {
				err = "the staged pak vanished before publication: "
					+ stagedPath.generic_string();
				return false;
			}
			const std::string digest = smvk::cache::HashBytes(
				image.data(), image.size());
			const std::string finalName =
				fs::path(plan.paks[i].output).stem().generic_string()
				+ "-" + digest + ".pak";
			const fs::path finalPath =
				(publish / finalName).lexically_normal();
			std::error_code renEc;
			if (fs::exists(finalPath, renEc)) {
				// A pre-existing content-addressed name is NOT evidence that
				// its bytes are intact: verify before trusting or deduping it.
				std::ifstream prior(finalPath, std::ios::binary);
				// Verify against the staged bytes in bounded blocks; loading
				// a second multi-GiB pak would double peak publish memory.
				bool identical = bool(prior);
				std::array<char, 65536> chunk{};
				for (size_t offset = 0; identical && offset < image.size();) {
					const size_t length = std::min(chunk.size(), image.size() - offset);
					prior.read(chunk.data(), std::streamsize(length));
					identical = size_t(prior.gcount()) == length &&
						std::memcmp(chunk.data(), image.data() + offset, length) == 0;
					offset += length;
				}
				// Also reject a same-prefix file with trailing injected bytes.
				if (identical && prior.peek() != std::char_traits<char>::eof())
					identical = false;
				if (!identical) {
					err = "published pak filename collision or corrupted bytes: "
						+ finalPath.generic_string();
					return false;
				}
				fs::remove(stagedPath, renEc);
			}
			else {
				fs::rename(stagedPath, finalPath, renEc);
				if (renEc) {
					err = "the publish rename failed: "
						+ stagedPath.generic_string() + " -> "
						+ finalPath.generic_string() + " ("
						+ renEc.message() + ")";
					return false;
				}
			}
			finalPaths.push_back(finalPath);
			finalDigests.push_back(digest);
			reps[i].output = finalPath.generic_string();
		}
	}
	// The set is live (immutable adds only): the reports describe
	// genuinely published paks from here on. The manifest write below
	// is the COMMIT; until it lands, the previous manifest + its paks
	// stand untouched.
	out.paks = std::move(reps);

	// ---- 4) publish the appmanifest (the single-file COMMIT) ----------
	// The manifest names the entry pak + the entry scene's cooked
	// identity + the ordered pak list (mount order = plan order, the
	// CONTENT-ADDRESSED file names). It is written to a STAGED path and
	// ATOMICALLY RENAMED over the final .tapp: the rename is the commit
	// point — every failure above it leaves the previous manifest + the
	// paks it references fully intact (content-addressed paks are
	// immutable adds; a half-published run only leaves unreferenced
	// orphans, swept below on the next success).
	{
		const PlanPak* entryPak = plan.FindPak(plan.entryPak);
		size_t entryIdx = SIZE_MAX;
		for (size_t i = 0; i < plan.paks.size(); ++i)
			if (plan.paks[i].id == plan.entryPak)
				entryIdx = i;
		if (!entryPak || entryIdx == SIZE_MAX
				|| !fs::is_regular_file(finalPaths[entryIdx], ec)) {
			err = "the entry pak '" + plan.entryPak
				+ "' did not build (cannot publish the manifest)";
			return false;
		}
		// The cooked entry-scene identity: list the entry pak's scenes
		// and record the tour scene's NAME (tour-built paks carry one
		// scene; the runtime resolves by name, never by index).
		std::string entrySceneName;
		{
			std::ifstream in(finalPaths[entryIdx], std::ios::binary);
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
		manifest.entryPak = finalPaths[entryIdx].filename()
			.generic_string();
		manifest.entryScene = entrySceneName;
		for (const fs::path& p : finalPaths)
			manifest.paks.push_back(p.filename().generic_string());
		// THE MANIFEST CARRIES NO CATALOG (appmanifest 2): the release
		// pointer is entry + [paks] ONLY — every listed pak carries
		// its OWN "#ids" identity table (appended post-cook in the pak
		// stage above), and the runtime joins them through discovery
		// (ProjectRuntime::Load — ContentRegistry::DiscoverPak per
		// listed pak). The generated project.h + the paks' tables are
		// the whole catalog contract; the manifest never mirrors
		// identity again (the [ids] duplication the consolidated
		// audit killed).
		std::string manifestName = options.manifestName;
		if (manifestName.empty()) {
			manifestName = fs::path(entryPak->output).stem()
				.generic_string();
		}
		const fs::path manifestPath =
			(publish / (manifestName + ".tapp")).lexically_normal();
		const fs::path stagedManifest =
			(stagingDir / (manifestName + ".tapp")).lexically_normal();
		staged.files.push_back(stagedManifest); // rolled back on failure
		std::string mErr;
		if (!smvk::SaveAppManifest(stagedManifest.generic_string(),
				manifest, mErr)) {
			err = "the appmanifest publish failed: " + mErr;
			return false;
		}
		// An immutable content-set receipt captures which source plan pak
		// owns each addressed package. The appmanifest remains the ONLY
		// release pointer; this receipt is a verifiable audit sidecar.
		// Land it BEFORE the manifest commit so a committed release never
		// refers to a generation with an absent receipt after a crash.
		{
			std::ifstream input(stagedManifest, std::ios::binary);
			if (!input) { err = "cannot read staged appmanifest for receipt"; return false; }
			const std::string manifestBytes((std::istreambuf_iterator<char>(input)),
				std::istreambuf_iterator<char>());
			if (!input.eof() && input.fail()) {
				err = "failed to read staged appmanifest for receipt"; return false;
			}
			const std::string manifestDigest = smvk::cache::HashBytes(
				manifestBytes.data(), manifestBytes.size());
			std::ostringstream receipt;
			receipt << "tourmaline-build-receipt 1\n"
				<< "manifest " << manifestPath.filename().generic_string()
				<< " fnv1a64:" << manifestDigest << "\n";
			for (size_t i = 0; i < plan.paks.size(); ++i)
				receipt << "pak " << plan.paks[i].id << ' '
					<< finalPaths[i].filename().generic_string() << " fnv1a64:"
					<< finalDigests[i] << "\n";
			// Capture logical source-to-package ownership as well as output
			// bytes. Stable content IDs remain in each pak's #ids table;
			// this ledger records the recipe edge that produced the pak.
			for (const PlanPak& pak : plan.paks) {
				receipt << "scene " << std::quoted(pak.id) << ' '
					<< std::quoted(pak.scene) << "\n";
				for (const std::string& artifact : pak.sceneArtifacts)
					receipt << "scene-artifact " << std::quoted(pak.id)
						<< ' ' << std::quoted(artifact) << "\n";
				for (const PlanAsset& asset : pak.assets)
					receipt << "input " << std::quoted(pak.id) << ' '
						<< std::quoted(asset.path) << ' '
						<< int(asset.kind) << "\n";
			}
			const std::string payload = receipt.str();
			const fs::path stagedReceipt = stagingDir /
				(manifestName + ".receipt");
			staged.files.push_back(stagedReceipt);
			{
				std::ofstream output(stagedReceipt, std::ios::binary | std::ios::trunc);
				output.write(payload.data(), std::streamsize(payload.size()));
				output.close();
				if (!output) { err = "cannot write staged build receipt"; return false; }
			}
			const fs::path finalReceipt = publish /
				(manifestName + "-" + manifestDigest + ".receipt");
			std::error_code receiptEc;
			if (fs::exists(finalReceipt, receiptEc)) {
				std::ifstream existing(finalReceipt, std::ios::binary);
				const std::string existingBytes((std::istreambuf_iterator<char>(existing)),
					std::istreambuf_iterator<char>());
				if (!existing || existingBytes != payload) {
					err = "existing build receipt is corrupt or collides: "
						+ finalReceipt.generic_string();
					return false;
				}
				fs::remove(stagedReceipt, receiptEc);
			} else {
				fs::rename(stagedReceipt, finalReceipt, receiptEc);
				if (receiptEc) {
					err = "cannot publish immutable build receipt: "
						+ receiptEc.message();
					return false;
				}
			}
		}
		// THE COMMIT: rename-over-existing is atomic on both supported
		// platforms (MSVC MoveFileEx REPLACE_EXISTING; POSIX rename).
		{
			std::error_code renEc;
			fs::rename(stagedManifest, manifestPath, renEc);
			if (renEc) {
				err = "the appmanifest commit rename failed: "
					+ stagedManifest.generic_string() + " -> "
					+ manifestPath.generic_string() + " ("
					+ renEc.message() + ")";
				return false;
			}
		}
		staged.committed = true;
		out.manifest = manifestPath.generic_string();
		// ---- the stale-generation sweep (only AFTER the commit) ------
		// Remove unreferenced <stem>.pak / <stem>-<digest>.pak files for
		// OUR stems: the previous generation's paks (content-addressed
		// or pre-content-addressing plain names) plus any orphans a
		// failed run left. Nothing referenced by the live manifest is
		// ever touched.
		{
			std::set<std::string> referenced(manifest.paks.begin(),
				manifest.paks.end());
			// Preserve content still referenced by OTHER published manifests
			// in this directory. An invalid manifest disables garbage
			// collection rather than risking deletion of a live pak.
			bool sweepSafe = true;
			std::error_code manifestIterEc;
			for (const fs::directory_entry& entry :
					fs::directory_iterator(publish, manifestIterEc)) {
				if (manifestIterEc) { sweepSafe = false; break; }
				if (entry.path().extension() != ".tapp") continue;
				smvk::AppManifest other;
				std::string loadError;
				if (!smvk::LoadAppManifest(entry.path().generic_string(),
						other, loadError)) { sweepSafe = false; break; }
				referenced.insert(other.paks.begin(), other.paks.end());
			}
			if (manifestIterEc) sweepSafe = false;
			// Another build may be cooking against this publish directory.
			// Its forthcoming manifest is not visible yet: do not collect
			// any immutable packages while its staging directory exists.
			std::error_code activeEc;
			for (const fs::directory_entry& pending :
					fs::directory_iterator(stagingRoot, activeEc)) {
				if (activeEc) { sweepSafe = false; break; }
				if (pending.path() != stagingDir) { sweepSafe = false; break; }
			}
			if (activeEc) sweepSafe = false;
			const auto isHex16 = [](const std::string& s) {
				if (s.size() != 16)
					return false;
				for (char c : s)
					if (!(c >= '0' && c <= '9')
							&& !(c >= 'a' && c <= 'f'))
						return false;
				return true;
			};
			std::vector<fs::path> stale;
			for (const fs::directory_entry& e :
					fs::directory_iterator(publish, ec)) {
				if (!e.is_regular_file(ec))
					continue;
				const std::string name =
					e.path().filename().generic_string();
				for (const PlanPak& pak : plan.paks) {
					const std::string stem =
						fs::path(pak.output).stem().generic_string();
					const bool plain = name == stem + ".pak";
					const bool addressed = name.size() > stem.size() + 17
						&& name.compare(0, stem.size(), stem) == 0
						&& name[stem.size()] == '-'
						&& name.compare(name.size() - 4, 4, ".pak") == 0
						&& isHex16(name.substr(stem.size() + 1,
							name.size() - stem.size() - 5));
					if ((plain || addressed)
							&& sweepSafe && !referenced.count(name))
						stale.push_back(e.path());
				}
			}
			for (const fs::path& p : stale) {
				std::error_code rmEc;
				fs::remove(p, rmEc); // best-effort: swept again next run
			}
			if (!stale.empty())
				std::printf("[build] swept %zu stale pak(s) from the"
					" previous generation\n", stale.size());
		}
		std::printf("[build] published appmanifest: %s (entry %s:%s,"
			" %zu pak(s))\n", out.manifest.c_str(),
			manifest.entryPak.c_str(), manifest.entryScene.c_str(),
			manifest.paks.size());
		// ---- 5) re-stamp the generated project.h -------------------
		// The recipe build is the ONE writer that knows the paks'
		// content (verify + police just passed), so the EOF metadata's
		// pak records go state=verified HERE — a catalog-only refresh
		// can never upgrade a state, only preserve or demote it. The
		// stamp follows the SAME policy the ids command runs
		// (ComposeProjectHeader): a fully unchanged build (dedup'd
		// paks, untouched sources) re-emits byte-identical text and
		// never touches the file. Best-effort BY DESIGN: the paks +
		// the manifest are already committed — a header failure warns
		// instead of failing a build whose content stands.
		if (!options.projectHeaderPath.empty()) {
			HeaderMetadata prev;
			std::string hErr;
			std::vector<AssetFingerprint> assetPrints;
			std::vector<PakFingerprint> pakPrints;
			if (ReadProjectHeaderMetadata(options.projectHeaderPath,
					prev, hErr)
				&& CollectAssetFingerprints(project, catalog,
					assetPrints, hErr)) {
				for (size_t i = 0; i < plan.paks.size()
						&& i < finalPaths.size(); ++i) {
					PakFingerprint f;
					f.file = finalPaths[i].filename()
						.generic_string();
					// The publish pass content-addressed on this
					// digest — reuse it as the hash (the pak bytes
					// are never read a second time).
					if (i < finalDigests.size())
						f.hash = "fnv1a64:" + finalDigests[i];
					std::string sErr;
					if (!StatFile(finalPaths[i].generic_string(),
							f.mtime, f.size, sErr)) {
						hErr = sErr;
						break;
					}
					f.state = f.mtime >= 0 ? "verified" : "missing";
					pakPrints.push_back(std::move(f));
				}
				if (hErr.empty()) {
					std::string text;
					GenerationStamp stamp;
					bool changed = false;
					ComposeProjectHeader(catalog, assetPrints,
						pakPrints, options.projectHeaderPath, false,
						text, stamp, changed);
					bool wrote = false;
					if (WriteProjectHeaderIfChanged(
							options.projectHeaderPath, text, wrote,
							hErr))
						std::printf("[build] project.h generation"
							" %llu (%s)\n",
							static_cast<unsigned long long>(
								stamp.generation),
							wrote ? "re-stamped" : "unchanged");
					// C companion: exactly the same catalog and generation.
					if (hErr.empty()) {
						const fs::path cHeaderPath =
							fs::path(options.projectHeaderPath)
							.parent_path() / "project_c.h";
						bool cWrote = false;
						if (WriteProjectHeaderIfChanged(
							cHeaderPath.generic_string(),
							EmitProjectCHeader(catalog, stamp),
							cWrote, hErr) && cWrote)
							std::printf("[build] project_c.h regenerated\n");
					}
					// THE COMPILED STATIC PLANS: the private header
					// re-emits from the JUST-BUILT paks (the strict
					// mode's cook-precedes-plan-generation order).
					if (hErr.empty()) {
						const fs::path plansPath =
							fs::path(options.projectHeaderPath)
							.parent_path() / "project_plans.h";
						std::string plansText;
						if (EmitPlansHeader(catalog,
								publish.generic_string(),
								plansText, hErr)) {
							bool plansWrote = false;
							if (WriteProjectHeaderIfChanged(
									plansPath.generic_string(),
									plansText, plansWrote, hErr)
								&& plansWrote)
								std::printf("[build] project_plans.h"
									" regenerated\n");
						}
					}
				}
			}
			if (!hErr.empty())
				std::printf("[build] WARNING: the project header"
					" re-stamp failed (%s) — the paks + the manifest"
					" stand; the next ids refresh re-stamps\n",
					hErr.c_str());
		}
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
			+ project.root + " --source <model .gltf/.glb/.obj>)";
		return false;
	}
	out.artifact = refSource;

	// ---- 3) import freshness (reimport stale; unchanged = no-op) --------
	// The source (a mesh source format — .gltf/.glb/.obj) beside the
	// imported artifact (same stem, same directory — the project's
	// copy-in convention).
	for (const char* ext : { ".gltf", ".glb", ".obj" }) {
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
		const int rc = RunStage(StageExe("Cook").c_str(), a, err);
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
		// 2 of the octree retirement): NO --out, no
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
		const int rc = RunStage(StageExe("GiBaker").c_str(), a, err);
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
		const int rc = RunStage(StageExe("Pak").c_str(), a, err);
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
