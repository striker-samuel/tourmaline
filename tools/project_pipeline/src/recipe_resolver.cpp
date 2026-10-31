// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The recipe resolver (see recipe_resolver.h). Three passes over the
// parsed recipe, all in AUTHORED order so failures are deterministic:
//   1) scene paks — load + validate every owning scene, walk its
//      references (nested .tours recurse, cycle-guarded) into the
//      ordered artifact list, and FORCE-own the geometry into the
//      scene's pak (cluster-internal: no cross-pak geometry, ever)
//   2) includes — expand every glob against the project tree, classify
//      by extension, claim ownership; a second claim on an owned path
//      is an error (RES-04: never silently dedup)
//   3) topological sort — dependencies first, authored order as the
//      stable tiebreak; the plan's pak order IS the mount order

#include <project_pipeline/recipe_resolver.h>

#include <filesystem>
#include <map>
#include <set>
#include <utility>

#include <Engine/Asset/tour.h>

namespace smvk::project {

namespace {

namespace fs = std::filesystem;

std::string LowerExt(const std::string& path) {
	std::string ext = fs::path(path).extension().generic_string();
	for (char& c : ext)
		c = char(std::tolower(static_cast<unsigned char>(c)));
	return ext;
}

std::vector<std::string> SplitSegments(const std::string& p) {
	std::vector<std::string> segs;
	size_t start = 0;
	while (start < p.size()) {
		const size_t end = p.find('/', start);
		if (end == std::string::npos) {
			segs.push_back(p.substr(start));
			break;
		}
		segs.push_back(p.substr(start, end - start));
		start = end + 1;
	}
	return segs;
}

bool HasWildcard(const std::string& s) {
	return s.find('*') != std::string::npos
		|| s.find('?') != std::string::npos;
}

// Which plan kind a matched file is. Family/support files (the .gltf/
// .bin sources, .material, .meta sidecars) are NOT assets — the stages
// discover them through the artifact families + the VFS; claiming them
// would double-ship content the packer already resolves.
PlanAssetKind ClassifyAsset(const std::string& path, bool& known) {
	known = true;
	const std::string ext = LowerExt(path);
	if (ext == ".geometry")
		return PlanAssetKind::Geometry;
	if (ext == ".png" || ext == ".jpg" || ext == ".jpeg"
			|| ext == ".tga" || ext == ".bmp" || ext == ".ktx2")
		return PlanAssetKind::Texture;
	if (ext == ".givolumemanifest" || ext == ".givolume"
			|| ext == ".cubemanifest")
		return PlanAssetKind::BakeFile;
	known = false;
	return PlanAssetKind::Texture;
}

// Expands ONE include pattern against the project tree into `hits`
// (project-relative, forward slashes; the caller's set keeps them
// sorted + unique). Returns false only on a filesystem walk error.
bool ExpandPattern(const fs::path& root, const std::string& pattern,
	std::set<std::string>& hits) {
	const std::vector<std::string> segs = SplitSegments(pattern);
	// The deepest wildcard-free prefix anchors the walk (an unanchored
	// '**' walks the whole project — correct, just slower).
	size_t staticCount = 0;
	while (staticCount < segs.size() && !HasWildcard(segs[staticCount]))
		++staticCount;
	fs::path base = root;
	for (size_t i = 0; i < staticCount; ++i)
		base /= segs[i];
	std::error_code ec;
	if (staticCount == segs.size()) {
		// A fully-static pattern: the exact file, claimed verbatim.
		if (fs::is_regular_file(base, ec))
			hits.insert(pattern);
		return true;
	}
	if (!fs::is_directory(base, ec))
		return true; // no matches; the caller's empty-match check fires
	fs::recursive_directory_iterator it(base, ec);
	const fs::recursive_directory_iterator end;
	while (!ec && it != end) {
		const fs::directory_entry entry = *it;
		if (entry.is_regular_file(ec)) {
			const std::string rel =
				fs::relative(entry.path(), root, ec).generic_string();
			if (!ec && !rel.empty()
					&& RecipeGlobMatch(pattern, rel))
				hits.insert(rel);
		}
		it.increment(ec);
	}
	return true;
}

// Loads + validates one scene tour (BuildProject's contract, extended
// to nested references): the tour loads, has a root reference, the
// root is project-relative and its artifact exists. Non-root
// references contribute their geometry (.geometry/.gltf/.glb) to the
// SAME pak; nested .tour references recurse (cycle-guarded) — a
// referenced scene's content is part of the referencing scene's
// cluster, so it ships with it.
bool CollectSceneArtifacts(const Project& project, const std::string& scene,
	std::vector<std::string>& artifacts, std::string& err,
	std::set<std::string>& visited) {
	std::error_code ec;
	const fs::path tourAbs = fs::path(project.root) / fs::path(scene);
	if (!fs::is_regular_file(tourAbs, ec)) {
		err = "scene tour not found: " + scene;
		return false;
	}
	if (!visited.insert(tourAbs.lexically_normal().generic_string()).second) {
		err = "nested scene reference cycle through: " + scene;
		return false;
	}
	smvk::tour::Doc doc;
	std::string loadErr;
	if (!smvk::tour::LoadTour(tourAbs.generic_string(), doc, loadErr)) {
		err = "tour load failed (" + scene + "): " + loadErr;
		return false;
	}
	if (doc.references.empty()) {
		err = "the scene tour has no root reference (nothing to build): "
			+ scene;
		return false;
	}
	// Root reference (#0): the artifact the packer takes as its primary
	// VFS input. PROJECT-RELATIVE only (an absolute reference breaks
	// mesh-URI registration — the same rejection BuildProject applies).
	const std::string& rootRef = doc.references[0].source;
	if (fs::path(rootRef).is_absolute()) {
		err = "the tour's root reference must be project-relative: '"
			+ rootRef + "' (" + scene + ")";
		return false;
	}
	if (!fs::is_regular_file(fs::path(project.root) / fs::path(rootRef),
			ec)) {
		err = "the tour's referenced artifact is missing: '" + rootRef
			+ "' (" + scene + ")";
		return false;
	}
	artifacts.push_back(rootRef);
	for (size_t i = 1; i < doc.references.size(); ++i) {
		const smvk::tour::ReferenceCmd& ref = doc.references[i];
		if (!ref.enabled || ref.source.empty())
			continue; // an unloaded placeholder carries no dependency
		if (fs::path(ref.source).is_absolute()) {
			err = "the tour's reference must be project-relative: '"
				+ ref.source + "' (" + scene + ")";
			return false;
		}
		const std::string ext = LowerExt(ref.source);
		if (ext == ".tour") {
			// Nested scene: its content joins THIS pak's cluster.
			if (!CollectSceneArtifacts(project, ref.source, artifacts, err,
					visited))
				return false;
		}
		else if (ext == ".geometry" || ext == ".gltf" || ext == ".glb") {
			if (!fs::is_regular_file(
					fs::path(project.root) / fs::path(ref.source), ec)) {
				err = "the tour's referenced artifact is missing: '"
					+ ref.source + "' (" + scene + ")";
				return false;
			}
			artifacts.push_back(ref.source);
		}
		else {
			err = "unsupported reference '" + ref.source + "' (" + scene
				+ "): references are .tour scenes or .geometry/.gltf"
					" artifacts";
			return false;
		}
	}
	return true;
}

} // namespace

bool ResolveRecipe(const Project& project, const ProjectRecipe& recipe,
	BuildPlan& out, std::string& err) {
	err.clear();
	out = BuildPlan{};
	if (!project.Valid()) {
		err = "invalid project (open/init first)";
		return false;
	}
	if (recipe.version != kRecipeGrammarVersion || recipe.paks.empty()
			|| recipe.entry.empty()) {
		err = "the recipe is not parsed/validated (ParseProjectRecipe"
			" first)";
		return false;
	}
	out.projectName = recipe.projectName;
	out.bakeGi = recipe.bakeGi;
	out.bakeCubemaps = recipe.bakeCubemaps;

	// ---- 1) the entry scene is owned by exactly one pak --------------
	// (the parser already verified ownership exists; a hand-built
	// recipe gets the same check here — and two paks claiming the same
	// scene is always wrong: a scene ships once)
	const RecipePak* entryPak = nullptr;
	for (const RecipePak& pak : recipe.paks) {
		if (pak.scene.empty() || pak.scene != recipe.entry)
			continue;
		if (entryPak) {
			err = "the entry scene is claimed by both [pak "
				+ entryPak->id + "] and [pak " + pak.id + "]";
			return false;
		}
		entryPak = &pak;
	}
	if (!entryPak) {
		err = "the entry scene '" + recipe.entry
			+ "' is not owned by any [pak] 'scene' directive";
		return false;
	}
	out.entryScene = recipe.entry;
	out.entryPak = entryPak->id;

	// Ownership table: path -> owner pak (+ how it was claimed, for the
	// conflict diagnostics). One entry per path, ever.
	struct Owned {
		std::string pak;
		PlanAssetKind kind;
		bool fromScene; // claimed as a scene's geometry vs an include
	};
	std::map<std::string, Owned> owned;

	// ---- 2) scene paks: validate + force-own their geometry ----------
	// Authored order. A scene's artifacts BELONG to its pak — a second
	// scene pak or an include claiming them is the v1 boundary hit:
	// meshes/materials are cluster-internal and cannot cross paks.
	std::map<std::string, std::vector<std::string>> pakArtifacts;
	for (const RecipePak& pak : recipe.paks) {
		if (pak.scene.empty())
			continue;
		std::vector<std::string> artifacts;
		std::set<std::string> visited;
		if (!CollectSceneArtifacts(project, pak.scene, artifacts, err,
				visited))
			return false;
		// order-preserving dedupe (a nested tour may share a reference
		// with the parent)
		{
			std::set<std::string> seen;
			std::vector<std::string> unique;
			for (const std::string& a : artifacts)
				if (seen.insert(a).second)
					unique.push_back(a);
			artifacts.swap(unique);
		}
		for (const std::string& a : artifacts) {
			auto it = owned.find(a);
			if (it != owned.end()) {
				err = "[pak " + pak.id + "] references '" + a
					+ "' already owned by [pak " + it->second.pak + "] —"
						" meshes/materials are cluster-internal and"
						" cannot be shared across paks (v1): give one"
						" pak the scene, the other a 'depends' edge";
				return false;
			}
			owned[a] = { pak.id, PlanAssetKind::Geometry, true };
		}
		pakArtifacts[pak.id] = std::move(artifacts);
	}

	// ---- 3) includes: expand + claim (authored order) -----------------
	for (const RecipePak& pak : recipe.paks) {
		std::set<std::string> hits;
		for (const std::string& pattern : pak.includes) {
			std::set<std::string> one;
			if (!ExpandPattern(fs::path(project.root), pattern, one)) {
				err = "[pak " + pak.id + "] include walk failed: '"
					+ pattern + "'";
				return false;
			}
			if (one.empty()) {
				// An include is a CLAIM — a glob matching nothing is a
				// broken claim (typo'd directory), not an empty pak.
				err = "[pak " + pak.id + "] include '" + pattern
					+ "' matched no files";
				return false;
			}
			hits.insert(one.begin(), one.end());
		}
		size_t assetsClaimed = 0;
		for (const std::string& rel : hits) {
			bool known = false;
			const PlanAssetKind kind = ClassifyAsset(rel, known);
			if (!known)
				continue; // family/support file — the stages' discovery
			if (kind == PlanAssetKind::Geometry && pak.scene.empty()) {
				// Meshes are cluster-internal: a .geometry in a
				// scene-less pak is unreachable content (nothing loads
				// it — meshes are not VFS-addressable). The scene's pak
				// owns its geometry; an asset-only pak carries
				// VFS-addressable content only.
				err = "[pak " + pak.id + "] include claims the geometry '"
					+ rel + "' but owns no scene — meshes are"
						" cluster-internal (v1): give the pak the scene"
						" that uses it, or share textures instead";
				return false;
			}
			auto it = owned.find(rel);
			if (it != owned.end()) {
				if (it->second.pak == pak.id)
					continue; // already ours (our own scene's artifact
					          // matched by our own glob)
				err = "[pak " + pak.id + "] include claims '" + rel
					+ "' already owned by [pak " + it->second.pak + "]"
					+ (it->second.fromScene
						? " (a scene's geometry is cluster-internal —"
							" it cannot move into an include pak)"
						: "")
					+ " — every cooked artifact has exactly one owner";
				return false;
			}
			owned[rel] = { pak.id, kind, false };
			++assetsClaimed;
		}
		if (pak.scene.empty() && assetsClaimed == 0) {
			err = "[pak " + pak.id + "] owns no content: its includes"
				" matched no asset files";
			return false;
		}
	}

	// ---- 4) topological sort (dependencies first, authored tiebreak) --
	// The parser rejected unknown targets + cycles; re-verify so a
	// hand-built recipe can't smuggle either past the plan.
	std::map<std::string, size_t> idIndex;
	for (size_t i = 0; i < recipe.paks.size(); ++i) {
		const std::string id = recipe.paks[i].id;
		if (!idIndex.emplace(id, i).second) {
			err = "duplicate pak id '" + id + "'";
			return false;
		}
	}
	for (const RecipePak& pak : recipe.paks)
		for (const std::string& dep : pak.depends)
			if (idIndex.find(dep) == idIndex.end()) {
				err = "[pak " + pak.id + "] depends on unknown pak '"
					+ dep + "'";
				return false;
			}
	std::vector<size_t> order;
	{
		std::vector<int> placed(recipe.paks.size(), 0);
		while (order.size() < recipe.paks.size()) {
			// The authored-earliest unplaced pak whose dependencies are
			// all placed — deterministic, and dependencies always land
			// before their dependents.
			bool progressed = false;
			for (size_t i = 0; i < recipe.paks.size(); ++i) {
				if (placed[i])
					continue;
				bool ready = true;
				for (const std::string& dep : recipe.paks[i].depends)
					if (!placed[idIndex.at(dep)]) {
						ready = false;
						break;
					}
				if (ready) {
					placed[i] = 1;
					order.push_back(i);
					progressed = true;
					break; // restart the scan: authored order first
				}
			}
			if (!progressed) {
				err = "dependency cycle among [pak] sections";
				return false;
			}
		}
	}

	// ---- 5) assemble the plan ------------------------------------------
	for (size_t idx : order) {
		const RecipePak& rp = recipe.paks[idx];
		PlanPak pp;
		pp.id = rp.id;
		pp.output = rp.output;
		pp.scene = rp.scene;
		pp.transitive = rp.dependenciesTransitive;
		pp.depends = rp.depends;
		pp.line = rp.line;
		if (!pp.scene.empty())
			pp.sceneArtifacts = pakArtifacts.at(pp.id);
		// `owned` iterates in path order — assets come out sorted.
		for (const auto& [path, own] : owned)
			if (own.pak == pp.id)
				pp.assets.push_back(PlanAsset{ path, own.kind });
		out.paks.push_back(std::move(pp));
	}
	return true;
}

} // namespace smvk::project
