// Editor-side nested scene references (Clarisse-style) + the Maya-style
// Reference Editor panel: the master scene's reference list is scene
// DEFINITION — bound at import by gltf_import's ApplySceneReferences
// (placeholder group node + the referenced source's subtree, recursing
// through each source's own .tour). Adding/removing/replacing a
// reference persists the list and rebinds by reloading the scene (the
// import is the deterministic id contract). Sources may be .gltf (raw
// geometry) or .tour (the authored scene: its source gltf is imported,
// then its tour commands replay with id offsets).

#include "editor_app.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>

namespace editor {

namespace {

// Subtree node count below root (excluding the placeholder itself).
int RefSubtreeSize(const Scene& scene, uint32_t root) {
	if (root >= scene.hierarchy.size())
		return 0;
	int n = 0;
	std::vector<uint32_t> stack;
	for (int c = scene.hierarchy[root].firstChild; c >= 0;
		c = scene.hierarchy[size_t(c)].nextSibling)
		stack.push_back(uint32_t(c));
	while (!stack.empty()) {
		const uint32_t v = stack.back();
		stack.pop_back();
		++n;
		for (int c = scene.hierarchy[v].firstChild; c >= 0;
			c = scene.hierarchy[size_t(c)].nextSibling)
			stack.push_back(uint32_t(c));
	}
	return n;
}

// Lowercased extension without the dot ("" when none).
std::string RefExt(const std::string& path) {
	const size_t dot = path.find_last_of('.');
	if (dot == std::string::npos)
		return {};
	std::string e = path.substr(dot + 1);
	for (char& c : e)
		c = char(std::tolower((unsigned char)c));
	return e;
}

} // namespace

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
	return RemoveSceneReferenceAt(
		size_t(std::distance(m_refPlaceholders.begin(), it)));
}

bool EditorApp::RemoveSceneReferenceAt(size_t idx) {
	if (idx >= m_references.size())
		return false;
	const std::string src = m_references[idx].source;
	m_references.erase(m_references.begin() + ptrdiff_t(idx));
	SaveTour();
	Log("Reference removed: '" + src + "' — reloading to rebind.");
	if (m_gltfPath.empty())
		return true;
	return OpenScene(m_gltfPath);
}

bool EditorApp::SetSceneReferenceEnabled(size_t idx, bool enabled) {
	if (idx >= m_references.size())
		return false;
	if (m_references[idx].enabled == enabled)
		return true;
	m_references[idx].enabled = enabled;
	SaveTour();
	Log(std::string("Reference ") + (enabled ? "loaded" : "unloaded")
		+ ": '" + m_references[idx].source + "' — reloading to rebind.");
	if (m_gltfPath.empty())
		return true;
	return OpenScene(m_gltfPath);
}

uint32_t EditorApp::RefPlaceholderNode(size_t idx) const {
	if (!m_world || idx >= m_references.size())
		return UINT32_MAX;
	// Name match first (robust when a failed bind shifted the parallel
	// index assumption), index fallback second.
	const std::string want = "REF " + m_references[idx].name;
	for (uint32_t ph : m_refPlaceholders)
		if (getNodeName(m_world->scene, int(ph)) == want)
			return ph;
	if (idx < m_refPlaceholders.size())
		return m_refPlaceholders[idx];
	return UINT32_MAX;
}

static bool RefSourceExists(const std::string& gltfHostPath,
	const std::string& source) {
	namespace fs = std::filesystem;
	std::error_code ec;
	if (fs::exists(fs::path(source), ec))
		return true;
	if (!gltfHostPath.empty() && fs::exists(
			fs::path(gltfHostPath).parent_path() / source, ec))
		return true;
	return false;
}

void EditorApp::DrawReferencesPanel() {
	if (!m_showReferencesPanel)
		return;
	ImGui::SetNextWindowSize(ImVec2(680, 380), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("References", &m_showReferencesPanel)) {
		ImGui::End();
		return;
	}
	if (ImGui::Button("Add reference...")) {
		m_refSourceBuf[0] = '\0';
		m_refNameBuf[0] = '\0';
		ImGui::OpenPopup("Add scene reference");
	}
	ImGui::SameLine();
	if (ImGui::Button("Reload all") && !m_gltfPath.empty())
		OpenScene(m_gltfPath);
	ImGui::SameLine();
	ImGui::TextDisabled("%zu reference(s)%s", m_references.size(),
		m_gltfPath.empty() ? " (no scene open)" : "");
	// Mutating actions rebind the world (reload): never inside the row
	// loop — collect and apply after the table.
	int pendingRemove = -1;
	int pendingToggle = -1;
	bool pendingToggleVal = false;
	int pendingReplace = -1;
	if (ImGui::BeginTable("##refed",
			5,
			ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
				ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
		ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed,
			74.0f);
		ImGui::TableSetupColumn("Name");
		ImGui::TableSetupColumn("Source");
		ImGui::TableSetupColumn("Nodes", ImGuiTableColumnFlags_WidthFixed,
			52.0f);
		ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed,
			218.0f);
		ImGui::TableHeadersRow();
		for (size_t i = 0; i < m_references.size(); ++i) {
			const smvk::tour::ReferenceCmd& r = m_references[i];
			const uint32_t ph = RefPlaceholderNode(i);
			const bool bound = ph != UINT32_MAX;
			const bool fileOk = RefSourceExists(m_gltfHostPath, r.source);
			const bool isTour = RefExt(r.source) == "tour";
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			if (!r.enabled)
				ImGui::TextDisabled("unloaded");
			else if (!fileOk)
				ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
					"missing");
			else if (!bound)
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
					"unbound");
			else
				ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f),
					"loaded");
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%s", r.name.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("[%s]", isTour ? "tour" : "gltf");
			ImGui::TableSetColumnIndex(2);
			ImGui::TextDisabled("%s", r.source.c_str());
			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%d",
				(bound && m_world)
					? RefSubtreeSize(m_world->scene, ph)
					: 0);
			ImGui::TableSetColumnIndex(4);
			ImGui::PushID(int(i));
			if (bound && ImGui::SmallButton("Select"))
				m_selected = ph;
			ImGui::SameLine();
			if (ImGui::SmallButton(r.enabled ? "Unload" : "Load")) {
				pendingToggle = int(i);
				pendingToggleVal = !r.enabled;
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Replace..."))
				pendingReplace = int(i);
			ImGui::SameLine();
			if (ImGui::SmallButton("Remove"))
				pendingRemove = int(i);
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (pendingRemove >= 0)
		RemoveSceneReferenceAt(size_t(pendingRemove));
	if (pendingToggle >= 0)
		SetSceneReferenceEnabled(size_t(pendingToggle), pendingToggleVal);
	if (pendingReplace >= 0) {
		std::string picked;
		if (NativeOpenFile(picked) && !picked.empty()
			&& size_t(pendingReplace) < m_references.size()) {
			m_references[size_t(pendingReplace)].source = picked;
			SaveTour();
			Log("Reference replaced: '" + picked + "' — reloading to rebind.");
			if (!m_gltfPath.empty())
				OpenScene(m_gltfPath);
		}
	}
	ImGui::End();
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
		ImGui::TextDisabled("[%s] %s",
			RefExt(r.source) == "tour" ? "tour" : "gltf",
			r.enabled ? "loaded" : "unloaded (kept in the list)");
		ImGui::TextDisabled(
			"structure + link kept (nested references resolve through the "
			"source's own tour)");
		if (ImGui::Button("Remove reference (reload)"))
			RemoveSceneReference(node);
		ImGui::SameLine();
		if (ImGui::Button(r.enabled ? "Unload" : "Load"))
			SetSceneReferenceEnabled(idx, !r.enabled);
	}
	ImGui::TextDisabled(
		"Add more: File > Add scene reference... (parent = selection)");
}

void EditorApp::DrawAddReferenceDialog() {
	if (!ImGui::BeginPopupModal("Add scene reference", nullptr,
			ImGuiWindowFlags_NoCollapse))
		return;
	ImGui::SetNextItemWidth(420.0f);
	ImGui::InputTextWithHint("##refsrc", "referenced .gltf/.tour (relative to "
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
