// Cook-asset creation (see EditorApp::CreateCookAsset): fresh UNBAKED
// .gicook/.lmcook manifests in the project, adopted into the assetdb.

#include "editor_app.h"

#include <filesystem>

namespace editor {

std::string EditorApp::CreateCookAsset(bool gi, const std::string& dir) {
	namespace fs = std::filesystem;
	if (m_projectRoot.empty()) {
		Log("New cook asset: open or create a project first (File > New / "
			"Open Project...) — cook assets live inside the project.");
		return {};
	}
	// Placement dir: the browser dir when it is a live project dir,
	// else the content home (same healing as DrawAssetsPanel).
	std::error_code ec;
	std::string target = dir;
	if (target.empty() || !fs::is_directory(fs::path(target), ec)
		|| !InProjectDir(target)) {
		target = (fs::path(m_projectRoot) / "content").generic_string();
		fs::create_directories(fs::path(target), ec);
	}
	const char* ext = gi ? smvk::cook::kGiCookExt
		: smvk::cook::kLightmapCookExt;
	const char* kind = gi ? smvk::cook::kCookAssetKindGiCook
		: smvk::cook::kCookAssetKindLightmapCook;
	const std::string base = gi ? "NewGiCook" : "NewLightmapCook";
	// Unique file name (never overwrite: NewGiCook.gicook, NewGiCook1…).
	fs::path path;
	for (int i = 0; i < 10000; ++i) {
		const std::string stem = base
			+ (i == 0 ? std::string() : std::to_string(i));
		path = fs::path(target) / (stem + ext);
		if (!fs::exists(path, ec))
			break;
		path.clear();
	}
	if (path.empty()) {
		Log("New cook asset: no free file name under '" + target + "'.");
		return {};
	}
	// Scene seed: the open scene's stem when one is open ("" = unbound —
	// the asset still creates; the scene binds at bake time).
	std::string scene;
	if (m_hasGltfModel || !m_pakPath.empty())
		scene = SceneIdForHost();
	const std::string guid = smvk::tour::GenerateUuid();
	std::string werr;
	bool ok = false;
	if (gi) {
		// Settings ride the CURRENT cook dialog (a fresh asset bakes
		// what the dialog would bake today).
		smvk::cook::GiCookAsset a =
			smvk::cook::DefaultGiCookAsset(
				path.stem().string(), scene);
		a.guid = guid;
		a.cvol = m_cook.giCvol;
		a.world = m_cook.giWorld;
		ok = smvk::cook::SaveGiCookAsset(path.generic_string(), a,
			werr);
	}
	else {
		smvk::cook::LightmapCookAsset a =
			smvk::cook::DefaultLightmapCookAsset(
				path.stem().string(), scene);
		a.guid = guid;
		a.lmMetersPerTexel = m_cook.lmMetersPerTexel;
		a.lmLobes = m_cook.lmLobes;
		a.lmAtlasWidth = m_cook.lmAtlasWidth;
		a.lmRays = m_cook.lmRays;
		a.lmProxyRes = m_cook.lmProxyRes;
		a.lmExcludeSmall = m_cook.lmExcludeSmall;
		a.lmExcludeKw = m_cook.lmExcludeKw;
		a.sunAzimuth = m_cook.sunAzimuth;
		a.sunElevation = m_cook.sunElevation;
		a.sunIntensity = m_cook.sunIntensity;
		a.skyIntensity = m_cook.skyIntensity;
		a.sunColor = m_cook.sunColor;
		ok = smvk::cook::SaveLightmapCookAsset(path.generic_string(),
			a, werr);
	}
	if (!ok) {
		Log("New cook asset: cannot write '" + path.generic_string()
			+ "': " + werr);
		return {};
	}
	// Identity: the .meta sidecar + assetdb row (kind gicook/lmcook, so
	// the rescan keeps the collection kinds — see BakeSidecarKind's
	// importer fallback).
	AdoptProjectAsset(path.generic_string(), kind);
	m_assetsDir = target;
	m_assetsSel = path.generic_string();
	Log(std::string(gi ? "GI" : "Lightmap") + " cook asset created: '"
		+ path.generic_string() + "' (unbaked — bake execution lands "
		"the blob result).");
	return path.generic_string();
}

} // namespace editor
