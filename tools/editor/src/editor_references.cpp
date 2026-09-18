// Editor-side nested scene references (Clarisse-style): the master
// scene's reference list is scene DEFINITION — bound at import by
// gltf_import's ApplySceneReferences (placeholder group node + the
// referenced source's subtree, recursing through each source's own
// .tour). Adding/removing a reference persists the list and rebinds by
// reloading the scene (the import is the deterministic id contract).

#include "editor_app.h"

#include <algorithm>
#include <filesystem>

#include <imgui.h>

namespace editor {

bool EditorApp::IsRefPlaceholder(uint32_t node) const {
	return std::find(m_refPlaceholders.begin(), m_refPlaceholders.end(),
		node) != m_refPlaceholders.end();
}

bool EditorApp::AddSceneReference(const std::string& source,
	const std::string& name) {
	if (m_gltfPath.empty() || source.empty()) {
		Log("References: no loaded source scene / empty reference path.");
		return false;
	}
	if (HasPakExtension(m_gltfPath)) {
		Log("References: pak scenes are read-only (open the source glTF "
			"to author references).");
		return false;
	}
	// Resolve like the loader will (as-is, then beside the master scene)
	// so the dialog feedback is honest.
	std::error_code ec;
	std::filesystem::path hp(source);
	if (!std::filesystem::exists(hp, ec)) {
		hp = std::filesystem::path(m_gltfHostPath).parent_path() / source;
		if (!std::filesystem::exists(hp, ec)) {
			Log("References: cannot resolve '" + source + "'.");
			return false;
		}
	}
	smvk::tour::ReferenceCmd r{};
	// Parent: the primary selection (cluster space), else cluster root.
	r.parent = (m_selected != UINT32_MAX && m_selected < UINT32_MAX)
		? uint32_t(std::max(WorldToCluster(m_selected), 0))
		: 0;
	if (WorldToCluster(m_selected) < 0)
		r.parent = 0;
	r.source = source;
	r.name = name.empty()
		? std::filesystem::path(hp).stem().string() : name;
	m_references.push_back(std::move(r));
	SaveTour(); // persist the reference list
	Log("Reference added: '" + r.source + "' under cluster node " +
		std::to_string(r.parent) + " — reloading to bind.");
	// References bind at import: re-open the scene (deterministic ids
	// keep every existing tour command + override valid).
	return OpenScene(m_gltfPath);
}

bool EditorApp::RemoveSceneReference(uint32_t refWorldNode) {
	auto it = std::find(m_refPlaceholders.begin(), m_refPlaceholders.end(),
		refWorldNode);
	if (it == m_refPlaceholders.end())
		return false;
	const size_t idx = size_t(std::distance(m_refPlaceholders.begin(), it));
	if (idx >= m_references.size())
		return false;
	const std::string src = m_references[idx].source;
	m_references.erase(m_references.begin() + ptrdiff_t(idx));
	SaveTour();
	Log("Reference removed: '" + src + "' — reloading to rebind.");
	return OpenScene(m_gltfPath);
}

void EditorApp::DrawReferenceSection(uint32_t node) {
	// Only on reference placeholder nodes (the Inspector's general
	// transform/override blocks apply too — a reference is a node).
	if (!IsRefPlaceholder(node))
		return;
	auto it = std::find(m_refPlaceholders.begin(), m_refPlaceholders.end(),
		node);
	const size_t idx = size_t(std::distance(m_refPlaceholders.begin(), it));
	ImGui::Separator();
	ImGui::Text("Scene reference");
	if (idx < m_references.size()) {
		const smvk::tour::ReferenceCmd& r = m_references[idx];
		ImGui::TextDisabled("source: %s", r.source.c_str());
		ImGui::TextDisabled(
			"structure + link kept (nested references resolve through the "
			"source's own tour)");
		if (ImGui::Button("Remove reference (reload)"))
			RemoveSceneReference(node);
	}
	ImGui::TextDisabled(
		"Add more: File > Add scene reference... (parent = selection)");
}

void EditorApp::DrawAddReferenceDialog() {
	if (!ImGui::BeginPopupModal("Add scene reference", nullptr,
			ImGuiWindowFlags_NoCollapse))
		return;
	ImGui::SetNextItemWidth(420.0f);
	ImGui::InputTextWithHint("##refsrc", "referenced .gltf (relative to "
		"this scene or absolute)", m_refSourceBuf, sizeof(m_refSourceBuf));
	ImGui::InputTextWithHint("##refname", "name (optional)",
		m_refNameBuf, sizeof(m_refNameBuf));
	ImGui::TextDisabled("parent: %s",
		(m_selected != UINT32_MAX && WorldToCluster(m_selected) >= 0)
		? ("cluster node " + std::to_string(WorldToCluster(m_selected))).c_str()
		: "cluster root (nothing selected)");
	if (ImGui::Button("Add")) {
		if (AddSceneReference(m_refSourceBuf, m_refNameBuf)) {
			m_refSourceBuf[0] = '\0';
			m_refNameBuf[0] = '\0';
		}
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel"))
		ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

} // namespace editor
