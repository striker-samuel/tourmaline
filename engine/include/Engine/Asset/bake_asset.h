#pragma once
// Bake-asset identity: GI bakes + cubemap captures as first-class assets.
//
// The problem this solves: every baked output was keyed by a human stem
// (gi/<scene>.sglm, gi/<scene>.sggi, textures/cubemaps/<name>[_N].ktx2,
// one-vfs-path-per-line manifests) resolved path-based through the VFS.
// Renaming a pak/scene orphaned its bakes; rebakes silently minted
// unrelated files; the asset browser + assetdb never saw them — unlike
// .mesh/.material, which bind by GUID.
//
// The model (mirrors the source-asset sidecars):
//   - Every baked payload + every bake manifest carries a per-file
//     ".meta" sidecar: guid=<32hex> (fresh at first bake, stable across
//     rebakes), kind=gi|cubemap, logical=<stable rebake key>.
//   - Cooked/runtime VFS paths are GUID-DERIVED (gi/<guid>.sglm,
//     textures/cubemaps/<guid>.ktx2); the manifests list those paths.
//   - The pak's Gi registry carries the guids (Probe/Lightmap names are
//     already strings — new paks record guids there; new RegManifest /
//     CvolManifest / Cubemap kinds cover the manifests); the runtime stays
//     path-based (it never queries the assetdb — per the registry's
//     layering line), the paths are just stable now.
//
// Kinds (the sidecar kind= line, the assetdb row, the browser
// collections): "gi" covers every GI payload (the lightmap atlas, probe
// octrees, regular + convex volumes and their manifests); "cubemap"
// covers captures and their placement manifests.
//
// Logical keys (the sidecar logical= line — what a rebake scans for):
//   lightmap:<scene>       the scene's .sglm atlas (scene = pak stem)
//   probe:<name>            a .sggi world-field octree
//   regmanifest:<scene>     the scene's .regmanifest
//   cvolmanifest:<scene>    the scene's .cvmanifest
//   cubemanifest:<base>     a capture set's .cubemanifest (base = the
//                          capture-set name: --cubemap-name or pak stem)
// Payload files need no logical (their identity IS the filename guid);
// multi-row manifests match rebake rows by capture position, with the
// row payload guids read from the previous row payloads' sidecars.

#include <cstddef>
#include <cstdint>
#include <string>

namespace smvk::bake {

// Sidecar kinds (see above).
inline constexpr const char* kBakeAssetKindGi = "gi";
inline constexpr const char* kBakeAssetKindCubemap = "cubemap";

// 32 lowercase hex chars (the asset-identity rule, shared with the
// source-side GUIDs).
[[nodiscard]] bool IsBakeAssetGuid(std::string_view s);

// <filePath>.meta contents (missing/invalid file -> false).
struct BakeAssetSidecar {
	std::string guid;
	std::string kind;    // kBakeAssetKindGi | kBakeAssetKindCubemap | ""
	std::string logical; // rebake key (see above); "" when absent
};
[[nodiscard]] bool ReadBakeAssetSidecar(const std::string& filePath,
	BakeAssetSidecar& out, std::string& err);
// Writes (truncates) <filePath>.meta. guid must be valid.
[[nodiscard]] bool WriteBakeAssetSidecar(const std::string& filePath,
	std::string_view guid, std::string_view kind,
	std::string_view logical, std::string& err);

// First bake: mint a fresh guid. Rebake: reuse the guid whose sidecar
// matches (kind, logical) under dir (the sidecar's own file may live
// anywhere under dir — the match is by sidecar CONTENT, not by name, so
// renames heal: the next bake finds it only if it is still under dir).
// Returns "" + err on failure (never throws). The caller writes the
// payload to its guid-derived path + refreshes the sidecar there.
[[nodiscard]] std::string EnsureBakeAssetGuid(const std::string& dir,
	std::string_view kind, std::string_view logical, std::string& err);

// Non-minting twin: the guid of a previous bake matching (kind,
// logical) under dir, or "" (no sidecar claimed it). Cook-side lookups
// (probe names, manifest discovery) use this — a miss just means
// "unbaked under the new scheme", never an error.
[[nodiscard]] std::string FindBakeAssetGuid(const std::string& dir,
	std::string_view kind, std::string_view logical);

// Guid for an already-baked payload file (its <file>.meta guid, or "" —
// legacy unguided payload).
[[nodiscard]] std::string PayloadBakeGuid(const std::string& filePath);

// Manifest row path (the regmanifest/cvmanifest line lists bake payload
// vfs paths, one per line): bake-asset rows carry the payload guid first
// ("<guid> <vfsPath>  # <note>"); legacy and hand-edited rows are bare
// paths. Returns the vfs path ("" for blanks/comments).
[[nodiscard]] inline std::string SplitManifestRow(std::string_view line) {
	// Inline '#' comments first (GiBaker writes a header comment + the
	// "# <note>" row tails).
	if (const size_t hash = line.find('#'); hash != std::string_view::npos)
		line = line.substr(0, hash);
	// Trim.
	while (!line.empty()
		&& (line.back() == ' ' || line.back() == '\t'
			|| line.back() == '\r' || line.back() == '\n'))
		line.remove_suffix(1);
	while (!line.empty()
		&& (line.front() == ' ' || line.front() == '\t'))
		line.remove_prefix(1);
	if (line.empty())
		return {};
	// "<guid> <path>": the first token is a 32-hex guid.
	const size_t sp = line.find_first_of(" \t");
	if (sp != std::string_view::npos) {
		const std::string_view first = line.substr(0, sp);
		if (IsBakeAssetGuid(first)) {
			std::string_view rest = line.substr(sp + 1);
			while (!rest.empty()
				&& (rest.front() == ' ' || rest.front() == '\t'))
				rest.remove_prefix(1);
			const size_t end = rest.find_first_of(" \t");
			return std::string(
				end == std::string_view::npos ? rest : rest.substr(0, end));
		}
	}
	return std::string(line);
}

} // namespace smvk::bake
