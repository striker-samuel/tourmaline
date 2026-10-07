#pragma once
// Asset-browser delete planning/execution (Unity Project-window Delete).
//
// Split deliberately: planning + execution are pure filesystem logic (no
// ImGui, no EditorApp) so tests pin the safety rules — the browser only
// confirms + logs. The rules:
//
//   - Containment: the target must live STRICTLY inside the project root
//     (normalized generic paths). The project root itself, the
//     project.tourproj marker, and the owned artifact cache
//     (artifacts/, legacy scratch/) are refused — those are project
//     structure, never content.
//   - Sidecars ride along: <file>.meta is deleted with its asset (no
//     orphan identity; the assetdb row stays as a MISSING retarget so
//     GUID references report cleanly instead of vanishing — the db's
//     standing "retarget, not a delete" contract).
//   - Derived assets (.geometry/.material/.anim listed in a sibling
//     <stem>.gltf.meta manifest) are REFUSED on their own — deleting one
//     would desync the manifest. Delete the source .gltf instead, which
//     takes its derived set with it (planned explicitly, noted in the
//     confirm prompt).
//   - Cook assets (.gicook/.lmcook) delete the manifest only — the baked
//     blob payload (gi/<guid>.*) is shared bake output and stays (noted
//     in the prompt).
//   - Directories delete recursively (entry count in the prompt).

#include <cstddef>
#include <string>
#include <vector>

namespace editor {

// What a confirmed delete will remove (all absolute generic paths).
struct AssetDeletePlan {
	std::string target;              // the selected path itself
	bool isDir = false;              // target is a directory
	size_t entryCount = 0;           // dir: recursive entries; file: 1
	std::vector<std::string> also;   // sidecar + derived files (files only)
	std::vector<std::string> notes;  // prompt lines ("+ 3 derived ...")
};

// Computes the plan (false + err when refused: outside the project,
// protected path, derived asset, missing file — never throws).
bool PlanAssetDelete(const std::string& absPath,
	const std::string& projectRoot, AssetDeletePlan& out,
	std::string& err);
// Removes every planned path (target + also). Best-effort order, first
// error wins in err (never throws).
bool ExecuteAssetDelete(const AssetDeletePlan& plan, std::string& err);

} // namespace editor
