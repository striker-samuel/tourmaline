#pragma once
// Bake-asset leftovers + GI manifest row parsing.
//
// History: baked outputs (GI octrees/volumes, lightmap atlases, cubemap
// captures) once rode GUID-derived paths with ".meta" identity sidecars
// (the bake-asset system). That machinery is REMOVED — bakes now ride
// STABLE VFS paths like mesh blobs (gi/<scene>.sggi, gi/<scene>_<n>.cvol
// + gi/<scene>.cvmanifest, gi/<scene>.regmanifest, gi/<scene>.sglm,
// textures/cubemaps/<base>[_N].ktx2 + <base>.cubemanifest); the pak's
// Gi registry carries the stable stems (scene data, like meshes), and
// the runtime resolves by path through the VFS overlay (pak or loose).
//
// What stays here:
//   - SplitManifestRow: the volume-manifest row parser. Rows are bare
//     VFS paths; legacy "<guid> <path>" rows from the bake-asset era are
//     still parsed (the guid column ignored) so old manifests keep
//     loading.
//   - IsBakeAssetGuid: the 32-hex test that recognizes legacy guid
//     columns.
//   - kBakeAssetKindGi / kBakeAssetKindCubemap: opaque assetdb kind
//     labels (the editor's bake-output collections).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace smvk::bake {

// Assetdb kind labels for bake outputs (the editor's collections).
inline constexpr const char* kBakeAssetKindGi = "gi";
inline constexpr const char* kBakeAssetKindCubemap = "cubemap";

// 32 lowercase hex chars (recognizes legacy bake-asset guid columns).
[[nodiscard]] bool IsBakeAssetGuid(std::string_view s);

// Manifest row path (the regmanifest/cvmanifest line lists bake payload
// vfs paths, one per line): legacy bake-asset rows carry the payload
// guid first ("<guid> <vfsPath>  # <note>"); current and hand-edited
// rows are bare paths. Returns the vfs path ("" for blanks/comments).
[[nodiscard]] inline std::string SplitManifestRow(std::string_view line) {
	// Inline '#' comments first (headers + the "# <note>" row tails).
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
