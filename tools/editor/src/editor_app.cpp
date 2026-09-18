// EditorApp: init / frame loop / scene loading / frame-data upload.
// Rendering passes live in editor_render.cpp, UI in editor_ui.cpp.

#include "editor_app.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <Engine/Asset/cluster_serializer.h>
#include <Engine/Asset/pak.h>
#include <Engine/Flintbase/flintbase_backend.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Utils/gltf.h>
#include <Engine/core.h>

#include <imgui.h>

namespace editor {

bool EditorApp::HasPakExtension(const std::string& path) {
	if (path.size() < 4)
		return false;
	const std::string ext = path.substr(path.size() - 4);
	return ext == ".pak" || ext == ".PAK" || ext == ".Pak";
}

namespace {
// Splits "file.pak:N" into path + scene index (drive-letter colons never
// qualify: the suffix must be all digits and the base must end in .pak).
std::pair<std::string, uint32_t> SplitPakIndex(const std::string& path) {
	const size_t colon = path.find_last_of(':');
	if (colon == std::string::npos || colon + 1 >= path.size())
		return { path, 0 };
	for (size_t i = colon + 1; i < path.size(); ++i)
		if (path[i] < '0' || path[i] > '9')
			return { path, 0 };
	const std::string base = path.substr(0, colon);
	if (base.size() < 4)
		return { path, 0 };
	const std::string ext = base.substr(base.size() - 4);
	if (ext != ".pak" && ext != ".PAK")
		return { path, 0 };
	return { base, static_cast<uint32_t>(std::stoul(path.substr(colon + 1))) };
}
} // namespace

bool EditorApp::OpenScene(const std::string& path) {
	// Queued when called before Initialize (CLI path); otherwise loads now.
	// Dispatches cooked .pak vs source .gltf by extension.
	if (!m_context) {
		m_pendingGltf = path;
		return true;
	}
	const auto [pakBase, pakIndex] = SplitPakIndex(path);
	if (HasPakExtension(pakBase))
		return LoadPakIntoWorld(pakBase, pakIndex);
	if (HasPakExtension(path))
		return LoadPakIntoWorld(path);
	return LoadGltfIntoWorld(path);
}

bool EditorApp::OpenPak(const std::string& pakPath) {
	if (!m_context) {
		m_pendingGltf = pakPath;
		return true;
	}
	const auto [pakBase, pakIndex] = SplitPakIndex(pakPath);
	return LoadPakIntoWorld(HasPakExtension(pakBase) ? pakBase : pakPath,
		HasPakExtension(pakBase) ? pakIndex : 0);
}

bool EditorApp::Initialize() {
	if (!Super::Initialize())
		return false;
	// Static mesh heap (VMA virtual blocks + GPU buffers) backing
	// Cluster::UploadStaticMesh. Without this, the first glTF import
	// crashes in vmaVirtualAllocate on a null m_staticVertexBlock.
	smvk::AllocateStaticVertexFmt(*m_context);
	SetRelativeMouseMode(false);

	m_imgui = std::make_unique<smvk::ImGuiOverlay>();
	smvk::ImGuiOverlayDesc imguiDesc{};
	if (!m_imgui->Init(*m_context, *m_swapchain, window, imguiDesc)) {
		std::cerr << "[Editor] ImGui init failed, continuing without UI\n";
		m_imgui.reset();
	}
	else {
		imguiEventHook_ = &smvk::ImGuiOverlay::ProcessEvent;
	}

	// Default orbit pose (overwritten by scene-fit on load).
	m_camPos.setPosition(glm::vec3(4.0f, 3.0f, 6.0f));
	m_camPos.lookAt(glm::vec3(4.0f, 3.0f, 6.0f),
		glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

	if (!m_pendingGltf.empty()) {
		const std::string p = m_pendingGltf;
		m_pendingGltf.clear();
		if (!OpenScene(p))
			std::cerr << "[Editor] failed to load '" << p << "'\n";
	}

	Log("GltfEditor ready. File > Open glTF... to load a scene.");
	return true;
}

void EditorApp::Shutdown() {
	WaitForDeviceIdle();
	m_textureStreamer.reset();
	PollCookProcess();
	PollRunProcess();
	PollPublishProcess();
	m_imgui.reset();
	m_shadowAtlas.Destroy(*m_rendergraph);
	m_depth = smvk::DepthTarget{};
	m_frameUniforms = {};
	m_insetFrameUniforms = {};
	m_materialsBuf = {};
	m_lightsBuf = {};
	m_cascadeBuf = {};
	ClearSglmLayers();
	m_vsModule = {};
	m_unlitFsModule = {};
	m_litFsModule = {};
	m_wireFsModule = {};
	m_bakedFsModule = {};
	m_lineVsModule = {};
	m_lineFsModule = {};
	m_gizmoLines = {};
	m_unlitProgram = {};
	m_litProgram = {};
	m_bakedProgram = {};
	m_wireProgram = {};
	m_shadowProgram = {};
	m_lineProgram = {};
	Super::Shutdown();
}

bool EditorApp::LoadGltfIntoWorld(const std::string& path) {
	// Scheme paths keep their //flintbase/ identity in m_gltfPath but
	// load (and fingerprint) from the resolved storage file.
	std::string loadPath = path;
	if (smvk::vfs::HasFlintbasePrefix(path)) {
		loadPath = smvk::FlintbaseHostPath(path);
		if (loadPath.empty()) {
			Log("Failed to resolve " + std::string(smvk::vfs::Normalize(path)));
			return false;
		}
	}
	tinygltf::Model model;
	if (!LoadModelFromAsciiFile(model, loadPath.c_str())) {
		Log("Failed to load glTF: " + loadPath);
		return false;
	}
	WaitForDeviceIdle();
	// Fresh streamer per load: entries view the old cluster's stashed
	// payload bytes, so drop them before the world (and its clusters)
	// is replaced. GPU is idle, so teardown is safe.
	m_textureStreamer.reset();
	m_texHandleBySlot.clear();
	m_bindlessBySlot.clear();
	if (m_textureStreamingEnabled && m_texMode != TexMode::Full) {
		smvk::TextureStreamerConfig streamCfg;
		streamCfg.budgetBytes = 256ull * 1024 * 1024;
		// Coarsest-only mode pins the floor to the single coarsest mip
		// with no demand and no hysteresis (one Update() decays all).
		streamCfg.minResidentMips = (m_texMode == TexMode::Coarse) ? 1 : 2;
		streamCfg.initialResidentDimCap = 512; // cheapest tail first
		streamCfg.maxUpgradesPerFrame = 4;
		streamCfg.maxDowngradesPerFrame = 8;
		streamCfg.evictCooldownFrames = (m_texMode == TexMode::Coarse) ? 0 : 60;
		streamCfg.mipBias = 0.0f;
		m_textureStreamer =
			std::make_unique<smvk::TextureStreamer>(*m_context, streamCfg);
	}
	m_world = std::make_unique<smvk::World>();
	smvk::Cluster& cluster = m_world->LoadCluster(*m_context);
	Log(std::string("Importing geometry/materials (textures: ") +
		(m_textureStreamer ? (m_texMode == TexMode::Coarse ? "coarsest-only" : "streaming") : "full") +
		")...");
	try {
		GltfToCluster(*m_context, model, loadPath, cluster,
			smvk::MeshOptConfig{}, smvk::MeshletConfig{}, 0, glm::mat4(1.0f),
			m_textureStreamer.get());
	}
	catch (const std::exception& e) {
		Log(std::string("Import failed: ") + e.what());
		return false;
	}
	m_context->FlushUploads();
	Log("Resolving world...");
	m_world->ResolveAndFixup_clusters();
	RebuildTexHandleMap();
	if (m_textureStreamer) {
		const smvk::TextureStreamingStats stats = m_textureStreamer->GetStats();
		char sbuf[256];
		std::snprintf(sbuf, sizeof(sbuf),
			"Textures: %u streamed, resident %.1f MiB / full %.1f MiB (%u full-res)",
			stats.textureCount,
			double(stats.residentBytes) / (1024.0 * 1024.0),
			double(stats.fullBytes) / (1024.0 * 1024.0),
			stats.fullResidentCount);
		Log(sbuf);
	}

	m_gltfPath = path;
	m_gltfHostPath = loadPath;
	m_tourPath = TourPath(); // fresh scene: default target (save-as changes it)
	m_vfsRoot.clear(); // re-resolved lazily by the VFS tab
	m_vfsSel.clear();
	m_gltfModel = std::move(model);
	m_hasGltfModel = true;
	m_pakPath.clear();
	m_pakImage.clear();
	m_pakReader.reset();
	m_pakScenes.clear();
	m_pakSceneIndex = 0;
	m_hasSglm = false;
	m_sglm = smvk::gi::SgLightmapAtlas{};
	m_sglmPath.clear();
	ClearSglmLayers();
	m_sglmStatus = "no atlas loaded (glTF source has no bake)";
	ClearTexPreview();
	m_selected = UINT32_MAX;
	ClearHistory();
	// Cluster base for tour id translation (single-cluster scenes only).
	m_clusterBase = UINT32_MAX;
	if (m_world->clusters.size() == 1 &&
		m_world->scene.hierarchy.size() ==
		1 + m_world->clusters[0]->scene.hierarchy.size())
		m_clusterBase = 1;
	Log("Indexing nodes for picking...");
	RebuildNodeIndex();
	Log("Loading tour...");
	LoadTour(); // replays <stem>.tour into the undo stack when present
	RebuildCookedLights(); // census + seeds the viewport sun from the scene

	// Fit the orbit camera to the scene bounds.
	const ::Aabb bounds = m_world->ComputeSceneBounds();
	if (bounds.mx.x >= bounds.mn.x) {
		const glm::vec3 center = (bounds.mn + bounds.mx) * 0.5f;
		const float radius = glm::length(bounds.mx - bounds.mn) * 0.5f;
		m_camTarget = center;
		m_camDist = std::max(radius * 2.2f, 1.0f);
		m_camYaw = glm::radians(35.0f);
		m_camPitch = glm::radians(-18.0f);
	}
	char buf[512];
	std::snprintf(buf, sizeof(buf), "Loaded '%s' (nodes=%zu meshes=%zu materials=%zu)",
		path.c_str(), m_world->scene.hierarchy.size(),
		m_world->meshLocations.size(), m_world->materialLocations.size());
	Log(buf);
	return true;
}

bool EditorApp::LoadPakIntoWorld(const std::string& path,
	uint32_t sceneIndex) {
	// Cooked .pak: same path as App's SceneRenderer (PakReader +
	// LoadClusterFromPak + Resolve). Pak textures upload fully at load,
	// so the glTF texture streamer is dropped. Picking falls back to
	// AABB (no glTF model kept); tour replay is skipped (tours are
	// authored against the source glTF, not the cooked pak).
	// The image + parsed reader stay members (never re-parsed per frame:
	// Open decompresses).
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) {
		Log("Failed to open pak: " + path);
		return false;
	}
	const auto end = f.tellg();
	if (end == std::ifstream::pos_type(-1) || end == std::streampos(0)) {
		Log("Pak is empty or unreadable: " + path);
		return false;
	}
	f.seekg(0);
	m_pakImage.assign(static_cast<size_t>(end), 0);
	if (!f.read(reinterpret_cast<char*>(m_pakImage.data()),
			static_cast<std::streamsize>(m_pakImage.size()))) {
		Log("Pak read failed: " + path);
		m_pakImage.clear();
		return false;
	}
	m_pakReader.reset();
	m_pakScenes.clear();
	{
		std::string pakErr;
		m_pakReader = smvk::PakReader::Open(
			std::span<const uint8_t>(m_pakImage), pakErr);
		if (!m_pakReader) {
			Log("Pak parse failed (" + pakErr + ")");
			m_pakImage.clear();
			return false;
		}
	}
	const size_t entryCount = m_pakReader->EntryCount();
	{
		std::string lerr;
		m_pakScenes = smvk::ListPakScenes(*m_pakReader, &lerr);
		if (m_pakScenes.empty()) {
			Log("Pak has no loadable scenes" +
				std::string(lerr.empty() ? "" : (": " + lerr)));
			m_pakImage.clear();
			m_pakReader.reset();
			return false;
		}
		if (sceneIndex >= m_pakScenes.size()) {
			Log("Pak scene index " + std::to_string(sceneIndex) +
				" out of range (" + std::to_string(m_pakScenes.size()) +
				" scenes)");
			m_pakImage.clear();
			m_pakReader.reset();
			m_pakScenes.clear();
			return false;
		}
		for (size_t i = 0; i < m_pakScenes.size(); ++i)
			Log("Pak scene[" + std::to_string(i) + "] '" +
				m_pakScenes[i].name + "' nodes=" +
				std::to_string(m_pakScenes[i].nodes));
	}
	m_pakSceneIndex = sceneIndex;
	Log("Pak parsed: " + std::to_string(entryCount) + " entries, loading "
		"scene[" + std::to_string(sceneIndex) + "] '" +
		m_pakScenes[sceneIndex].name + "'...");
	WaitForDeviceIdle();
	m_textureStreamer.reset(); // pak path owns its textures; no streaming
	m_texHandleBySlot.clear();
	m_bindlessBySlot.clear();
	m_world = std::make_unique<smvk::World>();
	try {
		m_world->LoadClusterFromPak(*m_context, *m_pakReader, sceneIndex);
	}
	catch (const std::exception& e) {
		Log(std::string("Pak load failed: ") + e.what());
		m_world.reset();
		return false;
	}
	m_context->FlushUploads();
	Log("Resolving world...");
	m_world->ResolveAndFixup_clusters();
	// Slot maps for the viewport upload remap (pak textures are
	// full-resident; no streamer, but the pool->bindless check still
	// applies and is logged here).
	RebuildTexHandleMap();

	m_pakPath = path;
	// Keep the scene identity pointed at the pak so TourPath()/cook panel
	// stay coherent, but mark it as cooked: no glTF picking, no tour.
	m_gltfPath = path;
	m_gltfHostPath = path;
	m_tourPath = TourPath();
	m_vfsRoot.clear();
	m_vfsSel.clear();
	m_hasGltfModel = false;
	m_gltfModel = tinygltf::Model{};
	m_primRef.clear();
	ClearTexPreview();
	m_selected = UINT32_MAX;
	ClearHistory();
	m_clusterBase = UINT32_MAX;
	if (m_world->clusters.size() == 1 &&
		m_world->scene.hierarchy.size() ==
		1 + m_world->clusters[0]->scene.hierarchy.size())
		m_clusterBase = 1;
	RebuildNodeIndex(); // no-op without a glTF model (clears stale refs)
	ClearSglmLayers(); // dropped before the sibling-atlas probe below

	// Baked lightmap atlas next to the pak: content/gi/<stem>.sglm.
	TryLoadSiblingSglm(path);
	RebuildCookedLights(); // census + seeds the viewport sun from the pak

	const ::Aabb bounds = m_world->ComputeSceneBounds();
	if (bounds.mx.x >= bounds.mn.x) {
		const glm::vec3 center = (bounds.mn + bounds.mx) * 0.5f;
		const float radius = glm::length(bounds.mx - bounds.mn) * 0.5f;
		m_camTarget = center;
		m_camDist = std::max(radius * 2.2f, 1.0f);
		m_camYaw = glm::radians(35.0f);
		m_camPitch = glm::radians(-18.0f);
	}
	char buf[512];
	std::snprintf(buf, sizeof(buf),
		"Loaded pak '%s' scene[%u] (entries=%zu nodes=%zu meshes=%zu materials=%zu)%s",
		path.c_str(), sceneIndex, entryCount,
		m_world->scene.hierarchy.size(),
		m_world->meshLocations.size(), m_world->materialLocations.size(),
		m_hasSglm ? " + sglm atlas" : " (no sglm atlas)");
	Log(buf);
	Log("Note: pak is read-only in the editor (edits/tour apply to the source glTF).");
	return true;
}

bool EditorApp::TryLoadSiblingSglm(const std::string& pakPath) {
	m_hasSglm = false;
	m_sglm = smvk::gi::SgLightmapAtlas{};
	m_sglmPath.clear();
	namespace fs = std::filesystem;
	const fs::path pak(pakPath);
	const std::string stem = pak.stem().string();
	std::vector<std::string> candidates;
	if (!stem.empty()) {
		candidates.push_back(
			(pak.parent_path() / ".." / "gi" / (stem + ".sglm")).lexically_normal().string());
		candidates.push_back(
			(fs::path("content") / "gi" / (stem + ".sglm")).string());
		candidates.push_back(
			(pak.parent_path() / (stem + ".sglm")).string());
	}
	for (const std::string& c : candidates) {
		std::error_code ec;
		if (!fs::is_regular_file(c, ec))
			continue;
		if (LoadSglm(c))
			return true;
	}
	m_sglmStatus = "no sibling atlas found for '" + stem +
		"' (tried " + std::to_string(candidates.size()) + " paths)";
	return false;
}

bool EditorApp::LoadSglm(const std::string& sglmPath) {
	smvk::gi::SgLightmapAtlas atlas;
	std::string err;
	if (!smvk::gi::SgLightmapAtlas::ReadFile(sglmPath, atlas, err)) {
		m_hasSglm = false;
		m_sglmStatus = "sglm load failed: " + err;
		Log("Lightmap atlas load failed '" + sglmPath + "': " + err);
		return false;
	}
	m_sglm = std::move(atlas);
	m_sglmPath = sglmPath;
	m_hasSglm = true;
	std::snprintf(m_sglmBuf, sizeof(m_sglmBuf), "%s", sglmPath.c_str());
	uint32_t l5 = 0, l6 = 0, l9 = 0, l12 = 0;
	for (const auto& c : m_sglm.chunks) {
		if (c.lobeCount == 5) ++l5;
		else if (c.lobeCount == 6) ++l6;
		else if (c.lobeCount == 9) ++l9;
		else ++l12;
	}
	char buf[512];
	std::snprintf(buf, sizeof(buf),
		"Lightmap atlas '%s': %ux%u chunks=%zu (5:%u 6:%u 9:%u 12:%u) gpu=%.1f MiB",
		sglmPath.c_str(), m_sglm.width, m_sglm.height, m_sglm.chunks.size(),
		l5, l6, l9, l12, double(m_sglm.GpuBytes()) / (1024.0 * 1024.0));
	m_sglmStatus = buf;
	Log(m_sglmStatus);
	UploadSglmLayers();
	return true;
}

void EditorApp::ClearSglmLayers() {
	for (auto& l : m_lmLayers)
		l = {};
	for (auto& s : m_lmLayerSlots)
		s = UINT32_MAX;
	m_lmGpu = false;
}

void EditorApp::UploadSglmLayers() {
	// GPU mirror of the CPU atlas for the F8 baked-irradiance view:
	// 9 RGBA16F layers via SgLightmapAtlas::PackGpuLayers (App parity).
	// Without a device (pre-Initialize) the CPU atlas still serves the
	// stats panel; the layers upload on the next LoadSglm instead.
	ClearSglmLayers();
	if (!m_context || !m_hasSglm || m_sglm.chunks.empty() ||
		m_sglm.width == 0 || m_sglm.height == 0)
		return;
	std::vector<uint16_t> layers[smvk::gi::kLightmapLayerCount];
	m_sglm.PackGpuLayers(layers);
	for (uint32_t t = 0; t < smvk::gi::kLightmapLayerCount; ++t) {
		smvk::ImageCreateInfo ci{};
		ci.extent = { m_sglm.width, m_sglm.height, 1 };
		ci.format = smvk::Format::RGBA16_Float;
		ci.usage = smvk::ImageUsage::Sampled | smvk::ImageUsage::TransferDst;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = "editor sglm atlas layer";
		m_lmLayers[t] = m_context->CreateTexture(ci, layers[t].data());
		if (m_lmLayers[t].empty()) {
			Log("Lightmap GPU upload failed at layer " + std::to_string(t));
			ClearSglmLayers();
			return;
		}
		m_lmLayerSlots[t] = m_context->GetTextureBindlessIndex(m_lmLayers[t]);
	}
	m_lmGpu = true;
	char buf[256];
	std::snprintf(buf, sizeof(buf),
		"Lightmap GPU layers: 9x %ux%u RGBA16F (%.1f MiB, F8 = baked view)",
		m_sglm.width, m_sglm.height,
		double(m_sglm.GpuBytes()) / (1024.0 * 1024.0));
	Log(buf);
}

void EditorApp::UpdateTextureStreaming(smvk::Extent2D renderExtent) {
	if (!m_textureStreamingEnabled || !m_textureStreamer || !m_world)
		return;
	const uint32_t streamFrame = static_cast<uint32_t>(m_frameCounter);
	// Handle-based demand (NOT RequestMipsByDistance: that matches caller
	// keys against bindless slots, but MaterialValues::textures stores RHI
	// pool indices — equal only by creation-order coincidence. Resolving
	// the live handle per slot is exact either way). Coarsest-only mode
	// files no demand so Update() decays everything to the 1-mip floor.
	if ((m_frameCounter & 3u) == 0u && m_texMode == TexMode::Streaming &&
		!m_texHandleBySlot.empty()) {
		std::unordered_map<uint32_t, float> minDist;
		minDist.reserve(256);
		const glm::vec3 eye = m_camera.getPosition();
		auto note = [&](int slot, float dist) {
			if (slot < 0)
				return;
			const uint32_t s = static_cast<uint32_t>(slot);
			auto it = minDist.find(s);
			if (it == minDist.end() || dist < it->second)
				minDist[s] = dist;
		};
		m_world->forEachRenderItem(
			smvk::IteratorFlagBit_Renderable,
			[&](const smvk::World::RenderItem& item) {
				if (item.material == smvk::World::kInvalidIndex)
					return;
				const smvk::Material* mat = m_world->GetMaterial(item.material);
				if (!mat)
					return;
				const glm::mat4& model = m_world->scene.globalTransform[item.node];
				const float dist = glm::distance(eye, glm::vec3(model[3]));
				note(mat->values.textures.x, dist);
				note(mat->values.textures.y, dist);
				note(mat->values.textures.z, dist);
				note(mat->values.textures.w, dist);
				note(mat->values.textures2.x, dist);
			});
		// Per-entry chain shape for the mip math (fresh each demand tick;
		// entries only change on retarget, ~15 Hz scan over a few hundred).
		std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> chain;
		chain.reserve(256);
		for (const auto& d : m_textureStreamer->GetEntriesDebug()) {
			const uint64_t key =
				(static_cast<uint64_t>(d.handleGen) << 32) | d.handleIndex;
			const uint32_t baseLong = std::max(
				d.residentWidth << std::min(d.residentBase, 24u),
				d.residentHeight << std::min(d.residentBase, 24u));
			chain[key] = { d.fullMips, std::max(baseLong, 1u) };
		}
		const float fovY = glm::radians(kEditorFovYDeg);
		const float bias = m_textureStreamer->GetConfig().mipBias;
		for (const auto& [slot, dist] : minDist) {
			auto hit = m_texHandleBySlot.find(slot);
			if (hit == m_texHandleBySlot.end() || hit->second.empty())
				continue;
			const uint64_t key = (static_cast<uint64_t>(hit->second.gen()) << 32)
				| hit->second.index();
			auto cit = chain.find(key);
			if (cit == chain.end())
				continue;
			const uint32_t want = smvk::TextureStreamer::WantedMipForDistance(
				cit->second.first, cit->second.second, dist,
				static_cast<float>(renderExtent.height), fovY, 2.0f, bias);
			m_textureStreamer->RequestMip(hit->second, want, streamFrame);
		}
	}
	m_textureStreamer->Update(streamFrame);
	const smvk::TextureStreamingStats stats = m_textureStreamer->GetStats();
	if (stats.upgradesLastUpdate > 0 || stats.downgradesLastUpdate > 0)
		m_context->FlushUploads();
}

void EditorApp::RebuildTexHandleMap() {
	m_texHandleBySlot.clear();
	m_bindlessBySlot.clear();
	if (!m_world || !m_context)
		return;
	uint32_t mismatched = 0;
	for (const auto& clusterPtr : m_world->clusters) {
		if (!clusterPtr)
			continue;
		for (const auto& [uri, h] : clusterPtr->ListTextureUris()) {
			m_texHandleBySlot[h.index()] = h;
			const uint32_t bindless = m_context->GetTextureBindlessIndex(h);
			m_bindlessBySlot[h.index()] = bindless;
			if (bindless != h.index())
				++mismatched;
		}
	}
	if (!m_texHandleBySlot.empty()) {
		char buf[256];
		std::snprintf(buf, sizeof(buf),
			"Texture slots: %zu registered, %u pool!=bindless%s",
			m_texHandleBySlot.size(), mismatched,
			mismatched ? " (remapped at material upload)" : "");
		Log(buf);
	}
}

bool EditorApp::ApplyTexMode() {
	// Pak scenes are full-resident with no streamer: nothing to switch.
	if (!m_hasGltfModel || m_gltfPath.empty() || HasPakExtension(m_gltfPath)) {
		Log("Texture mode: only glTF scenes can switch (pak is full-resident).");
		return false;
	}
	// The streamer must exist at import, so every switch reimports (the
	// tour file is current — every edit saves it — so edits replay back).
	Log("Texture mode switch: reimporting scene...");
	return OpenScene(m_gltfPath);
}

void EditorApp::DumpStreamerStats() {
	if (!m_textureStreamer) {
		Log("Streamer: none (full-resident textures).");
		return;
	}
	const smvk::TextureStreamingStats stats = m_textureStreamer->GetStats();
	char buf[256];
	std::snprintf(buf, sizeof(buf),
		"Streamer: %u textures, resident %.1f MiB / full %.1f MiB, %u full-res (+%u/-%u last)",
		stats.textureCount,
		double(stats.residentBytes) / (1024.0 * 1024.0),
		double(stats.fullBytes) / (1024.0 * 1024.0),
		stats.fullResidentCount,
		stats.upgradesLastUpdate, stats.downgradesLastUpdate);
	Log(buf);
	auto entries = m_textureStreamer->GetEntriesDebug();
	uint32_t shown = 0;
	for (const auto& d : entries) {
		if (++shown > 12)
			break;
		std::snprintf(buf, sizeof(buf),
			"  %-40.40s base=%u want=%u %ux%u",
			d.name.c_str(), d.residentBase, d.desiredBase,
			d.residentWidth, d.residentHeight);
		Log(buf);
	}
	if (entries.size() > 12) {
		std::snprintf(buf, sizeof(buf), "  ... +%zu more",
			entries.size() - 12);
		Log(buf);
	}
}

void EditorApp::RebuildNodeIndex() {
	m_primRef.clear();
	m_clusterRootWorld = UINT32_MAX;
	if (!m_hasGltfModel || !m_world)
		return;
	// The importer creates, per glTF node with a mesh, one prim child node
	// per primitive in order (see importNodeRecursive): group nodes carry the
	// glTF node name, prim children are unnamed identity nodes. Re-walk the
	// glTF node tree in the same preorder to recover (mesh, prim) refs.
	struct Frame { int gltf; uint32_t world; };
	// Map group node name+child-index back: simpler robust approach — walk
	// the WORLD tree: a node with a mesh whose parent has the same mesh
	// count pattern... Instead do a parallel recursion mirroring the
	// importer: world cluster root (node 0) children == glTF roots.
	// The importer attaches glTF roots under the cluster root via
	// importNodeRecursive(gltf, parent=clusterRoot?) — actually under the
	// world-resolved tree the cluster root child order matches glTF root
	// order. Prim children of each group node match primitive order.
	// Walk: for each world node that has a mesh, its parent group maps to a
	// glTF node; we resolve by matching names in preorder.
	std::vector<int> gltfPreorder;
	std::function<void(int)> walk = [&](int n) {
		gltfPreorder.push_back(n);
		for (int c : m_gltfModel.nodes[n].children)
			walk(c);
	};
	// glTF roots: default scene or unreferenced nodes (mirror GltfToCluster).
	std::vector<int> roots;
	if (m_gltfModel.defaultScene >= 0 &&
		m_gltfModel.defaultScene < (int)m_gltfModel.scenes.size())
		roots = m_gltfModel.scenes[m_gltfModel.defaultScene].nodes;
	else if (!m_gltfModel.scenes.empty())
		roots = m_gltfModel.scenes.front().nodes;
	else {
		std::vector<bool> ref(m_gltfModel.nodes.size(), false);
		for (const auto& n : m_gltfModel.nodes)
			for (int c : n.children)
				if (c >= 0 && c < (int)m_gltfModel.nodes.size())
					ref[c] = true;
		for (int i = 0; i < (int)m_gltfModel.nodes.size(); ++i)
			if (!ref[i])
				roots.push_back(i);
	}
	for (int r : roots)
		walk(r);

	// World group nodes in preorder (skip world root 0 + cluster root).
	std::vector<uint32_t> worldGroups;
	std::function<void(uint32_t)> wwalk = [&](uint32_t n) {
		worldGroups.push_back(n);
		for (int c = m_world->scene.hierarchy[n].firstChild; c != -1;
			c = m_world->scene.hierarchy[c].nextSibling)
			wwalk((uint32_t)c);
	};
	// Find the cluster-root child of world root (first child).
	if (!m_world->scene.hierarchy.empty()) {
		for (int c = m_world->scene.hierarchy[0].firstChild; c != -1;
			c = m_world->scene.hierarchy[c].nextSibling) {
			if (m_clusterRootWorld == UINT32_MAX)
				m_clusterRootWorld = uint32_t(c);
			wwalk((uint32_t)c);
		}
	}
	// Pair group nodes with glTF nodes by name in preorder (names may repeat;
	// order is what the importer guarantees).
	size_t gi = 0;
	for (uint32_t wn : worldGroups) {
		if (gi >= gltfPreorder.size())
			break;
		// Prim nodes carry meshes (skipped here — paired below via their
		// group). The artificial cluster root is unnamed — skip it too so
		// group nodes stay 1:1 with glTF nodes in preorder.
		if (m_world->scene.meshForNode.contains(wn))
			continue;
		if (!m_world->scene.nameForNode.contains(wn))
			continue;
		int gn = gltfPreorder[gi++];
		const tinygltf::Node& gnode = m_gltfModel.nodes[gn];
		if (gnode.mesh < 0)
			continue;
		// Prim children of this group, in sibling order, map to primitives.
		int primIdx = 0;
		for (int c = m_world->scene.hierarchy[wn].firstChild; c != -1;
			c = m_world->scene.hierarchy[c].nextSibling) {
			if (!m_world->scene.meshForNode.contains((uint32_t)c))
				continue;
			m_primRef[(uint32_t)c] = PrimRef{ gnode.mesh, primIdx++ };
		}
	}
}

void EditorApp::UpdateCamera(float dt) {
	(void)dt;
	// Orbit-only camera (no WASD/QE fly: held movement keys leaked into
	// text fields and fought the editor). Navigate with Alt+LMB/MMB/RMB,
	// the wheel, and F / Shift+F framing.
	const glm::vec3 up(0, 1, 0);
	// Orbit drag: Maya-style (Alt+LMB tumble, Alt+MMB track,
	// Alt+RMB dolly) plus the legacy RMB-drag orbit when Alt is up.
	const bool imguiEats = m_imgui && m_imgui->WantsCaptureMouse();
	const bool alt = m_input.isKeyHeld(GLFW_KEY_LEFT_ALT) ||
		m_input.isKeyHeld(GLFW_KEY_RIGHT_ALT);
	if (!imguiEats && !m_gizmoDragging) {
		if (alt) {
			if (m_input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_LEFT)) {
				m_camYaw -= float(m_input.getMouseDeltaX()) * 0.005f;
				m_camPitch -= float(m_input.getMouseDeltaY()) * 0.005f;
				m_camPitch = std::clamp(m_camPitch, -1.55f, 1.55f);
			}
			if (m_input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_MIDDLE)) {
				const glm::vec3 fwd = m_camPos.getForward();
				glm::vec3 right = glm::cross(fwd, glm::vec3(0, 1, 0));
				if (glm::length(right) < 1e-6f)
					right = glm::vec3(1, 0, 0);
				right = glm::normalize(right);
				const glm::vec3 camUp = glm::normalize(glm::cross(right, fwd));
				const float k = m_camDist * 0.0016f;
				m_camTarget -= right * float(m_input.getMouseDeltaX()) * k;
				m_camTarget += camUp * float(m_input.getMouseDeltaY()) * k;
			}
			if (m_input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_RIGHT)) {
				const float dy = float(m_input.getMouseDeltaY());
				m_camDist = std::clamp(m_camDist * (1.0f + dy * 0.005f),
					0.2f, 500.0f);
			}
		}
		else {
			const bool orbitBtn = m_input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_RIGHT) ||
				(m_input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_LEFT) && m_selected == UINT32_MAX);
			if (orbitBtn) {
				m_camYaw -= float(m_input.getMouseDeltaX()) * 0.005f;
				m_camPitch -= float(m_input.getMouseDeltaY()) * 0.005f;
				m_camPitch = std::clamp(m_camPitch, -1.55f, 1.55f);
			}
		}
	}
	// Wheel zoom.
	const float wheel = float(m_input.getScrollDeltaY());
	if (wheel != 0.0f && !(m_imgui && m_imgui->WantsCaptureMouse()))
		m_camDist = std::clamp(m_camDist * (wheel > 0 ? 0.9f : 1.1f), 0.2f, 500.0f);

	const glm::vec3 eye =
		m_camTarget + m_camDist * glm::vec3(
			std::cos(m_camPitch) * std::sin(m_camYaw),
			-std::sin(m_camPitch),
			std::cos(m_camPitch) * std::cos(m_camYaw));
	m_camPos.setPosition(eye);
	m_camPos.lookAt(eye, m_camTarget, up);
}

bool EditorApp::SelectionBounds(glm::vec3& outCenter, float& outRadius) {
	if (!m_world || m_selected == UINT32_MAX ||
		m_selected >= m_world->scene.hierarchy.size())
		return false;
	// Union AABB over the selected subtree's mesh nodes (same union the
	// gizmo highlight draws in editor_gizmo.cpp).
	glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
	bool has = false;
	std::vector<uint32_t> stack{ m_selected };
	while (!stack.empty()) {
		const uint32_t n = stack.back();
		stack.pop_back();
		if (n >= m_world->scene.hierarchy.size())
			continue;
		for (int c = m_world->scene.hierarchy[n].firstChild; c != -1;
			c = m_world->scene.hierarchy[c].nextSibling)
			stack.push_back(uint32_t(c));
		auto mit = m_world->scene.meshForNode.find(n);
		if (mit == m_world->scene.meshForNode.end())
			continue;
		smvk::StaticMesh* mesh = m_world->GetMesh(mit->second);
		if (!mesh)
			continue;
		const glm::mat4& model = m_world->scene.globalTransform[n];
		for (int k = 0; k < 8; ++k) {
			const glm::vec3 corner(
				(k & 1) ? mesh->geometry.aabbMax.x : mesh->geometry.aabbMin.x,
				(k & 2) ? mesh->geometry.aabbMax.y : mesh->geometry.aabbMin.y,
				(k & 4) ? mesh->geometry.aabbMax.z : mesh->geometry.aabbMin.z);
			const glm::vec3 w = glm::vec3(model * glm::vec4(corner, 1.0f));
			mn = glm::min(mn, w);
			mx = glm::max(mx, w);
			has = true;
		}
	}
	if (!has) {
		// No mesh under the selection: pivot on the node origin.
		outCenter = glm::vec3(m_world->scene.globalTransform[m_selected][3]);
		outRadius = 0.5f;
		return true;
	}
	outCenter = (mn + mx) * 0.5f;
	outRadius = glm::length(mx - mn) * 0.5f;
	return true;
}

void EditorApp::FrameSelection() {
	glm::vec3 center{ 0.0f };
	float radius = 0.0f;
	if (SelectionBounds(center, radius)) {
		m_camTarget = center;
		m_camDist = std::max(radius * 2.2f, 1.0f);
	}
	else {
		FrameAll();
	}
}

void EditorApp::FrameAll() {
	if (!m_world)
		return;
	const ::Aabb bounds = m_world->ComputeSceneBounds();
	if (bounds.mx.x < bounds.mn.x)
		return;
	m_camTarget = (bounds.mn + bounds.mx) * 0.5f;
	m_camDist = std::max(glm::length(bounds.mx - bounds.mn) * 0.5f * 2.2f, 1.0f);
}

void EditorApp::DrawFrame() {
	UpdateDeltaTime();
	Update();
	auto fb = m_swapchain->AcquireFramebuffer();
	if (fb.imageIndex >= 0)
		RenderFrame(fb);
	m_swapchain->EndAndPresentFramebuffer(fb);
	PollCookProcess();
	PollRunProcess();
	PollPublishProcess();
}

void EditorApp::Log(const std::string& s) {
	m_consoleLines.push_back(s);
	if (m_consoleLines.size() > 400)
		m_consoleLines.erase(m_consoleLines.begin());
	std::cerr << "[Editor] " << s << "\n";
}

} // namespace editor
