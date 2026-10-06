// Live world-scene surgery: reparent / rename / material-flag refresh.
//
// All edits preserve node indices (no add/delete), so the world's drawItems,
// selection, overrides and pick map stay valid without a Resolve pass.
//
// The one exception: the CUBEMAP-BALL VISUALS (see
// RebuildCubemapBallVisuals below) — chrome spheres parented under the
// capture nodes so the authored pins read as balls carrying the scene's
// baked cubemap (the shared lit chrome material samples the IBL like the
// check-scene chrome ball). Physical nodes (they draw through the normal
// drawItems path), but EDITOR-VISUAL: filtered from the outliner/picks,
// never in the tour (no commands reference them).

#include "editor_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <Engine/Asset/tour.h>
#include <gltf_import/gltf.h>

namespace editor {

void EditorApp::RenameNode(uint32_t node, const std::string& name) {
	if (!m_world || node >= m_world->scene.hierarchy.size())
		return;
	const uint32_t id = uint32_t(m_world->scene.nodeNames.size());
	m_world->scene.nodeNames.push_back(name);
	m_world->scene.nameForNode[node] = id;
}

void EditorApp::ReparentNode(uint32_t node, uint32_t newParent) {
	if (!m_world)
		return;
	Scene& scene = m_world->scene;
	if (node >= scene.hierarchy.size() || newParent >= scene.hierarchy.size())
		return;
	const int before = scene.hierarchy[node].parent;
	smvk::tour::TourReparent(scene, node, newParent);
	if (scene.hierarchy[node].parent != before) {
		markAsChanged(scene, int(node));
		recalculateGlobalTransforms(scene);
	}
}

// Mirrors World::ResolveAndFixup_clusters flag rules for one node.
void EditorApp::RefreshNodeDrawState(uint32_t node) {
	if (!m_world || node >= m_world->scene.hierarchy.size())
		return;
	if (m_deletedNodes.contains(node))
		return; // deleted: links stay dropped (no wide-override resurrect)
	Scene& scene = m_world->scene;
	if (!scene.meshForNode.contains(node))
		return;
	smvk::IteratorFlags f = smvk::IteratorFlags(
		smvk::IteratorFlagBit_Renderable | smvk::IteratorFlagBit_StaticVertexFmt);
	auto matIt = scene.materialForNode.find(node);
	uint32_t matIdx = smvk::World::kInvalidIndex;
	if (matIt != scene.materialForNode.end() &&
		matIt->second < m_world->materialLocations.size()) {
		matIdx = matIt->second;
		const auto& loc = m_world->materialLocations[matIdx];
		if (loc.cluster && loc.handle.valid()) {
			if (auto* mat = loc.cluster->materials.get(loc.handle)) {
				if (mat->isTransparent()) {
					f |= smvk::IteratorFlags(smvk::IteratorFlagBit_Transparent);
				}
				else {
					f |= smvk::IteratorFlags(smvk::IteratorFlagBit_Opaque);
					if (mat->castsShadows)
						f |= smvk::IteratorFlags(smvk::IteratorFlagBit_CastShadow);
					if (mat->shadingModel == smvk::ShadingModel::Masked)
						f |= smvk::IteratorFlags(smvk::IteratorFlagBit_Masked);
				}
			}
			else {
				f |= smvk::IteratorFlags(smvk::IteratorFlagBit_Opaque |
					smvk::IteratorFlagBit_CastShadow);
			}
		}
		else {
			f |= smvk::IteratorFlags(smvk::IteratorFlagBit_Opaque |
				smvk::IteratorFlagBit_CastShadow);
		}
	}
	else {
		f |= smvk::IteratorFlags(smvk::IteratorFlagBit_Opaque |
			smvk::IteratorFlagBit_CastShadow);
	}
	if (node < m_world->flags.size())
		m_world->flags[node] = f;
	for (size_t i = 0; i < m_world->drawItems.size(); ++i) {
		if (m_world->drawItems[i].node == node) {
			m_world->drawItems[i].flags = f;
			m_world->drawItems[i].material = matIdx;
			if (i < m_world->itemFlags.size())
				m_world->itemFlags[i] = f;
		}
	}
	m_world->flagLists.clear();
}

void EditorApp::RefreshMaterialNodes(uint32_t mat) {
	if (!m_world)
		return;
	for (const auto& item : m_world->drawItems)
		if (item.material == mat)
			RefreshNodeDrawState(item.node);
}

// Definition + world aim for a light node (null when the node has none).
// Aim resolves through the node's global transform with the glTF -Z
// axis (see GltfPunctualAimAxis): raw locals (the coordFix basis rides
// the imported chain's top node), so the plain -Z read is exact in
// every mode.
const ::SceneLight* EditorApp::FindNodeLight(uint32_t node,
	glm::vec3* outAimWs) const {
	if (outAimWs)
		*outAimWs = glm::vec3(0.0f);
	if (!m_world || node >= m_world->scene.hierarchy.size())
		return nullptr;
	const Scene& scene = m_world->scene;
	auto lit = scene.lightForNode.find(node);
	if (lit == scene.lightForNode.end() ||
		lit->second >= m_world->lightLocations.size())
		return nullptr;
	const auto& loc = m_world->lightLocations[lit->second];
	if (!loc.cluster || !loc.handle.valid())
		return nullptr;
	const ::SceneLight* l = loc.cluster->lights.get(loc.handle);
	if (!l)
		return nullptr;
	if (outAimWs &&
		(l->type == ::SceneLight::Type::Directional ||
			l->type == ::SceneLight::Type::Spot) &&
		node < scene.globalTransform.size()) {
		const glm::vec3 aimAxis = GltfPunctualAimAxis(0);
		const glm::vec3 emis = glm::vec3(
			scene.globalTransform[node] * glm::vec4(aimAxis, 0.0f));
		if (glm::length(emis) > 1e-9f)
			*outAimWs = glm::normalize(emis);
	}
	return l;
}

// Definition for a camera node (null when the node has none).
// Resolves through world cameraLocations -> cluster cameras pool.
const ::SceneCamera* EditorApp::FindNodeCamera(uint32_t node) const {
	if (!m_world || node >= m_world->scene.hierarchy.size())
		return nullptr;
	const Scene& scene = m_world->scene;
	auto cit = scene.cameraForNode.find(node);
	if (cit == scene.cameraForNode.end() ||
		cit->second >= m_world->cameraLocations.size())
		return nullptr;
	const auto& loc = m_world->cameraLocations[cit->second];
	if (!loc.cluster || !loc.handle.valid())
		return nullptr;
	return loc.cluster->cameras.get(loc.handle);
}

uint32_t EditorApp::FindCameraNode() const {
	// First camera node in hierarchy order (outliner order parity).
	if (!m_world)
		return UINT32_MAX;
	uint32_t best = UINT32_MAX;
	for (const auto& [node, ci] : m_world->scene.cameraForNode) {
		(void)ci;
		if (node < m_world->scene.hierarchy.size()
			&& (best == UINT32_MAX || node < best))
			best = node;
	}
	return best;
}

// ---- cubemap-ball visuals (editor-only chrome spheres) -------------------
// One shared unit-sphere upload + one shared chrome material per world
// (pool reuse by URI — rebuilds never regrow the pools); one child node
// per ALIVE capture (local = the ball's scale at the capture origin, so
// gizmo moves track for free). The balls draw through the normal lit
// path — a white-metal sphere lit by the scene IBL reads the baked
// cubemap at the capture spot (the check-scene chrome-ball precedent).
void EditorApp::RebuildCubemapBallVisuals() {
	// Desired signature (the flag + the alive capture set + their
	// radii): rebuild ONLY on mismatch or when forced dirty. Moves,
	// renames and unrelated edits then no-op instead of churning node
	// ids every command (the dirty flag rides Do/PushDone/Undo/Redo).
	uint64_t sig = m_showCubemapBalls ? 0x9e3779b97f4a7c15ull : 0;
	if (m_showCubemapBalls && m_world) {
		std::vector<uint32_t> caps;
		for (const auto& [n, def] : m_world->scene.cubemapForNode) {
			(void)def;
			if (n < m_world->scene.hierarchy.size()
				&& !m_deletedNodes.contains(n))
				caps.push_back(n);
		}
		std::sort(caps.begin(), caps.end());
		for (uint32_t cap : caps) {
			sig = sig * 1315423911u + uint64_t(cap) * 2u + 1u;
			const float r = m_world->scene.cubemapForNode.at(cap).radius;
			uint32_t rb = 0;
			std::memcpy(&rb, &r, 4);
			sig = sig * 1315423911u + uint64_t(rb);
		}
	}
	if (!m_cubemapBallsDirty && sig == m_cubemapBallsSignature)
		return; // capture set (+ radii) unchanged: nothing to do
	m_cubemapBallsDirty = false;
	m_cubemapBallsSignature = sig;
	// Tear down the previous set PHYSICALLY (they carry no undo state
	// and no history references them — ids are free to churn).
	if (m_world) {
		for (uint32_t n : m_cubemapBallNodes) {
			for (size_t i = m_world->drawItems.size(); i-- > 0;) {
				if (m_world->drawItems[i].node == n) {
					m_world->drawItems.erase(
						m_world->drawItems.begin() + ptrdiff_t(i));
					if (i < m_world->itemFlags.size())
						m_world->itemFlags.erase(
							m_world->itemFlags.begin() + ptrdiff_t(i));
					break;
				}
			}
			m_world->scene.meshForNode.erase(n);
			m_world->scene.materialForNode.erase(n);
		}
		if (!m_world->drawItems.empty())
			m_world->flagLists.clear();
	}
	m_cubemapBallNodes.clear();
	if (!m_showCubemapBalls || !m_world || m_world->clusters.empty()
		|| !m_world->clusters[0] || !m_context)
		return;
	Scene& scene = m_world->scene;
	smvk::Cluster* cl = m_world->clusters[0].get();
	// Alive captures, deterministic order.
	std::vector<uint32_t> caps;
	for (const auto& [n, def] : scene.cubemapForNode) {
		(void)def;
		if (n < scene.hierarchy.size() && !m_deletedNodes.contains(n))
			caps.push_back(n);
	}
	if (caps.empty())
		return;
	std::sort(caps.begin(), caps.end());
	// Shared chrome material (pool reuse by URI: "editor://cubeball" —
	// metallic 1, low roughness; castsShadows off so the helper balls
	// never shadow the scene they visualize).
	uint32_t matWorld = UINT32_MAX;
	{
		if (auto hit = cl->FindMaterialByUri("editor://cubeball")) {
			for (uint32_t i = 0;
				i < uint32_t(m_world->materialLocations.size()); ++i) {
				const auto& loc = m_world->materialLocations[i];
				if (loc.cluster == cl
					&& loc.handle.index() == hit->index()) {
					matWorld = i;
					break;
				}
			}
		}
		if (matWorld == UINT32_MAX) {
			smvk::Material def{};
			def.values.diffuse = glm::vec4(1.0f);
			def.values.params.y = 1.0f;  // metallic
			def.values.params.z = 0.13f; // roughness (chrome-ish)
			def.castsShadows = false;    // helper balls never shadow
			def.rebuildPipeline();
			const smvk::ClusterMaterialHandle h =
				cl->materials.create(std::move(def));
			if (h.empty())
				return;
			cl->RegisterMaterialUri("editor://cubeball", h);
			matWorld = uint32_t(m_world->materialLocations.size());
			m_world->materialLocations.push_back({ cl, h });
			RebuildTexHandleMap();
		}
	}
	// Shared unit-sphere upload (pool reuse by mesh URI).
	uint32_t ballMesh = UINT32_MAX;
	{
		if (auto hit = cl->FindMeshByUri("editor://cubeball")) {
			for (uint32_t i = 0;
				i < uint32_t(m_world->meshLocations.size()); ++i) {
				const auto& loc = m_world->meshLocations[i];
				if (loc.cluster == cl
					&& loc.handle.index() == hit->index()) {
					ballMesh = i;
					break;
				}
			}
		}
		if (ballMesh == UINT32_MAX) {
			smvk::StaticMeshUpload up{};
			up.aabbMin = glm::vec3(-1.0f);
			up.aabbMax = glm::vec3(1.0f);
			// Unit UV sphere (24x16 — the check-scene chrome-ball
			// pattern, trimmed; static-upload vertex layout).
			const int slices = 24, stacks = 16;
			const float pi = 3.14159265f;
			for (int j = 0; j <= stacks; ++j) {
				const float phi = pi * float(j) / float(stacks);
				const float sinPhi = std::sin(phi);
				const float cosPhi = std::cos(phi);
				for (int i = 0; i <= slices; ++i) {
					const float theta = 2.0f * pi * float(i)
						/ float(slices);
					const glm::vec3 n(sinPhi * std::cos(theta), cosPhi,
						sinPhi * std::sin(theta));
					smvk::StaticVertex v{};
					v.pos = n;
					v.color = glm::vec4(1.0f);
					v.texCoord = glm::vec2(float(i) / float(slices),
						float(j) / float(stacks));
					v.normal = n;
					glm::vec3 t(-std::sin(theta), 0.0f, std::cos(theta));
					if (glm::dot(t, t) < 1e-12f)
						t = glm::vec3(1.0f, 0.0f, 0.0f);
					v.tangent = glm::vec4(glm::normalize(t), 1.0f);
					up.vertices.push_back(v);
				}
			}
			const uint32_t row = uint32_t(slices) + 1u;
			for (int j = 0; j < stacks; ++j) {
				for (int i = 0; i < slices; ++i) {
					const uint32_t a = uint32_t(j) * row + uint32_t(i);
					const uint32_t b = a + 1u;
					const uint32_t c = a + row + 1u;
					const uint32_t d = a + row;
					up.indices.insert(up.indices.end(),
						{ a, b, c, a, c, d });
				}
			}
			const smvk::ClusterStaticMeshHandle mh =
				cl->UploadStaticMesh(up);
			if (!mh.valid())
				return;
			cl->RegisterMeshUri("editor://cubeball", mh);
			ballMesh = uint32_t(m_world->meshLocations.size());
			m_world->meshLocations.push_back({ cl, mh });
		}
	}
	for (uint32_t cap : caps) {
		const auto defIt = scene.cubemapForNode.find(cap);
		if (defIt == scene.cubemapForNode.end())
			continue;
		const int level = scene.hierarchy[cap].level + 1;
		const int created = addNode(scene, int(cap), level);
		if (created < 0)
			continue;
		// Ball radius reads the influence at a glance (proportional,
		// clamped — a 10 m capture must not fill the frame).
		const float r = std::clamp(defIt->second.radius * 0.15f,
			0.1f, 0.4f);
		scene.localTransform[size_t(created)] =
			glm::scale(glm::mat4(1.0f), glm::vec3(r));
		const uint32_t nameId = uint32_t(scene.nodeNames.size());
		scene.nodeNames.push_back("CaptureBall");
		scene.nameForNode[uint32_t(created)] = nameId;
		scene.meshForNode[uint32_t(created)] = ballMesh;
		scene.materialForNode[uint32_t(created)] = matWorld;
		if (m_world->flags.size() <= size_t(created))
			m_world->flags.resize(size_t(created) + 1, 0);
		m_world->drawItems.push_back(smvk::World::RenderItem{
			uint32_t(created), ballMesh, matWorld,
			m_world->flags[size_t(created)] });
		m_world->itemFlags.push_back(m_world->flags[size_t(created)]);
		RefreshNodeDrawState(uint32_t(created));
		m_cubemapBallNodes.insert(uint32_t(created));
	}
	m_world->flagLists.clear();
	markAsChanged(scene, 0);
	recalculateGlobalTransforms(scene);
}

// Cooked-light census + viewport-sun seeding. First directional wins as
// the sun (App parity); its sliders are seeded from the decoded aim so a
// pak sun lights the viewport exactly as baked. Intensity is lux-scale in
// the asset but the editor viewport is LDR, so the slider takes the
// clamped value while the log keeps the physical one.
void EditorApp::RebuildCookedLights() {
	m_hasCookedSun = false;
	m_cookedSunName.clear();
	m_cookedDirs = m_cookedPoints = m_cookedSpots = 0;
	m_cookedStaticSkipped = 0;
	if (!m_world)
		return;
	const Scene& scene = m_world->scene;
	if (scene.lightForNode.empty()) {
		Log("Cooked lights: none (viewport sun default).");
		return;
	}
	std::vector<uint32_t> nodes;
	nodes.reserve(scene.lightForNode.size());
	for (const auto& [n, li] : scene.lightForNode)
		nodes.push_back(n);
	std::sort(nodes.begin(), nodes.end());
	bool sunTaken = false;
	for (uint32_t node : nodes) {
		glm::vec3 aimWs(0.0f);
		const ::SceneLight* l = FindNodeLight(node, &aimWs);
		if (!l)
			continue;
		if (l->type == ::SceneLight::Type::Directional) {
			++m_cookedDirs;
			if (sunTaken || glm::length(aimWs) < 1e-9f)
				continue;
			const glm::vec3 toSun = -aimWs;
			m_cookedSunName = l->name.empty()
				? ("node " + std::to_string(node)) : l->name;
			SceneEnv& env = m_world->scene.defaultEnvironment; // seed the scene default
			env.sunAzimuth = glm::degrees(std::atan2(toSun.x, toSun.z));
			if (env.sunAzimuth < 0.0f)
				env.sunAzimuth += 360.0f;
			env.sunElevation = glm::degrees(
				std::asin(glm::clamp(toSun.y, -1.0f, 1.0f)));
			env.sunColor = l->color;
			env.sunIntensity = l->intensity; // lux-native env: straight in
			m_hasCookedSun = true;
			sunTaken = true;
			char buf[384];
			std::snprintf(buf, sizeof(buf),
				"Cooked sun '%s' (node %u): toSun=(%.3f,%.3f,%.3f) az=%.1f el=%.1f "
				"color=(%.2f,%.2f,%.2f) I=%.1f lux (env block, lux-native)",
				m_cookedSunName.c_str(), node,
				toSun.x, toSun.y, toSun.z, env.sunAzimuth, env.sunElevation,
				l->color.x, l->color.y, l->color.z, env.sunIntensity);
			Log(buf);
		}
		else if (l->type == ::SceneLight::Type::Point) {
			++m_cookedPoints;
			if ((l->flags & ::SceneLight::kSceneLightFlag_Static) != 0)
				++m_cookedStaticSkipped;
		}
		else {
			++m_cookedSpots;
			if ((l->flags & ::SceneLight::kSceneLightFlag_Static) != 0)
				++m_cookedStaticSkipped;
		}
	}
	char buf[256];
	std::snprintf(buf, sizeof(buf),
		"Cooked lights: %u dir, %u point, %u spot (%u static/bake-only)%s",
		m_cookedDirs, m_cookedPoints, m_cookedSpots, m_cookedStaticSkipped,
		sunTaken ? "" : " — no sun, viewport default stands in");
	Log(buf);
	if ((m_cookedPoints + m_cookedSpots) != 0)
		Log("Note: points/spots are logged only — the viewport renders the sun (no forward+ path).");
	// The cooked sun seeded the scene default above — resolve into the
	// rendered look.
	ResolveSceneEnv();
}

} // namespace editor
