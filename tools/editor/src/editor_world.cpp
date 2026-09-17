// Live world-scene surgery: reparent / rename / material-flag refresh.
//
// All edits preserve node indices (no add/delete), so the world's drawItems,
// selection, overrides and pick map stay valid without a Resolve pass.

#include "editor_app.h"

#include <algorithm>

#include <Engine/Asset/tour.h>

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

} // namespace editor
