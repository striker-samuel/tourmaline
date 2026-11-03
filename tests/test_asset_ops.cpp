// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// asset-ops tests: the asset browser's delete planning/execution
// (editor/editor_asset_ops.h) — pure filesystem logic, no ImGui.
//
// What it pins down:
//   1. standalone files plan as [file + .meta sidecar] (no orphan GUIDs).
//   2. protected structure refuses (project root, the tourproj marker,
//      the owned artifact cache) and outside-project paths refuse.
//   3. manifest-derived assets (.mesh listed in a sibling .gltf.meta)
//      refuse alone; the source .gltf plans its derived set with it.
//   4. cook assets note that the blob payload stays.
//   5. directories plan recursively; execution removes everything.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "editor_asset_ops.h"
#include "editor_asset_browser_nav.h"
#include "editor_live_preview_policy.h"
#include "editor_asset_inspect.h"
#include "editor_watch_routes.h"
#include "editor_asset_dcc.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	++g_checks;
	if (cond) {
		std::cout << "PASS " << name << "\n";
	}
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

std::string Root() {
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::path dir = fs::temp_directory_path(ec)
		/ "tourmaline_assetops";
	fs::remove_all(dir, ec);
	fs::create_directories(dir / "content" / "models", ec);
	fs::create_directories(dir / "content" / "scenes", ec);
	fs::create_directories(dir / "artifacts", ec);
	return dir.generic_string();
}

void Write(const std::string& path, const std::string& text) {
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	f << text;
}

constexpr const char* kGuidA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* kGuidB = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr const char* kGuidC = "cccccccccccccccccccccccccccccccc";
constexpr const char* kGuidD = "dddddddddddddddddddddddddddddddd";
constexpr const char* kGuidE = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";

// ---------------------------------------------------------------------------
// The watched-project ROUTING TABLE (editor_watch_routes.h — the item-16
// classifier ConsumeWatchedProjectChanges switches on): a real scratch
// tree pins which settled change lands in which funnel. The contract
// this guards: a texture SOURCE (image beside its .texture) must reach
// the live-refresh funnel, a BARE image must NOT (it would pool-swap
// something that is not a .texture asset), and the .tour log-only rule
// must never rot into an auto-replay.
// ---------------------------------------------------------------------------

void TestWatchRoutes() {
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path dir = fs::temp_directory_path(ec)
		/ "tourmaline_watchroutes";
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);
	const auto touch = [&dir](const char* name) {
		Write((dir / name).generic_string(), "x");
		return dir / name;
	};
	using R = editor::WatchRoute;
	Check(editor::ClassifyWatchedChange(touch("scene.gltf"))
			== R::GltfSource, "watchroute.gltf");
	Check(editor::ClassifyWatchedChange(touch("model.GLB"))
			== R::GltfSource, "watchroute.glb-case-insensitive");
	Check(editor::ClassifyWatchedChange(touch("statue.obj"))
			== R::GltfSource, "watchroute.obj");
	Check(editor::ClassifyWatchedChange(touch("asset.meta"))
			== R::MetaSidecar, "watchroute.meta");
	Check(editor::ClassifyWatchedChange(touch("game.tour"))
			== R::TourDoc, "watchroute.tour");
	Check(editor::ClassifyWatchedChange(touch("game.tourrecipe"))
			== R::TourDoc, "watchroute.tourrecipe");
	// The texture leg: an image BESIDE its .texture asset.
	{
		touch("brick.png");
		touch("brick.texture");
		Check(editor::ClassifyWatchedChange(dir / "brick.png")
				== R::TextureSource, "watchroute.texture-source");
		Check(editor::ClassifyWatchedChange(dir / "brick.ktx2")
				== R::TextureSource,
			"watchroute.texture-source-ktx2-sibling-covers");
	}
	// A BARE image (no .texture beside it): Other — glTF source images
	// ride the artifact's content digest, never a pool swap.
	Check(editor::ClassifyWatchedChange(touch("bare.jpg"))
			== R::Other, "watchroute.bare-image-other");
	Check(editor::ClassifyWatchedChange(touch("note.txt"))
			== R::Other, "watchroute.other");
	// Case-insensitive extension match (a DCC saving X.PNG).
	{
		touch("UPPER.PNG");
		Check(editor::ClassifyWatchedChange(dir / "UPPER.PNG")
				== R::Other, "watchroute.upper-png-no-asset");
	}
	fs::remove_all(dir, ec);
}

void TestDccSourcePublishing() {
	namespace fs = std::filesystem;
	const std::string root = Root();
	const fs::path native = fs::path(root) / "content" / "models" / "ship.ma";
	const fs::path published = fs::path(root) / "content" / "models" / "ship.gltf";
	Write(native.generic_string(), "Maya ASCII source");
	{
		editor::DccAsset resolved;
		std::string err;
		Check(editor::ResolveDccAsset(native.generic_string(), resolved, err)
			&& resolved.linked && resolved.fromNative
			&& resolved.document == native.generic_string()
			&& resolved.published == published.generic_string(),
			"dcc.native-source-maps-to-gltf");
	}
	Write(published.generic_string(), "{\"asset\":{\"version\":\"2.0\"}}");
	{
		std::string err;
		Check(editor::LinkDccSource(published.generic_string(),
				native.generic_string(), err),
			"dcc.link-source-to-publication");
		editor::DccAsset resolved;
		Check(editor::ResolveDccAsset(published.generic_string(),
				resolved, err) && resolved.linked
				&& resolved.document == native.generic_string(),
			"dcc.published-source-round-trip");
		Check(editor::HideAssetBrowserFile(
			published.generic_string() + ".dccsource"),
			"dcc.link-sidecar-hidden-from-browser");
			Check(editor::UnlinkDccSource(published.generic_string(), err),
			"dcc.unlink-source");
		Check(editor::ResolveDccAsset(published.generic_string(),
				resolved, err) && !resolved.linked
				&& resolved.document == resolved.published,
			"dcc.unlinked-falls-back-to-published");
	}
	{
		std::string err;
		Check(editor::LinkDccSource(published.generic_string(),
				native.generic_string(), err), "dcc.relink-before-delete");
		editor::AssetDeletePlan plan;
		Check(editor::PlanAssetDelete(published.generic_string(),
				root, plan, err)
				&& std::find(plan.also.begin(), plan.also.end(),
					published.generic_string() + ".dccsource") != plan.also.end(),
			"dcc.delete-removes-authoring-link");
	}
	std::error_code ec;
	fs::remove_all(root, ec);
}

void TestStandaloneFile() {	const std::string root = Root();
	const std::string file = root + "/content/bar.txt";
	Write(file, "hello");
	Write(file + ".meta", std::string("guid=") + kGuidA
		+ "\nimporter=model\nversion=1\n");
	editor::AssetDeletePlan plan;
	std::string err;
	Check(editor::PlanAssetDelete(file, root, plan, err)
		&& !plan.isDir && plan.entryCount == 1
		&& plan.also.size() == 1
		&& plan.also[0] == file + ".meta",
		"ops.file.sidecar-rides-along");
	Check(editor::ExecuteAssetDelete(plan, err), "ops.file.executes");
	namespace fs = std::filesystem;
	std::error_code ec;
	Check(!fs::exists(fs::path(file), ec)
		&& !fs::exists(fs::path(file + ".meta"), ec),
		"ops.file.gone");
	fs::remove_all(root, ec);
}

void TestRefusals() {
	const std::string root = Root();
	editor::AssetDeletePlan plan;
	std::string err;
	// Missing file.
	Check(!editor::PlanAssetDelete(root + "/content/nope.txt", root,
			plan, err), "ops.refuse.missing");
	// Outside the project.
	Check(!editor::PlanAssetDelete(
			(std::filesystem::temp_directory_path() / "x.txt")
				.generic_string(), root, plan, err),
		"ops.refuse.outside");
	// Project root itself.
	Check(!editor::PlanAssetDelete(root, root, plan, err),
		"ops.refuse.root");
	// The project marker.
	Write(root + "/project.tourproj", "name=t\nversion=1\n");
	Check(!editor::PlanAssetDelete(root + "/project.tourproj", root,
			plan, err), "ops.refuse.marker");
	// The owned artifact cache (and its legacy scratch twin).
	Write(root + "/artifacts/cache.db", "x");
	{
		std::error_code cec;
		std::filesystem::create_directories(root + "/scratch", cec);
	}
	Write(root + "/scratch/old.txt", "x");
	Check(!editor::PlanAssetDelete(root + "/artifacts/cache.db", root,
			plan, err), "ops.refuse.artifacts");
	Check(!editor::PlanAssetDelete(root + "/scratch/old.txt", root,
			plan, err), "ops.refuse.scratch");
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
}

void TestDerivedSet() {
	const std::string root = Root();
	const std::string gltf = root + "/content/models/foo.gltf";
	Write(gltf, "{}");
	// The manifest's data-blob row is "geometry=" now (the pre-10/6
	// ".mesh" derived asset renamed — ".mesh" is the higher placeable
	// asset's name; a legacy "mesh=" row still parses, see ParseManifestLine).
	Write(std::string(gltf) + ".meta",
		std::string("guid=") + kGuidB + "\nsource=foo.gltf\nstem=foo\n"
		+ "geometry=foo.geometry " + kGuidC + " FooGeometry\n"
		+ "material.0=foo.mat0_Base.material " + kGuidD + " Base\n");
	Write(root + "/content/models/foo.geometry", "geometryblob");
	Write(root + "/content/models/foo.mat0_Base.material", "matblob");
	editor::AssetDeletePlan plan;
	std::string err;
	// A derived file alone refuses (the manifest would desync).
	Check(!editor::PlanAssetDelete(root + "/content/models/foo.geometry",
			root, plan, err)
		&& err.find("derived") != std::string::npos,
		"ops.derived.refuses-alone");
	// The source takes its derived set with it.
	Check(editor::PlanAssetDelete(gltf, root, plan, err)
		&& plan.also.size() == 3 // .meta + geometry + material
		&& !plan.notes.empty(), "ops.derived.source-takes-set");
	Check(editor::ExecuteAssetDelete(plan, err), "ops.derived.executes");
	namespace fs = std::filesystem;
	std::error_code ec;
	Check(!fs::exists(fs::path(gltf), ec)
		&& !fs::exists(
			fs::path(root + "/content/models/foo.geometry"), ec),
		"ops.derived.gone");
	fs::remove_all(root, ec);
}

void TestDerivedSetLegacyMesh() {
	// The legacy manifest form ("mesh=<file>.mesh" rows, pre-10/6
	// derivations) still parses + plans: the blob delete refuses alone,
	// the source takes the derived set.
	const std::string root = Root();
	const std::string gltf = root + "/content/models/foo.gltf";
	Write(gltf, "{}");
	Write(std::string(gltf) + ".meta",
		std::string("guid=") + kGuidB + "\nsource=foo.gltf\nstem=foo\n"
		+ "mesh=foo.mesh " + kGuidC + " FooMesh\n");
	Write(root + "/content/models/foo.mesh", "meshblob");
	editor::AssetDeletePlan plan;
	std::string err;
	Check(!editor::PlanAssetDelete(root + "/content/models/foo.mesh",
			root, plan, err), "ops.derived.legacy-mesh.refuses-alone");
	Check(editor::PlanAssetDelete(gltf, root, plan, err)
		&& plan.also.size() == 2, "ops.derived.legacy-mesh.set");
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
}

void TestCookAssetAndDir() {
	const std::string root = Root();
	const std::string cook = root + "/content/scenes/main.gicook";
	Write(cook, std::string("guid=") + kGuidE + "\nname=main\n");
	Write(cook + ".meta", std::string("guid=") + kGuidE
		+ "\nimporter=gicook\nversion=1\n");
	editor::AssetDeletePlan plan;
	std::string err;
	Check(editor::PlanAssetDelete(cook, root, plan, err)
		&& plan.also.size() == 1 && !plan.notes.empty()
		&& plan.notes[0].find("blob") != std::string::npos,
		"ops.cook.blob-stays-noted");
	// Directories plan recursively (exactly the two files above —
	// the cook asset lives in content/scenes, outside this subtree).
	Write(root + "/content/models/a.txt", "a");
	Write(root + "/content/models/b.txt", "b");
	Check(editor::PlanAssetDelete(root + "/content/models", root,
			plan, err)
		&& plan.isDir && plan.entryCount == 2
		&& !plan.notes.empty(), "ops.dir.recursive");
	Check(editor::ExecuteAssetDelete(plan, err), "ops.dir.executes");
	namespace fs = std::filesystem;
	std::error_code ec;
	Check(!fs::exists(fs::path(root + "/content/models"), ec),
		"ops.dir.gone");
	fs::remove_all(root, ec);
}

void TestAssetBrowserSelectionClears() {
    std::string primary = "project/content/models/mesh.geometry";
    std::vector<std::string> selection = {
        primary, "project/content/models/other.geometry"
    };
    std::string anchor = primary;
    editor::ClearAssetBrowserSelection(primary, selection, anchor);
    Check(primary.empty() && selection.empty() && anchor.empty(),
        "browser.navigation.clears-multi-selection");
}

void TestVirtualFolderLocation() {
    const std::string root = "project";
    Check(editor::AssetBrowserAtProjectRoot(root, root),
        "browser.root.is-valid");
    Check(!editor::AssetBrowserAtProjectRoot(root, root + "/content"),
        "browser.content.not-root");
    Check(!editor::AssetBrowserAtProjectRoot("", ""),
        "browser.no-project.not-root");

    Check(editor::ShowAssetBrowserCollectionFolders(
            root, root, false, false), "browser.virtual.root");
    Check(!editor::ShowAssetBrowserCollectionFolders(
            root, root + "/content", false, false),
        "browser.virtual.content-hidden");
    Check(!editor::ShowAssetBrowserCollectionFolders(
            root, root + "/content/models", false, false),
        "browser.virtual.nested-hidden");
    Check(!editor::ShowAssetBrowserCollectionFolders(
            root, root, true, false), "browser.virtual.gltf-expanded-hidden");
    Check(!editor::ShowAssetBrowserCollectionFolders(
            root, root, false, true), "browser.virtual.tour-expanded-hidden");
}


void TestAssetBrowserHiddenArtifacts() {
	using editor::HideAssetBrowserFile;
	Check(HideAssetBrowserFile("content/sky.cubemap"),
		"browser.hidden.cubemap");
	Check(HideAssetBrowserFile("content/sky.CUBEMAP"),
		"browser.hidden.cubemap-case-insensitive");
	Check(HideAssetBrowserFile("content/main.gicook"),
		"browser.hidden.gicook");
	Check(HideAssetBrowserFile("content/captures.CUBEMANIFEST"),
		"browser.hidden.cubemanifest");
	Check(HideAssetBrowserFile("content/legacy.bin", "cubemap"),
		"browser.hidden.db-kind-cubemap");
	Check(HideAssetBrowserFile("content/legacy.bin", "GICOOK"),
		"browser.hidden.db-kind-gicook");
	Check(!HideAssetBrowserFile("content/scene.tour"),
		"browser.visible.tour");
	Check(!HideAssetBrowserFile("content/readme.txt"),
		"browser.visible.text");
	Check(!HideAssetBrowserFile("content/scene.lmcook", "lmcook"),
		"browser.visible.lightmap-cook");
	Check(!HideAssetBrowserFile("content/scene.givolume", "givolume"),
		"browser.visible.givolume");
	Check(!HideAssetBrowserFile("content/sky.texture", "texture"),
		"browser.visible.texture");
}

void TestAssetInspectorFallback() {
	using editor::BrowserInspectorKind;
	using editor::ClassifyBrowserInspector;
	using editor::ReadBrowserFilePreview;
	Check(ClassifyBrowserInspector("foo.GICOOK") == BrowserInspectorKind::GiCook,
		"inspector.gicook");
	Check(ClassifyBrowserInspector("foo.LMCOOK") == BrowserInspectorKind::LmCook,
		"inspector.lmcook");
	Check(ClassifyBrowserInspector("foo.CUBEMAP") == BrowserInspectorKind::Cubemap,
		"inspector.cubemap");
	Check(ClassifyBrowserInspector("foo.GIVOLUME") == BrowserInspectorKind::Givolume,
		"inspector.givolume");
	Check(ClassifyBrowserInspector("old.MESH") == BrowserInspectorKind::Geometry,
		"inspector.legacy-mesh");
	Check(ClassifyBrowserInspector("notes.TXT") == BrowserInspectorKind::Generic,
		"inspector.generic-text");
	Check(ClassifyBrowserInspector("photo.PNG") == BrowserInspectorKind::TextureSource,
		"inspector.image-source");

	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / "tourmaline_inspector";
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);
	Check(ReadBrowserFilePreview(dir).isDirectory, "inspector.directory");
	Write((dir / "note.txt").string(), "hello\nworld");
	const auto text = ReadBrowserFilePreview(dir / "note.txt");
	Check(text.readOk && text.isText && text.text == "hello\nworld",
		"inspector.text");
	Write((dir / "binary.dat").string(), std::string("a\0b", 3));
	const auto binary = ReadBrowserFilePreview(dir / "binary.dat");
	Check(binary.readOk && !binary.isText && binary.fileSize == 3,
		"inspector.binary");
	Write((dir / "large.txt").string(), "abcde");
	const auto capped = ReadBrowserFilePreview(dir / "large.txt", 3);
	Check(capped.isText && capped.truncated && capped.text == "abc",
		"inspector.bounded");
	Write((dir / "unicode.txt").string(), "\xE2\x82\xAC hello");
	const auto unicode = ReadBrowserFilePreview(dir / "unicode.txt", 2);
	Check(unicode.isText && unicode.text.empty() && unicode.truncated,
		"inspector.utf8-cutoff");
	Check(!ReadBrowserFilePreview(dir / "not-here.txt").readOk,
		"inspector.missing");
	fs::remove_all(dir, ec);
}

void TestLiveBakedPreviewRouting() {
	enum class Mode { Lit, Baked, Unlit };
	const Mode normal[4] = { Mode::Lit, Mode::Unlit,
		Mode::Unlit, Mode::Unlit };
	const auto selected = [](const Mode* modes, int count, int active) {
		return editor::SelectLivePreviewViewport(count, active,
			[&](int vp) { return modes[vp]; }, Mode::Lit, Mode::Baked);
	};
	Check(selected(normal, 1, 0) == 0,
		"preview.single.lit-hosts");
	Check(selected(normal, 4, 3) == 0,
		"preview.quad.lit-stays-in-0");
	const Mode activeBaked[4] = { Mode::Lit, Mode::Unlit,
		Mode::Baked, Mode::Unlit };
	Check(selected(activeBaked, 4, 2) == 2,
		"preview.quad.active-baked-hosts");
	Check(selected(activeBaked, 4, 1) == 2,
		"preview.quad.inactive-baked-stays-hosted");
	const Mode noLit[4] = { Mode::Unlit, Mode::Unlit,
		Mode::Unlit, Mode::Unlit };
	Check(selected(noLit, 1, 0) == -1,
		"preview.unlit-uses-editor-path");
	const Mode noLitBaked[4] = { Mode::Unlit, Mode::Baked,
		Mode::Unlit, Mode::Unlit };
	Check(selected(noLitBaked, 4, 1) == 1,
		"preview.baked-without-lit-hosts");
	const Mode twoBaked[4] = { Mode::Baked, Mode::Baked,
		Mode::Unlit, Mode::Unlit };
	Check(selected(twoBaked, 4, 1) == 1,
		"preview.multiple-baked-prefers-active");
	Check(selected(normal, 0, 0) == -1,
		"preview.zero-views");
}

} // namespace

int main() {
	TestDccSourcePublishing();
	TestStandaloneFile();
	TestRefusals();
	TestDerivedSet();
	TestDerivedSetLegacyMesh();
	TestCookAssetAndDir();
	TestWatchRoutes();
	TestVirtualFolderLocation();
	TestAssetBrowserSelectionClears();
	TestAssetInspectorFallback();
	TestAssetBrowserHiddenArtifacts();
	TestLiveBakedPreviewRouting();
	std::cout << "checks=" << g_checks
		<< " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
