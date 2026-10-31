// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Project.exe: the project-pipeline CLI (a thin wrapper over the shared
// project_pipeline library — the editor calls the same implementation;
// there is no separate CLI logic of any kind).
//
//   Project.exe init   --project <dir> [--name <n>] [--from-gltf <glTF>]
//   Project.exe import --project <dir> --source <glTF|GLB>
//                      [--no-lods] [--no-meshlets] [--lod-ratios 0.75,0.5,0.3]
//                      [--lod-error 0.01] [--force]
//   Project.exe validate --project <dir> --recipe <file.tourrecipe>
//                      (parse + resolve; prints the plan, no stages run)
//   Project.exe ids    --project <dir> --recipe <file.tourrecipe>
//                      [--header <out>] [--deps <depfile>]
//                      [--pak-dir <dir>] [--stamp]
//                      (the content-as-code generation: the ids +
//                      the GENERATION-STAMPED project.h)
//   Project.exe build  --project <dir> --recipe <file.tourrecipe>
//                      [--publish <dir>] [--manifest-name <name>]
//                      [--header <out>]
//                      (the recipe build: resolve -> import -> cook ->
//                       bake -> pak per plan -> the appmanifest LAST;
//                       with --header project.h re-stamps verified)
//       or the legacy  --project <dir> --output <pak> [--scene <tour>]
//   Project.exe status --project <dir>        (the staleness report)
//
// Exit codes: 0 success, 1 pipeline failure, 2 usage.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <project_pipeline/build_pipeline.h>
#include <project_pipeline/build_plan.h>
#include <project_pipeline/import_pipeline.h>
#include <project_pipeline/project.h>
#include <project_pipeline/project_catalog.h>
#include <project_pipeline/project_recipe.h>
#include <project_pipeline/recipe_resolver.h>

#include <artifact_cache/assetdb.h>
#include <artifact_cache/artifact_cache.h>

#include <Engine/Asset/tour.h>

namespace {

int Usage(const char* prog) {
	std::cout << "Usage: " << (prog ? prog : "Project.exe") << " <command> [flags]\n"
		<< "Commands:\n"
		<< "  init   --project <dir> [--name <name>] [--from-gltf <glTF|GLB>]\n"
		<< "         create (or adopt) a project; --from-gltf imports the\n"
		<< "         source into it after creation\n"
		<< "  import --project <dir> --source <glTF|GLB>\n"
		<< "         [--no-lods] [--no-meshlets] [--lod-ratios 0.75,0.5,0.3,0.15]\n"
		<< "         [--lod-error 0.01] [--shader-lods] [--force]\n"
		<< "         copy the source into the project + derive the native\n"
		<< "         artifact family (idempotent: unchanged = no-op)\n"
		<< "  validate --project <dir> --recipe <file.tourrecipe>\n"
		<< "         parse + resolve the recipe and print the plan (paks,\n"
		<< "         ownership, mount order) — NO stage runs\n"
		<< "  ids    --project <dir> --recipe <file.tourrecipe>\n"
		<< "         [--header <out>] [--deps <depfile>] [--pak-dir <dir>]\n"
		<< "         [--stamp]\n"
		<< "         the content-as-code generation: ensure the content\n"
		<< "         ids in the ASSET DATABASE (assets.db — the one\n"
		<< "         identity store) + regenerate the GENERATION-STAMPED\n"
		<< "         project.h (write-only-on-change; --stamp forces a\n"
		<< "         new generation every run; --pak-dir fingerprints\n"
		<< "         that publish dir's paks, default <root>/build)\n"
		<< "  build  --project <dir> --recipe <file.tourrecipe>\n"
		<< "         [--publish <dir>] [--manifest-name <name>]\n"
		<< "         [--header <out>]\n"
		<< "         the RECIPE build: resolve -> import freshness -> Cook\n"
		<< "         -> GiBaker -> Pak per plan (verified + policed) -> the\n"
		<< "         appmanifest (.tapp) publishes LAST (with --header:\n"
		<< "         project.h re-stamps, its EOF pak records go\n"
		<< "         state=verified)\n"
		<< "      or --project <dir> --output <pak> [--scene <tour>] (the\n"
		<< "         legacy single-scene build — unchanged)\n"
		<< "  status --project <dir>\n"
		<< "         report every imported source: current, or why not\n";
	return 2;
}

bool ParseRatios(const std::string& csv, std::vector<float>& out) {
	out.clear();
	size_t pos = 0;
	while (pos <= csv.size()) {
		size_t end = csv.find(',', pos);
		if (end == std::string::npos)
			end = csv.size();
		const std::string tok = csv.substr(pos, end - pos);
		char* endp = nullptr;
		const float v = std::strtof(tok.c_str(), &endp);
		if (tok.empty() || (endp && *endp != '\0') || !(v > 0.0f && v < 1.0f))
			return false;
		out.push_back(v);
		pos = end + 1;
	}
	return !out.empty();
}

} // namespace

int main(int argc, const char* argv[]) {
	if (argc < 2)
		return Usage(argv[0]);
	const std::string cmd = argv[1];

	// Shared flag scan.
	std::string projectDir, source, name, ratiosCsv, scene, output,
		fromScene, recipeFile, publishDir, manifestName, headerOut,
		depsOut, pakDir;
	bool fromGltf = false, force = false, stampRefresh = false;
	bool noLods = false, noMeshlets = false, shaderLods = false;
	bool ratiosSet = false, errorSet = false;
	float lodError = 0.01f;
	for (int i = 2; i < argc; ++i) {
		const char* a = argv[i];
		if ((std::strcmp(a, "--project") == 0) && i + 1 < argc)
			projectDir = argv[++i];
		else if (std::strncmp(a, "--project=", 10) == 0)
			projectDir = a + 10;
		else if ((std::strcmp(a, "--recipe") == 0) && i + 1 < argc)
			recipeFile = argv[++i];
		else if (std::strncmp(a, "--recipe=", 9) == 0)
			recipeFile = a + 9;
		else if ((std::strcmp(a, "--publish") == 0) && i + 1 < argc)
			publishDir = argv[++i];
		else if (std::strncmp(a, "--publish=", 10) == 0)
			publishDir = a + 10;
		else if ((std::strcmp(a, "--manifest-name") == 0) && i + 1 < argc)
			manifestName = argv[++i];
		else if (std::strncmp(a, "--manifest-name=", 16) == 0)
			manifestName = a + 16;
		else if ((std::strcmp(a, "--header") == 0) && i + 1 < argc)
			headerOut = argv[++i];
		else if (std::strncmp(a, "--header=", 9) == 0)
			headerOut = a + 9;
		else if ((std::strcmp(a, "--pak-dir") == 0) && i + 1 < argc)
			pakDir = argv[++i];
		else if (std::strncmp(a, "--pak-dir=", 10) == 0)
			pakDir = a + 10;
		else if (std::strcmp(a, "--stamp") == 0)
			stampRefresh = true;
		else if ((std::strcmp(a, "--deps") == 0) && i + 1 < argc)
			depsOut = argv[++i];
		else if (std::strncmp(a, "--deps=", 7) == 0)
			depsOut = a + 7;
		else if ((std::strcmp(a, "--source") == 0
				|| std::strcmp(a, "--from-gltf") == 0) && i + 1 < argc) {
			source = argv[++i];
			fromGltf = std::strcmp(a, "--from-gltf") == 0;
		}
		else if (std::strncmp(a, "--from-gltf=", 12) == 0) {
			source = a + 12;
			fromGltf = true;
		}
		else if ((std::strcmp(a, "--name") == 0) && i + 1 < argc)
			name = argv[++i];
		else if (std::strncmp(a, "--name=", 7) == 0)
			name = a + 7;
		else if ((std::strcmp(a, "--scene") == 0) && i + 1 < argc)
			scene = argv[++i];
		else if (std::strncmp(a, "--scene=", 8) == 0)
			scene = a + 8;
		else if ((std::strcmp(a, "--from-scene") == 0) && i + 1 < argc)
			fromScene = argv[++i];
		else if (std::strncmp(a, "--from-scene=", 13) == 0)
			fromScene = a + 13;
		else if ((std::strcmp(a, "--output") == 0
				|| std::strcmp(a, "--out-pak") == 0) && i + 1 < argc)
			output = argv[++i];
		else if (std::strncmp(a, "--output=", 9) == 0)
			output = a + 9;
		else if (std::strncmp(a, "--out-pak=", 10) == 0)
			output = a + 10;
		else if ((std::strcmp(a, "--lod-ratios") == 0) && i + 1 < argc) {
			ratiosCsv = argv[++i];
			ratiosSet = true;
		}
		else if (std::strncmp(a, "--lod-ratios=", 13) == 0) {
			ratiosCsv = a + 13;
			ratiosSet = true;
		}
		else if ((std::strcmp(a, "--lod-error") == 0) && i + 1 < argc) {
			lodError = std::strtof(argv[++i], nullptr);
			errorSet = true;
		}
		else if (std::strncmp(a, "--lod-error=", 12) == 0) {
			lodError = std::strtof(a + 12, nullptr);
			errorSet = true;
		}
		else if (std::strcmp(a, "--no-lods") == 0)
			noLods = true;
		else if (std::strcmp(a, "--no-meshlets") == 0)
			noMeshlets = true;
		else if (std::strcmp(a, "--shader-lods") == 0)
			shaderLods = true;
		else if (std::strcmp(a, "--force") == 0)
			force = true;
		else {
			std::cerr << "Unknown flag: " << a << "\n";
			return Usage(argv[0]);
		}
	}
	if (projectDir.empty()) {
		std::cerr << "--project is required.\n";
		return Usage(argv[0]);
	}

	if (cmd == "init") {
		bool created = false;
		std::string err;
		smvk::project::Project p =
			smvk::project::Project::Init(projectDir, name, created, err);
		if (!p.Valid()) {
			std::cerr << "init failed: " << err << "\n";
			return 1;
		}
		std::cout << "Project " << (created ? "created" : "adopted")
			<< ": " << p.root << " (name '" << p.name << "')\n";
		// Persist the default build settings on first creation (the
		// project owns the stage flags — scripts never duplicate them).
		if (created) {
			std::error_code ec;
			std::filesystem::create_directories(
				p.DerivedDataDir(), ec);
			const std::filesystem::path bs =
				std::filesystem::path(p.DerivedDataDir())
				/ "build-settings.txt";
			if (!std::filesystem::is_regular_file(bs, ec)) {
				std::ofstream out(bs, std::ios::binary
					| std::ios::trunc);
				if (out)
					out << "# Tourmaline project build settings (the"
						" stage flags the build orchestrator reads;\n"
						"# edit here, never in a batch script)\n"
						"cubemap=on\n"
						"lightmap=off\n"
						"giBackend=embree\n"
						"giMaxProbes=131072\n";
			}
		}
		if (!fromGltf && fromScene.empty())
			return 0;
		// --from-gltf / --from-scene = the normal import/seed right
		// after creation (the same paths as separate commands).
	}

	if (cmd == "import" || (cmd == "init"
			&& (fromGltf || !fromScene.empty()))) {
		std::string err;
		smvk::project::Project p =
			smvk::project::Project::Open(projectDir, err);
		if (!p.Valid()) {
			std::cerr << err << "\n";
			return 1;
		}
		// The import half (--from-gltf or a bare 'import').
		if (!source.empty() || fromGltf) {
			if (source.empty()) {
				std::cerr << (cmd == "import"
					? "--source is required.\n"
					: "--from-gltf is required.\n");
				return Usage(argv[0]);
			}
			smvk::project::ImportRequest req;
			req.project = p;
			req.source = std::filesystem::path(source);
			req.settings.lods = !noLods;
			req.settings.meshlets = !noMeshlets;
			req.settings.shaderLods = shaderLods;
			if (ratiosSet
					&& !ParseRatios(ratiosCsv, req.settings.lodRatios)) {
				std::cerr << "bad --lod-ratios '" << ratiosCsv
					<< "' (want comma-separated fractions in (0,1), e.g."
					" 0.75,0.5,0.3)\n";
				return 2;
			}
			if (errorSet)
				req.settings.lodTargetError = lodError;
			req.force = force;
			smvk::project::ImportResult result;
			if (!smvk::project::ImportProjectAsset(req, result, err)) {
				std::cerr << "import failed: " << err << "\n";
				return 1;
			}
			if (!result.derived)
				std::cout << "Import current (no work): "
					<< result.geometry.generic_string() << "\n";
			else
				std::cout << "Imported: "
					<< result.projectSource.generic_string()
					<< "\n  geometry=" << result.geometry.generic_string()
					<< "\n  guid=" << result.guid
					<< "\n  blobs=" << result.geometryBlobs
					<< " lods=" << result.lodLevels
					<< " meshletSets=" << result.meshletSets << "\n";
		}
		// The scene-seed half (--from-scene; runs after the import so the
		// reference re-pointing knows the imported family).
		if (!fromScene.empty()) {
			const std::string stem = std::filesystem::path(
				source.empty() ? fromScene : source).stem().string();
			const std::string seeded =
				smvk::project::SeedProjectScene(p, fromScene, stem, err);
			if (seeded.empty()) {
				std::cerr << "scene seed failed: " << err << "\n";
				return 1;
			}
			std::cout << "Scene: " << seeded
				<< " (authored scenes are never regenerated)\n";
		}
		return 0;
	}

	// ---- validate + the recipe build ----------------------------------
	// Shared: open the project, locate the recipe, parse + resolve.
	const auto resolveRecipe =
		[&](smvk::project::Project& p, smvk::project::ProjectRecipe& r,
			std::string& err) -> std::string {
		namespace fs = std::filesystem;
		std::error_code ec;
		fs::path path(recipeFile);
		if (!fs::is_regular_file(path, ec)) {
			// project-relative fallback (the common authoring form)
			path = fs::path(p.root) / path;
			if (!fs::is_regular_file(path, ec)) {
				err = "recipe not found: " + recipeFile;
				return {};
			}
		}
		smvk::project::RecipeDiagnostic diag;
		if (!smvk::project::ParseProjectRecipe(path.generic_string(), r,
				diag)) {
			err = diag.Format(path.generic_string());
			return {};
		}
		return path.lexically_normal().generic_string();
	};

	if (cmd == "validate") {
		if (recipeFile.empty()) {
			std::cerr << "--recipe <file.tourrecipe> is required.\n";
			return Usage(argv[0]);
		}
		std::string err;
		smvk::project::Project p =
			smvk::project::Project::Open(projectDir, err);
		if (!p.Valid()) {
			std::cerr << err << "\n";
			return 1;
		}
		smvk::project::ProjectRecipe recipe;
		const std::string path = resolveRecipe(p, recipe, err);
		if (path.empty()) {
			std::cerr << "recipe rejected: " << err << "\n";
			return 1;
		}
		smvk::project::BuildPlan plan;
		if (!smvk::project::ResolveRecipe(p, recipe, plan, err)) {
			std::cerr << "resolve failed: " << err << "\n";
			return 1;
		}
		std::cout << "Recipe OK: " << path << "\n"
			<< "  project: "
			<< (recipe.projectName.empty() ? p.name
				: recipe.projectName) << "\n"
			<< "  entry: " << plan.entryScene << " (pak '"
			<< plan.entryPak << "')\n"
			<< "  bake: gi=" << (plan.bakeGi ? "on" : "off")
			<< " cubemaps=" << (plan.bakeCubemaps ? "on" : "off")
			<< "\n"
			<< "  paks (mount order):\n";
		for (const smvk::project::PlanPak& pak : plan.paks) {
			std::cout << "    [" << pak.id << "] -> " << pak.output
				<< (pak.scene.empty() ? " (asset-only)" : " scene")
				<< ", " << pak.assets.size() << " owned asset(s)";
			if (!pak.depends.empty()) {
				std::cout << ", depends";
				for (const std::string& d : pak.depends)
					std::cout << " " << d;
			}
			std::cout << "\n";
		}
		return 0;
	}

	if (cmd == "ids") {
		// The content-as-code generation (the CMake layer's form):
		// resolve the plan, ensure the content ids in the ASSET
		// DATABASE (assets.db — the one identity store), regenerate
		// the GENERATION-STAMPED project.h (the stamp bumps only on a
		// generation event, or always with --stamp). NO stage runs,
		// no pak is written — the paks are only fingerprinted for the
		// EOF metadata (state claims, never cooks).
		if (recipeFile.empty()) {
			std::cerr << "--recipe <file.tourrecipe> is required.\n";
			return Usage(argv[0]);
		}
		std::string err;
		smvk::project::Project p =
			smvk::project::Project::Open(projectDir, err);
		if (!p.Valid()) {
			std::cerr << err << "\n";
			return 1;
		}
		smvk::project::ProjectRecipe recipe;
		const std::string path = resolveRecipe(p, recipe, err);
		if (path.empty()) {
			std::cerr << "recipe rejected: " << err << "\n";
			return 1;
		}
		// The RAW-SOURCE pre-pass: a project of authored sources (no
		// import yet) must resolve + catalog on the FIRST run — import
		// every scene pak's ROOT family before the plan needs the
		// artifacts (stale families re-derive; unchanged = no-op).
		for (const smvk::project::RecipePak& pak : recipe.paks) {
			if (pak.scene.empty())
				continue;
			smvk::tour::Doc doc;
			std::string tErr;
			if (!smvk::tour::LoadTour(
					(std::filesystem::path(p.root) / pak.scene)
						.generic_string(),
					doc, tErr))
				continue; // the resolve below reports it properly
			if (doc.references.empty())
				continue;
			std::string srcRel = doc.references[0].source;
			std::string ext =
				std::filesystem::path(srcRel).extension().generic_string();
			if (ext == ".geometry") {
				// The canonical form: the imported family beside the
				// artifact (SeedProjectScene's re-pointed reference).
				std::filesystem::path sibling =
					std::filesystem::path(srcRel);
				for (const char* e : { ".gltf", ".glb" }) {
					sibling.replace_extension(e);
					if (std::filesystem::is_regular_file(
							std::filesystem::path(p.root) / sibling))
						break;
					sibling = std::filesystem::path(srcRel);
				}
				if (sibling.extension().generic_string() == ".geometry")
					continue; // no source family found — resolve reports
				srcRel = sibling.generic_string();
				ext = std::filesystem::path(srcRel).extension()
					.generic_string();
			}
			if (ext != ".gltf" && ext != ".glb")
				continue;
			smvk::project::ImportRequest req;
			req.project = p;
			req.source = std::filesystem::path(p.root) / srcRel;
			smvk::project::ImportResult res;
			std::string iErr;
			if (!smvk::project::ImportProjectAsset(req, res, iErr)) {
				std::cerr << "pre-pass import failed: " << iErr << "\n";
				return 1;
			}
		}
		smvk::project::BuildPlan plan;
		if (!smvk::project::ResolveRecipe(p, recipe, plan, err)) {
			std::cerr << "resolve failed: " << err << "\n";
			return 1;
		}
		// The plan's own freshness (nested references + owned geometry
		// the pre-pass above did not see).
		if (!smvk::project::RefreshPlanImports(p, plan, nullptr, err)) {
			std::cerr << err << "\n";
			return 1;
		}
		// The registry: assets.db under the project's .tourmaline (the
		// ONE identity store — no separate id file by design).
		smvk::cache::AssetDatabase db;
		{
			const std::string registryRoot =
				smvk::cache::EnsureRegistryRoot(p.DerivedDataDir());
			if (registryRoot.empty() || !db.Open(registryRoot, p.root,
					err)) {
				std::cerr << "asset db open failed: " << err << "\n";
				return 1;
			}
		}
		smvk::project::ProjectCatalog catalog;
		std::vector<std::string> deps;
		if (!smvk::project::GenerateProjectCatalog(p, plan, db, catalog,
				deps, err)) {
			std::cerr << "catalog generation failed: " << err << "\n";
			return 1;
		}
		const std::string header = headerOut.empty()
			? (std::filesystem::path(p.root) / "project.h")
				.generic_string()
			: headerOut;
		// The pak dir the EOF metadata fingerprints (the publish dir —
		// the same default the recipe build publishes to; the CMake
		// layer passes the consuming exe's dir).
		const std::string effectivePakDir = pakDir.empty()
			? (std::filesystem::path(p.root) / "build")
				.generic_string()
			: pakDir;
		// The previous generation's EOF metadata (the stamp ledger +
		// the cook-state witness — the no-op probe compares against
		// it. It is NOT a freshness cache: every source below is
		// hashed from bytes on every run).
		smvk::project::HeaderMetadata prev;
		{
			std::string mErr;
			if (!smvk::project::ReadProjectHeaderMetadata(header,
					prev, mErr)) {
				std::cerr << mErr << "\n";
				return 1;
			}
		}
		std::vector<smvk::project::AssetFingerprint> assetPrints;
		if (!smvk::project::CollectAssetFingerprints(p, catalog,
				assetPrints, err)) {
			std::cerr << err << "\n";
			return 1;
		}
		std::vector<smvk::project::PakFingerprint> pakPrints;
		if (!smvk::project::DiscoverPakFingerprints(effectivePakDir,
				catalog, assetPrints, prev, pakPrints, err)) {
			std::cerr << err << "\n";
			return 1;
		}
		std::string text;
		smvk::project::GenerationStamp stamp;
		bool changed = false;
		smvk::project::ComposeProjectHeader(catalog, assetPrints,
			pakPrints, header, stampRefresh, text, stamp, changed);
		bool wrote = false;
		if (!smvk::project::WriteProjectHeaderIfChanged(header, text,
				wrote, err)) {
			std::cerr << err << "\n";
			return 1;
		}
		// The depfile (the CMake/Ninja rebuild trigger): the header
		// depends on the recipe, every content file the resolution
		// touched, and the registry itself (an id allocation anywhere
		// regenerates).
		if (!depsOut.empty()) {
			std::ofstream d(depsOut, std::ios::binary | std::ios::trunc);
			// Ninja depfile escapes: spaces only. Drive-letter colons
			// stay bare — ninja's separator is ": " (colon + space) and
			// a "$:" escape MANGLES the drive letter (FindFirstFileExA
			// on ":/REPOS/...").
			auto esc = [](const std::string& s) {
				std::string o;
				for (char c : s) {
					if (c == ' ' || c == '$')
						o += '$';
					o += c;
				}
				return o;
			};
			d << esc(header) << ": " << esc(path);
			for (const std::string& dep : deps)
				d << " \\\n  " << esc(dep);
			d << " \\\n  " << esc(p.DerivedDataDir() + "/assets.db");
			d << "\n";
		}
		std::cout << "Content ids: " << catalog.scenes.size()
			<< " scene(s), " << catalog.textures.size()
			<< " texture(s), " << catalog.meshes.size() << " mesh(es), "
			<< catalog.sounds.size() << " sound(s)\n"
			<< "  generation " << stamp.generation
			<< " (content fnv1a64:" << stamp.contentHash << ")"
			<< (stampRefresh ? " [forced]" : "") << "\n";
		for (const smvk::project::PakFingerprint& pak : pakPrints)
			std::cout << "  pak " << (pak.file.empty()
					? "?" : pak.file)
				<< ": " << pak.state << "\n";
		std::cout << "  project.h: " << header
			<< (wrote ? " (written)" : " (unchanged)") << "\n";
		return 0;
	}

	if (cmd == "build") {
		if (!recipeFile.empty()) {
			// The RECIPE build: resolve -> the shared executor (the same
			// plan the editor will drive) -> verified + policed paks ->
			// the appmanifest published last.
			std::string err;
			smvk::project::Project p =
				smvk::project::Project::Open(projectDir, err);
			if (!p.Valid()) {
				std::cerr << err << "\n";
				return 1;
			}
			smvk::project::ProjectRecipe recipe;
			const std::string path = resolveRecipe(p, recipe, err);
			if (path.empty()) {
				std::cerr << "recipe rejected: " << err << "\n";
				return 1;
			}
			smvk::project::BuildPlan plan;
			if (!smvk::project::ResolveRecipe(p, recipe, plan, err)) {
				std::cerr << "resolve failed: " << err << "\n";
				return 1;
			}
			// The manifest name: the explicit --manifest-name wins;
			// the default is the RECIPE stem (sponza.tourrecipe ->
			// sponza.tapp). (This condition was inverted at first
			// landing — a supplied name was silently overwritten; the
			// commit review caught it.)
			if (manifestName.empty())
				manifestName = std::filesystem::path(recipeFile)
					.stem().generic_string();
			smvk::project::BuildPlanOptions opts;
			opts.publishDir = publishDir;
			opts.manifestName = manifestName;
			// The generated project.h to re-stamp after the publish
			// (the <Name>_paks CMake target passes it; empty = the
			// editor's own flow / plain builds skip the rewrite).
			opts.projectHeaderPath = headerOut;
			auto adapter = smvk::project::MakeSubprocessStageAdapter();
			smvk::project::BuildPlanReport report;
			if (!smvk::project::ExecuteBuildPlan(p, plan, *adapter, opts,
					report, err)) {
				std::cerr << "build failed: " << err << "\n";
				return 1;
			}
			std::cout << "Built " << report.paks.size() << " pak(s):\n";
			for (const auto& pak : report.paks)
				std::cout << "  [" << pak.id << "] " << pak.output
					<< (pak.scene.empty() ? " (asset-only)" : "")
					<< (pak.reimported ? " (reimported)" : "") << "\n";
			std::cout << "  appmanifest: " << report.manifest << "\n";
			return 0;
		}
		if (output.empty()) {
			std::cerr << "--output <pak> (or --recipe <file>) is"
				" required.\n";
			return Usage(argv[0]);
		}
		std::string err;
		smvk::project::Project p =
			smvk::project::Project::Open(projectDir, err);
		if (!p.Valid()) {
			std::cerr << err << "\n";
			return 1;
		}
		smvk::project::BuildOptions opts;
		opts.scene = scene;
		opts.outputPak = output;
		smvk::project::BuildReport report;
		if (!smvk::project::BuildProject(p, opts, report, err)) {
			std::cerr << "build failed: " << err << "\n";
			return 1;
		}
		std::cout << "Built: " << report.pak
			<< "\n  scene=" << report.scene
			<< "\n  artifact=" << report.artifact
			<< "\n  reimported=" << (report.reimported ? "yes" : "no")
			<< "\n";
		return 0;
	}

	if (cmd == "status") {
		std::string err;
		smvk::project::Project p =
			smvk::project::Project::Open(projectDir, err);
		if (!p.Valid()) {
			std::cerr << err << "\n";
			return 1;
		}
		const std::vector<smvk::project::ImportStatus> rows =
			smvk::project::ProjectImportStatuses(p, &err);
		if (!err.empty())
			std::cerr << "registry: " << err << "\n";
		if (rows.empty()) {
			std::cout << "No imported sources (registry: "
				<< p.RegistryPath() << ")\n";
			return 0;
		}
		int stale = 0;
		for (const smvk::project::ImportStatus& r : rows) {
			std::cout << r.source << ": "
				<< (r.current ? "current" : "RE-DERIVE")
				<< " — " << r.why << "\n";
			if (!r.current)
				++stale;
		}
		std::cout << (rows.size() - stale) << "/" << rows.size()
			<< " current\n";
		return 0;
	}

	return Usage(argv[0]);
}
