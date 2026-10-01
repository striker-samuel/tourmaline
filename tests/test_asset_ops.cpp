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

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "editor_asset_ops.h"

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
		/ "vulkantoy_assetops";
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

void TestStandaloneFile() {
	const std::string root = Root();
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
	Write(std::string(gltf) + ".meta",
		std::string("guid=") + kGuidB + "\nsource=foo.gltf\nstem=foo\n"
		+ "mesh=foo.mesh " + kGuidC + " FooMesh\n"
		+ "material.0=foo.mat0_Base.material " + kGuidD + " Base\n");
	Write(root + "/content/models/foo.mesh", "meshblob");
	Write(root + "/content/models/foo.mat0_Base.material", "matblob");
	editor::AssetDeletePlan plan;
	std::string err;
	// A derived file alone refuses (the manifest would desync).
	Check(!editor::PlanAssetDelete(root + "/content/models/foo.mesh",
			root, plan, err)
		&& err.find("derived") != std::string::npos,
		"ops.derived.refuses-alone");
	// The source takes its derived set with it.
	Check(editor::PlanAssetDelete(gltf, root, plan, err)
		&& plan.also.size() == 3 // .meta + mesh + material
		&& !plan.notes.empty(), "ops.derived.source-takes-set");
	Check(editor::ExecuteAssetDelete(plan, err), "ops.derived.executes");
	namespace fs = std::filesystem;
	std::error_code ec;
	Check(!fs::exists(fs::path(gltf), ec)
		&& !fs::exists(
			fs::path(root + "/content/models/foo.mesh"), ec),
		"ops.derived.gone");
	fs::remove_all(root, ec);
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

} // namespace

int main() {
	TestStandaloneFile();
	TestRefusals();
	TestDerivedSet();
	TestCookAssetAndDir();
	std::cout << "checks=" << g_checks
		<< " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
