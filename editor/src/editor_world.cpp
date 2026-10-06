// Live world-scene surgery: reparent / rename / material-flag refresh.
//
// All edits preserve node indices (no add/delete), so the world's drawItems,
// selection, overrides and pick map stay valid without a Resolve pass.

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
		sunTaken ? "" : " â€” no sun, viewport default stands in");
	Log(buf);
	if ((m_cookedPoints + m_cookedSpots) != 0)
		Log("Note: points/spots are logged only â€” the viewport renders the sun (no forward+ path).");
	// The cooked sun seeded the scene default above â€” resolve into the
	// rendered look.
	ResolveSceneEnv();
}

} // namespace editor
