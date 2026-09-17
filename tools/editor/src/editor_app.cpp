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
#include <Engine/Renderer/mesh.h>
#include <Engine/Utils/gltf.h>
#include <Engine/core.h>

#include <imgui.h>

namespace editor {

bool EditorApp::OpenScene(const std::string& gltfPath) {
	// Queued when called before Initialize (CLI path); otherwise loads now.
	if (!m_context) {
		m_pendingGltf = gltfPath;
		return true;
	}
	return LoadGltfIntoWorld(gltfPath);
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
		if (!LoadGltfIntoWorld(p))
			std::cerr << "[Editor] failed to load '" << p << "'\n";
	}

	Log("GltfEditor ready. File > Open glTF... to load a scene.");
	return true;
}

void EditorApp::Shutdown() {
	WaitForDeviceIdle();
	m_textureStreamer.reset();
	PollCookProcess(); // reap a finished cook child if any
	m_imgui.reset();
	m_shadowAtlas.Destroy(*m_rendergraph);
	m_depth = smvk::DepthTarget{};
	m_frameUniforms = {};
	m_materialsBuf = {};
	m_lightsBuf = {};
	m_cascadeBuf = {};
	m_vsModule = {};
	m_unlitFsModule = {};
	m_litFsModule = {};
	m_wireFsModule = {};
	m_lineVsModule = {};
	m_lineFsModule = {};
	m_gizmoLines = {};
	m_unlitProgram = {};
	m_litProgram = {};
	m_wireProgram = {};
	m_shadowProgram = {};
	m_lineProgram = {};
	Super::Shutdown();
}

bool EditorApp::LoadGltfIntoWorld(const std::string& path) {
	tinygltf::Model model;
	if (!LoadModelFromAsciiFile(model, path.c_str())) {
		Log("Failed to load glTF: " + path);
		return false;
	}
	WaitForDeviceIdle();
	// Fresh streamer per load: entries view the old cluster's stashed
	// payload bytes, so drop them before the world (and its clusters)
	// is replaced. GPU is idle, so teardown is safe.
	m_textureStreamer.reset();
	if (m_textureStreamingEnabled) {
		smvk::TextureStreamerConfig streamCfg;
		streamCfg.budgetBytes = 256ull * 1024 * 1024;
		streamCfg.minResidentMips = 2;
		streamCfg.initialResidentDimCap = 512; // cheapest tail first
		streamCfg.maxUpgradesPerFrame = 4;
		streamCfg.maxDowngradesPerFrame = 8;
		streamCfg.evictCooldownFrames = 60;
		streamCfg.mipBias = 0.0f;
		m_textureStreamer =
			std::make_unique<smvk::TextureStreamer>(*m_context, streamCfg);
	}
	m_world = std::make_unique<smvk::World>();
	smvk::Cluster& cluster = m_world->LoadCluster(*m_context);
	Log("Importing geometry/materials...");
	try {
		GltfToCluster(*m_context, model, path, cluster,
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

	m_gltfPath = path;
	m_gltfModel = std::move(model);
	m_hasGltfModel = true;
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

void EditorApp::UpdateTextureStreaming(smvk::Extent2D renderExtent) {
	if (!m_textureStreamingEnabled || !m_textureStreamer || !m_world)
		return;
	const uint32_t streamFrame = static_cast<uint32_t>(m_frameCounter);
	if ((m_frameCounter & 3u) == 0u) {
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
		const float fovY = glm::radians(kEditorFovYDeg);
		m_textureStreamer->RequestMipsByDistance(minDist,
			static_cast<float>(renderExtent.height), fovY, streamFrame);
	}
	m_textureStreamer->Update(streamFrame);
	const smvk::TextureStreamingStats stats = m_textureStreamer->GetStats();
	if (stats.upgradesLastUpdate > 0 || stats.downgradesLastUpdate > 0)
		m_context->FlushUploads();
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
	const float moveSpeed = m_camDist * 1.2f;
	const glm::vec3 fwd = m_camPos.getForward();
	const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0, 1, 0)));
	const glm::vec3 up(0, 1, 0);
	glm::vec3 pos = m_camPos.getPosition();
	bool moved = false;
	if (m_input.isKeyHeld(GLFW_KEY_W)) { pos += fwd * moveSpeed * dt; moved = true; }
	if (m_input.isKeyHeld(GLFW_KEY_S)) { pos -= fwd * moveSpeed * dt; moved = true; }
	if (m_input.isKeyHeld(GLFW_KEY_A)) { pos -= right * moveSpeed * dt; moved = true; }
	if (m_input.isKeyHeld(GLFW_KEY_D)) { pos += right * moveSpeed * dt; moved = true; }
	if (m_input.isKeyHeld(GLFW_KEY_Q)) { pos -= up * moveSpeed * dt; moved = true; }
	if (m_input.isKeyHeld(GLFW_KEY_E)) { pos += up * moveSpeed * dt; moved = true; }
	if (moved) {
		m_camTarget = pos + fwd * m_camDist;
		m_camPos.setPosition(pos);
	}
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
	if (!moved) {
		m_camPos.setPosition(eye);
		m_camPos.lookAt(eye, m_camTarget, up);
	}
	else {
		m_camPos.lookAt(pos, pos + fwd, up);
	}
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
}

void EditorApp::Log(const std::string& s) {
	m_consoleLines.push_back(s);
	if (m_consoleLines.size() > 400)
		m_consoleLines.erase(m_consoleLines.begin());
	std::cerr << "[Editor] " << s << "\n";
}

} // namespace editor
