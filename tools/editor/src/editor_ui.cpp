// Editor UI: menu bar, outliner (hierarchy), inspector (transform /
// material / flags), cook panel, console, viewport overlay.
//
// Every edit goes through the CRUD command stack (Do/Undo/Redo); drags
// coalesce into one command per gesture via m_pending.

#include "editor_app.h"

#include <cstring>
#include <algorithm>
#include <filesystem>
#include <iostream>

#include <glm/gtc/type_ptr.hpp>

#include <imgui.h>

namespace editor {
namespace {
constexpr float kLeftW = 330.0f;
constexpr float kRightW = 370.0f;
} // namespace

// Coalesced drag helpers (member functions for world access).
void EditorApp::FinalizePending() {
	if (!m_pending.active)
		return;
	EditCmd& c = m_pending.cmd;
	switch (c.verb) {
	case EditVerb::Translate:
	case EditVerb::Rotate:
	case EditVerb::Scale: {
		glm::vec3 t, r, s;
		ReadLocalTRS(c.node, t, r, s);
		if (c.verb == EditVerb::Translate)
			c.afterVec = t;
		else if (c.verb == EditVerb::Rotate)
			c.afterVec = r;
		else
			c.afterVec = s;
		break;
	}
	case EditVerb::SetMatAttr: {
		glm::vec4 v4;
		double num = 0.0;
		ReadSharedMatAttr(c.matWorld, c.attr, v4, num);
		c.vec4 = v4;
		c.number = num;
		break;
	}
	default:
		break;
	}
	PushDone(c);
	m_pending.active = false;
	m_pending.cmd = EditCmd{};
}

bool EditorApp::TrackDrag(EditVerb verb, uint32_t node,
	const glm::mat4& beforeMat) {
	if (ImGui::IsItemActivated()) {
		if (m_pending.active && (m_pending.cmd.verb != verb ||
			m_pending.cmd.node != node))
			FinalizePending(); // switching controls mid-gesture
		m_pending.active = true;
		m_pending.cmd = EditCmd{};
		m_pending.cmd.verb = verb;
		m_pending.cmd.node = node;
		m_pending.cmd.beforeMat = beforeMat;
		return true;
	}
	return m_pending.active && m_pending.cmd.verb == verb &&
		m_pending.cmd.node == node;
}

void EditorApp::FinishDrag() {
	PushDone(m_pending.cmd);
	m_pending.active = false;
	m_pending.cmd = EditCmd{};
}

void EditorApp::DrawUI(smvk::Extent2D targetExtent) {
	(void)targetExtent;
	if (ImGui::BeginMainMenuBar()) {
		DrawMenuBar();
		ImGui::EndMainMenuBar();
	}
	const float menuH = ImGui::GetFrameHeight();
	const float vh = float(ImGui::GetIO().DisplaySize.y);

	ImGui::SetNextWindowPos(ImVec2(0, menuH), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(kLeftW, vh - menuH - 170), ImGuiCond_Always);
	if (ImGui::Begin("Outliner", nullptr,
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse)) {
		DrawOutliner();
	}
	ImGui::End();

	ImGui::SetNextWindowPos(
		ImVec2(float(ImGui::GetIO().DisplaySize.x) - kRightW, menuH),
		ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(kRightW, vh - menuH - 170),
		ImGuiCond_Always);
	if (ImGui::Begin("Inspector + Cook", nullptr,
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse)) {
		DrawInspector();
		ImGui::Separator();
		DrawCookPanel();
	}
	ImGui::End();

	ImGui::SetNextWindowPos(ImVec2(0, vh - 170), ImGuiCond_Always);
	ImGui::SetNextWindowSize(
		ImVec2(float(ImGui::GetIO().DisplaySize.x), 170), ImGuiCond_Always);
	if (ImGui::Begin("Console", nullptr,
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse)) {
		DrawConsolePanel();
	}
	ImGui::End();

	DrawViewportOverlay(targetExtent);
	DrawFlintbasePanel();
}

void EditorApp::DrawMenuBar() {
	if (ImGui::BeginMenu("File")) {
		if (ImGui::MenuItem("Open glTF..."))
			m_showOpenDialog = true;
		if (ImGui::MenuItem("Save tour"))
			SaveTour();
		if (ImGui::MenuItem("Cook to .pak"))
			CookToPak();
		ImGui::Separator();
		if (ImGui::MenuItem("Quit"))
			RequestClose();
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Edit")) {
		const std::string ul = UndoLabel();
		const std::string rl = RedoLabel();
		if (ImGui::MenuItem(("Undo " + ul).c_str(), "Ctrl+Z", false,
			!ul.empty()))
			Undo();
		if (ImGui::MenuItem(("Redo " + rl).c_str(), "Ctrl+Shift+Z", false,
			!rl.empty()))
			Redo();
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("View")) {
		if (ImGui::MenuItem("Unlit (diffuse)", nullptr,
			m_viewMode == ViewMode::Unlit))
			m_viewMode = ViewMode::Unlit;
		if (ImGui::MenuItem("Lit", nullptr, m_viewMode == ViewMode::Lit))
			m_viewMode = ViewMode::Lit;
		if (ImGui::MenuItem("Lit + shadows", nullptr,
			m_viewMode == ViewMode::LitShadows))
			m_viewMode = ViewMode::LitShadows;
		ImGui::Separator();
		if (ImGui::MenuItem("UV0", "F4", m_viewMode == ViewMode::Uv0))
			m_viewMode = ViewMode::Uv0;
		if (ImGui::MenuItem("UV1 (lightmap)", "F5", m_viewMode == ViewMode::Uv1))
			m_viewMode = ViewMode::Uv1;
		if (ImGui::MenuItem("Vertex colors", "F6",
			m_viewMode == ViewMode::VertexColors))
			m_viewMode = ViewMode::VertexColors;
		ImGui::Separator();
		if (ImGui::MenuItem("Translate gizmo", "1",
			m_gizmoMode == GizmoMode::Translate))
			m_gizmoMode = GizmoMode::Translate;
		if (ImGui::MenuItem("Rotate gizmo", "2",
			m_gizmoMode == GizmoMode::Rotate))
			m_gizmoMode = GizmoMode::Rotate;
		if (ImGui::MenuItem("Scale gizmo", "3",
			m_gizmoMode == GizmoMode::Scale))
			m_gizmoMode = GizmoMode::Scale;
		ImGui::Separator();
		if (ImGui::MenuItem("Frame selection", "F"))
			FrameSelection();
		if (ImGui::MenuItem("Frame all", "Shift+F"))
			FrameAll();
		ImGui::Separator();
		if (ImGui::MenuItem("Flintbase browser", nullptr, m_showFlintbasePanel))
			m_showFlintbasePanel = !m_showFlintbasePanel;
		ImGui::Separator();
		ImGui::MenuItem("Selection wire overlay", nullptr, &m_wireOverlay);
		if (ImGui::MenuItem("Panels visible", nullptr, m_uiMode))
			m_uiMode = !m_uiMode;
		ImGui::EndMenu();
	}

	if (m_showOpenDialog) {
		ImGui::OpenPopup("Open glTF");
		m_showOpenDialog = false;
	}
	DrawOpenDialog();
}

void EditorApp::DrawOpenDialog() {
	namespace fs = std::filesystem;
	ImGui::SetNextWindowSize(ImVec2(620, 420), ImGuiCond_FirstUseEver);
	if (!ImGui::BeginPopupModal("Open glTF", nullptr,
		ImGuiWindowFlags_NoCollapse))
		return;
	auto closeDlg = [&]() {
		m_browseDir.clear();
		m_browseSel.clear();
		ImGui::CloseCurrentPopup();
	};
	if (m_browseDir.empty()) {
		// First frame: start from the path field's parent dir.
		std::error_code ec;
		fs::path d = fs::path(m_openPathBuf).parent_path();
		if (d.empty())
			d = fs::current_path(ec);
		if (!fs::is_directory(d, ec))
			d = ".";
		m_browseDir = d.string();
	}
	ImGui::InputText("##path", m_openPathBuf, sizeof(m_openPathBuf));
	ImGui::SameLine();
	if (ImGui::Button("Up")) {
		std::error_code ec;
		fs::path p(m_browseDir);
		if (p.has_parent_path()) {
			fs::path up = p.parent_path();
			if (fs::is_directory(up, ec))
				m_browseDir = up.string();
		}
		m_browseSel.clear();
	}
	ImGui::TextDisabled("%s", m_browseDir.c_str());
	ImGui::BeginChild("##browse", ImVec2(0, -60), true);
	{
		std::error_code ec;
		std::vector<fs::directory_entry> dirs, files;
		for (fs::directory_iterator it(m_browseDir, ec), end;
			!ec && it != end; it.increment(ec)) {
			const fs::directory_entry& e = *it;
			if (e.is_directory(ec))
				dirs.push_back(e);
			else if (e.is_regular_file(ec) &&
				e.path().extension() == ".gltf")
				files.push_back(e);
		}
		auto byName = [](const fs::directory_entry& a,
			const fs::directory_entry& b) {
			return a.path().filename().string() <
				b.path().filename().string();
		};
		std::sort(dirs.begin(), dirs.end(), byName);
		std::sort(files.begin(), files.end(), byName);
		// Parent entry.
		if (ImGui::Selectable("[..]", false,
			ImGuiSelectableFlags_AllowDoubleClick)) {
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				fs::path p(m_browseDir);
				if (p.has_parent_path()) {
					fs::path up = p.parent_path();
					if (fs::is_directory(up, ec))
						m_browseDir = up.string();
				}
				m_browseSel.clear();
			}
		}
		char label[512];
		for (auto& d : dirs) {
			const std::string name = d.path().filename().string();
			std::snprintf(label, sizeof(label), "[%s]", name.c_str());
			if (ImGui::Selectable(label, false,
				ImGuiSelectableFlags_AllowDoubleClick)) {
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
					m_browseDir = d.path().string();
					m_browseSel.clear();
				}
			}
		}
		for (auto& f : files) {
			const std::string full = f.path().string();
			const std::string name = f.path().filename().string();
			const bool sel = (m_browseSel == full);
			if (ImGui::Selectable(name.c_str(), sel,
				ImGuiSelectableFlags_AllowDoubleClick)) {
				m_browseSel = full;
				std::snprintf(m_openPathBuf, sizeof(m_openPathBuf),
					"%s", full.c_str());
			}
			if (sel)
				ImGui::SetItemDefaultFocus();
			if (ImGui::IsItemHovered() &&
				ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				LoadGltfIntoWorld(full);
				closeDlg();
				ImGui::EndChild();
				ImGui::EndPopup();
				return;
			}
		}
	}
	ImGui::EndChild();
	ImGui::TextDisabled("%s",
		m_browseSel.empty() ? "no file selected" : m_browseSel.c_str());
	if (ImGui::Button("Open")) {
		if (!m_browseSel.empty())
			std::snprintf(m_openPathBuf, sizeof(m_openPathBuf),
				"%s", m_browseSel.c_str());
		LoadGltfIntoWorld(m_openPathBuf);
		closeDlg();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel"))
		closeDlg();
	ImGui::EndPopup();
}

void EditorApp::DrawOutliner() {
	if (!m_world || m_world->scene.hierarchy.empty()) {
		ImGui::TextDisabled("No scene. File > Open glTF...");
		return;
	}
	ImGui::InputTextWithHint("##filter", "filter nodes...",
		m_filterBuf, sizeof(m_filterBuf));
	const bool filtering = m_filterBuf[0] != '\0';
	ImGui::TextDisabled("nodes: %zu  selected: %s",
		m_world->scene.hierarchy.size(),
		m_selected == UINT32_MAX ? "-" : getNodeName(m_world->scene,
			int(m_selected)).c_str());
	ImGui::Separator();
	ImGui::BeginChild("##tree");
	if (filtering) {
		for (uint32_t n = 0; n < uint32_t(m_world->scene.hierarchy.size()); ++n) {
			const std::string name = getNodeName(m_world->scene, int(n));
			if (name.find(m_filterBuf) == std::string::npos)
				continue;
			ImGui::PushID(int(n));
			const bool sel = (n == m_selected);
			char label[256];
			std::snprintf(label, sizeof(label), "%u: %s%s", n,
				name.empty() ? "(unnamed)" : name.c_str(),
				m_world->scene.meshForNode.contains(n) ? " [mesh]" : "");
			if (ImGui::Selectable(label, sel))
				m_selected = n;
			if (sel && n == m_revealNode)
				ImGui::SetScrollHereY(0.5f);
			ImGui::PopID();
		}
	}
	else {
		// Reveal a viewport pick: force-open the ancestor chain so the
		// selected node is visible, then scroll to it.
		std::unordered_set<uint32_t> expand;
		if (m_revealNode != UINT32_MAX) {
			uint32_t n = m_revealNode;
			while (n < uint32_t(m_world->scene.hierarchy.size())) {
				expand.insert(n);
				const int p = m_world->scene.hierarchy[n].parent;
				if (p < 0)
					break;
				n = uint32_t(p);
			}
		}
		for (int c = m_world->scene.hierarchy[0].firstChild; c != -1;
			c = m_world->scene.hierarchy[c].nextSibling)
			DrawOutlinerNode(uint32_t(c), expand.empty() ? nullptr : &expand);
	}
	ImGui::EndChild();
	m_revealNode = UINT32_MAX;
}

void EditorApp::DrawOutlinerNode(uint32_t node,
	const std::unordered_set<uint32_t>* expandSet) {
	const Scene& scene = m_world->scene;
	const std::string name = getNodeName(scene, int(node));
	const bool hasMesh = scene.meshForNode.contains(node);
	const bool sel = (node == m_selected);
	ImGui::PushID(int(node));
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
		ImGuiTreeNodeFlags_OpenOnDoubleClick;
	if (sel)
		flags |= ImGuiTreeNodeFlags_Selected;
	if (scene.hierarchy[node].firstChild < 0)
		flags |= ImGuiTreeNodeFlags_Leaf;
	if (expandSet && expandSet->contains(node))
		ImGui::SetNextItemOpen(true);
	char label[256];
	std::snprintf(label, sizeof(label), "%s%s%s", name.empty() ? "(node)" : name.c_str(),
		hasMesh ? " [mesh]" : "",
		IsNoLightmapNode(scene, int(node)) ? " [noLM]" : "");
	const bool open = ImGui::TreeNodeEx(label, flags);
	// Drag source (reparent by drop).
	if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
		ImGui::SetDragDropPayload("EDITOR_NODE", &node, sizeof(node));
		ImGui::Text("%s", label);
		ImGui::EndDragDropSource();
	}
	if (ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("EDITOR_NODE")) {
			const uint32_t src = *static_cast<const uint32_t*>(p->Data);
			EditCmd c{};
			c.verb = EditVerb::Parent;
			c.node = src;
			c.node2 = node;
			c.beforeParent = uint32_t(
				m_world->scene.hierarchy[src].parent);
			Do(c);
			Log("Reparented node " + std::to_string(src) + " under " +
				std::to_string(node));
		}
		ImGui::EndDragDropTarget();
	}
	if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
		m_selected = node;
	if (sel && node == m_revealNode)
		ImGui::SetScrollHereY(0.5f);
	if (open) {
		for (int c = scene.hierarchy[node].firstChild; c != -1;
			c = scene.hierarchy[c].nextSibling)
			DrawOutlinerNode(uint32_t(c), expandSet);
		ImGui::TreePop();
	}
	ImGui::PopID();
}

static const char* AlphaModeName(smvk::AlphaMode m) {
	switch (m) {
	case smvk::AlphaMode::Opaque: return "Opaque";
	case smvk::AlphaMode::Mask: return "Mask (cutout)";
	case smvk::AlphaMode::Blend: return "Blend";
	}
	return "?";
}

void EditorApp::DrawInspector() {
	if (!m_world || m_selected == UINT32_MAX ||
		m_selected >= m_world->scene.hierarchy.size()) {
		ImGui::TextDisabled("Nothing selected.");
		ImGui::TextWrapped("Click a mesh in the viewport or pick a node in the outliner. "
			"Drag nodes onto each other to reparent. All edits are undoable (Ctrl+Z).");
		return;
	}
	Scene& scene = m_world->scene;
	const uint32_t node = m_selected;
	ImGui::Text("Node %u", node);

	// Name.
	{
		char buf[256];
		const std::string cur = getNodeName(scene, int(node));
		std::snprintf(buf, sizeof(buf), "%s", cur.c_str());
		if (ImGui::InputText("Name", buf, sizeof(buf),
			ImGuiInputTextFlags_EnterReturnsTrue)) {
			EditCmd c{};
			c.verb = EditVerb::Rename;
			c.node = node;
			c.afterText = buf;
			c.beforeText = cur;
			Do(c);
		}
	}

	// Transform (coalesced drags: one command per gesture).
	{
		glm::vec3 t, r, s;
		ReadLocalTRS(node, t, r, s);
		float ts[3] = { t.x, t.y, t.z };
		float rs[3] = { r.x, r.y, r.z };
		float ss[3] = { s.x, s.y, s.z };
		ImGui::DragFloat3("Translate", ts, 0.05f);
		const bool tOwn = 	TrackDrag( EditVerb::Translate, node,
			scene.localTransform[node]);
		if (tOwn) {
			WriteLocalTRS(node, glm::vec3(ts[0], ts[1], ts[2]), r, s);
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				m_pending.cmd.afterVec = glm::vec3(ts[0], ts[1], ts[2]);
				FinishDrag();
			}
		}
		ImGui::DragFloat3("Rotate (deg)", rs, 0.5f);
		const bool rOwn = 	TrackDrag( EditVerb::Rotate, node,
			scene.localTransform[node]);
		if (rOwn) {
			WriteLocalTRS(node, t, glm::vec3(rs[0], rs[1], rs[2]), s);
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				m_pending.cmd.afterVec = glm::vec3(rs[0], rs[1], rs[2]);
				FinishDrag();
			}
		}
		ImGui::DragFloat3("Scale", ss, 0.02f);
		const bool sOwn = 	TrackDrag( EditVerb::Scale, node,
			scene.localTransform[node]);
		if (sOwn) {
			WriteLocalTRS(node, t, r, glm::vec3(ss[0], ss[1], ss[2]));
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				m_pending.cmd.afterVec = glm::vec3(ss[0], ss[1], ss[2]);
				FinishDrag();
			}
		}
	}

	// Parent.
	{
		const int p = scene.hierarchy[node].parent;
		ImGui::Text("Parent: %d (%s)", p,
			p >= 0 ? getNodeName(scene, p).c_str() : "-");
		ImGui::SameLine();
		if (ImGui::Button("Detach to root") && p != int(m_clusterBase)) {
			if (m_clusterBase == UINT32_MAX) {
				Log("Detach: no cluster mapping (multi-cluster scene?)");
			}
			else {
				EditCmd c{};
				c.verb = EditVerb::Parent;
				c.node = node;
				c.node2 = m_clusterBase;
				c.beforeParent = uint32_t(p);
				Do(c);
			}
		}
	}

	// Mesh info.
	if (auto mit = scene.meshForNode.find(node); mit != scene.meshForNode.end()) {
		if (smvk::StaticMesh* mesh = m_world->GetMesh(mit->second)) {
			uint32_t tris = 0;
			for (auto& s : mesh->geometry.surfaces)
				tris += s.indexCount / 3;
			ImGui::Text("Mesh: %u verts, %u tris", mesh->geometry.vertexCount, tris);
			ImGui::Text("LM chunk: %s",
				mesh->lmChunk == 0xFFFFFFFFu ? "none" : "baked");
		}
	}

	// Material reassignment.
	if (scene.materialForNode.contains(node)) {
		ImGui::Separator();
		ImGui::Text("Material");
		const uint32_t curMat = scene.materialForNode[node];
		std::string preview = "mat." + std::to_string(curMat);
		if (ImGui::BeginCombo("##mat", preview.c_str())) {
			const uint32_t count = uint32_t(m_world->materialLocations.size());
			for (uint32_t i = 0; i < count; ++i) {
				const auto& loc = m_world->materialLocations[i];
				std::string uri = loc.cluster
					? std::string(loc.cluster->GetMaterialUri(loc.handle).Str()) : "";
				char item[320];
				std::snprintf(item, sizeof(item), "%u: %s", i,
					uri.empty() ? "(unnamed)" : uri.c_str());
				const bool sel = (i == curMat);
				if (ImGui::Selectable(item, sel) && i != curMat) {
					EditCmd c{};
					c.verb = EditVerb::AssignMaterial;
					c.node = node;
					c.material = i;
					c.beforeMaterial = curMat;
					Do(c);
					Log("Node " + std::to_string(node) + " -> material " +
						std::to_string(i));
				}
				if (sel)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		// 2D material preview: unlit albedo swatch + slot summary.
		if (smvk::Material* prevMat = m_world->GetMaterial(curMat)) {
			ImGui::Separator();
			ImGui::Text("Preview (unlit swatch)");
			const glm::vec4& bc = prevMat->values.diffuse;
			ImGui::ColorButton("##matPreview",
				ImVec4(bc.x, bc.y, bc.z, bc.w),
				ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip,
				ImVec2(64, 64));
			ImGui::SameLine();
			ImGui::BeginGroup();
			ImGui::Text("base %.2f %.2f %.2f a %.2f", bc.x, bc.y, bc.z, bc.w);
			ImGui::Text("metal %.2f  rough %.2f",
				prevMat->values.params.y, prevMat->values.params.z);
			ImGui::Text("emis %.2f %.2f %.2f x %.2f",
				prevMat->values.emissiveFactor.x,
				prevMat->values.emissiveFactor.y,
				prevMat->values.emissiveFactor.z,
				prevMat->values.params2.z);
			const glm::ivec4& t0 = prevMat->values.textures;
			const glm::ivec4& t1 = prevMat->values.textures2;
			ImGui::Text("tex base:%d mr:%d n:%d occ:%d e:%d",
				t0.x, t0.y, t0.z, t0.w, t1.x);
			ImGui::TextDisabled("alpha: %s cut %.2f%s%s",
				AlphaModeName(prevMat->alphaMode), prevMat->alphaCutoff,
				prevMat->doubleSided ? " DS" : "",
				prevMat->castsShadows ? "" : " noShadow");
			ImGui::EndGroup();
		}
		// Shared material quick edit (affects every node using it).
		if (smvk::Material* mat = m_world->GetMaterial(curMat)) {
			ImGui::TextDisabled("Shared edit: affects all users of mat.%u", curMat);
			glm::vec4 bc = mat->values.diffuse;
			if (ImGui::ColorEdit4("Base color", glm::value_ptr(bc))) {
				if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetMatAttr ||
					m_pending.cmd.matWorld != curMat || m_pending.cmd.attr != "baseColor"))
					FinalizePending();
				if (!m_pending.active) {
					m_pending.active = true;
					m_pending.cmd = EditCmd{};
					m_pending.cmd.verb = EditVerb::SetMatAttr;
					m_pending.cmd.matWorld = curMat;
					m_pending.cmd.attr = "baseColor";
					ReadSharedMatAttr(curMat, "baseColor",
						m_pending.cmd.beforeVec4, m_pending.cmd.beforeNum);
				}
				WriteSharedMatAttr(curMat, "baseColor", bc, 0.0);
				m_pending.cmd.vec4 = bc;
			}
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
				m_pending.cmd.attr == "baseColor") {
				FinishDrag();
			}
			float metallic = mat->values.params.y;
			if (ImGui::SliderFloat("Metallic", &metallic, 0.0f, 1.0f)) {
				if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetMatAttr ||
					m_pending.cmd.matWorld != curMat || m_pending.cmd.attr != "metallic"))
					FinalizePending();
				if (!m_pending.active) {
					m_pending.active = true;
					m_pending.cmd = EditCmd{};
					m_pending.cmd.verb = EditVerb::SetMatAttr;
					m_pending.cmd.matWorld = curMat;
					m_pending.cmd.attr = "metallic";
					ReadSharedMatAttr(curMat, "metallic",
						m_pending.cmd.beforeVec4, m_pending.cmd.beforeNum);
				}
				WriteSharedMatAttr(curMat, "metallic", glm::vec4(0), metallic);
				m_pending.cmd.number = metallic;
			}
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
				m_pending.cmd.attr == "metallic") {
				FinishDrag();
			}
			float rough = mat->values.params.z;
			if (ImGui::SliderFloat("Roughness", &rough, 0.04f, 1.0f)) {
				if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetMatAttr ||
					m_pending.cmd.matWorld != curMat || m_pending.cmd.attr != "roughness"))
					FinalizePending();
				if (!m_pending.active) {
					m_pending.active = true;
					m_pending.cmd = EditCmd{};
					m_pending.cmd.verb = EditVerb::SetMatAttr;
					m_pending.cmd.matWorld = curMat;
					m_pending.cmd.attr = "roughness";
					ReadSharedMatAttr(curMat, "roughness",
						m_pending.cmd.beforeVec4, m_pending.cmd.beforeNum);
				}
				WriteSharedMatAttr(curMat, "roughness", glm::vec4(0), rough);
				m_pending.cmd.number = rough;
			}
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
				m_pending.cmd.attr == "roughness") {
				FinishDrag();
			}
			bool ds = mat->doubleSided;
			if (ImGui::Checkbox("Double sided", &ds)) {
				EditCmd c{};
				c.verb = EditVerb::SetMatAttr;
				c.matWorld = curMat;
				c.attr = "doubleSided";
				c.number = ds ? 1.0 : 0.0;
				ReadSharedMatAttr(curMat, "doubleSided",
					c.beforeVec4, c.beforeNum);
				Do(c);
			}
			bool cs = mat->castsShadows;
			if (ImGui::Checkbox("Casts shadows", &cs)) {
				EditCmd c{};
				c.verb = EditVerb::SetMatAttr;
				c.matWorld = curMat;
				c.attr = "castsShadows";
				c.number = cs ? 1.0 : 0.0;
				ReadSharedMatAttr(curMat, "castsShadows",
					c.beforeVec4, c.beforeNum);
				Do(c);
			}
			int alpha = int(mat->alphaMode);
			if (ImGui::Combo("Alpha", &alpha, "Opaque\0Mask (cutout)\0Blend\0")) {
				EditCmd c{};
				c.verb = EditVerb::SetMatAttr;
				c.matWorld = curMat;
				c.attr = "alphaMode";
				c.number = double(alpha);
				ReadSharedMatAttr(curMat, "alphaMode",
					c.beforeVec4, c.beforeNum);
				Do(c);
			}
			ImGui::TextDisabled("alpha: %s", AlphaModeName(mat->alphaMode));
		}
	}

	// Lightmap participation.
	{
		ImGui::Separator();
		const bool inherited = IsNoLightmapNode(scene, int(node));
		bool self = node < scene.noLightmap.size() && scene.noLightmap[node] != 0;
		if (ImGui::Checkbox("Exclude from lightmaps (subtree)", &self)) {
			EditCmd c{};
			c.verb = EditVerb::SetNoLightmap;
			c.node = node;
			c.afterFlag = self;
			c.beforeFlag = !self;
			Do(c);
		}
		ImGui::TextDisabled("effective: %s (cook --lightmap skips it)",
			inherited ? "excluded" : "baked");
	}

	// Sun quick controls.
	{
		ImGui::Separator();
		ImGui::Text("Viewport sun");
		ImGui::SliderFloat("Azimuth", &m_sunAzimuth, 0.0f, 360.0f);
		ImGui::SliderFloat("Elevation", &m_sunElevation, 0.0f, 90.0f);
		ImGui::SliderFloat("Intensity", &m_sunIntensity, 0.0f, 10.0f);
		ImGui::SliderFloat("Sky", &m_skyIntensity, 0.0f, 2.0f);
	}
}

void EditorApp::DrawCookPanel() {
	ImGui::Text("Cook to .pak (PakCooker --tour)");
	char pakBuf[512];
	std::snprintf(pakBuf, sizeof(pakBuf), "%s", m_cook.outPak.c_str());
	bool cookTouched = false;
	if (ImGui::InputText("Out pak", pakBuf, sizeof(pakBuf),
		ImGuiInputTextFlags_EnterReturnsTrue)) {
		m_cook.outPak = pakBuf;
		cookTouched = true;
	}
	if (ImGui::Checkbox("MeshOpt", &m_cook.meshopt))
		cookTouched = true;
	ImGui::SameLine();
	if (ImGui::Checkbox("LODs", &m_cook.lods))
		cookTouched = true;
	if (ImGui::Checkbox("Lightmaps (bake)", &m_cook.lightmap))
		cookTouched = true;
	if (m_cook.lightmap) {
		int dens = int(m_cook.lmDensity);
		if (ImGui::SliderInt("Density (tex/m)", &dens, 2, 128)) {
			m_cook.lmDensity = uint32_t(dens);
			cookTouched = true;
		}
		int lobes = int(m_cook.lmLobes);
		if (ImGui::SliderInt("Lobes", &lobes, 5, 12)) {
			m_cook.lmLobes = uint32_t(lobes);
			cookTouched = true;
		}
		int rays = int(m_cook.lmRays);
		if (ImGui::SliderInt("Rays/texel", &rays, 16, 2048)) {
			m_cook.lmRays = uint32_t(rays);
			cookTouched = true;
		}
		if (ImGui::SliderFloat("Sun az", &m_cook.sunAzimuth, 0.0f, 360.0f))
			cookTouched = true;
		if (ImGui::SliderFloat("Sun el", &m_cook.sunElevation, 0.0f, 90.0f))
			cookTouched = true;
		if (ImGui::SliderFloat("Sun I", &m_cook.sunIntensity, 0.0f, 50000.0f))
			cookTouched = true;
	}
	if (cookTouched)
		SaveTour(); // persist the cook block (not undoable)
	ImGui::TextDisabled("tour: %s (%zu commands)",
		TourPath().empty() ? "-" : std::filesystem::path(TourPath())
		.filename().string().c_str(),
		m_historyCursor);
	if (m_cookRunning) {
		ImGui::TextDisabled("Cooking... (see Console)");
	}
	else if (ImGui::Button("Cook")) {
		m_cook.outPak = pakBuf;
		CookToPak();
	}
}

void EditorApp::DrawConsolePanel() {
	ImGui::BeginChild("##log", ImVec2(0, 0), false,
		ImGuiWindowFlags_HorizontalScrollbar);
	for (auto& l : m_consoleLines)
		ImGui::TextUnformatted(l.c_str());
	if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
		ImGui::SetScrollHereY(1.0f);
	ImGui::EndChild();
}

void EditorApp::DrawViewportOverlay(smvk::Extent2D extent) {
	(void)extent;
	const float menuH = ImGui::GetFrameHeight();
	ImGui::SetNextWindowPos(ImVec2(kLeftW + 12, menuH + 8), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.55f);
	if (ImGui::Begin("##overlay", nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoInputs)) {
		ImGui::Text("%s  |  %s gizmo%s  |  %s",
			ViewModeName(m_viewMode),
			m_gizmoMode == GizmoMode::Translate ? "Translate" :
			m_gizmoMode == GizmoMode::Rotate ? "Rotate" : "Scale",
			m_gizmoMode == GizmoMode::Translate ? " (world)" : " (local)",
			m_gltfPath.empty() ? "no scene" : std::filesystem::path(
				m_gltfPath).filename().string().c_str());
		if (m_selected != UINT32_MAX && m_world)
			ImGui::Text("sel: %u %s", m_selected,
				getNodeName(m_world->scene, int(m_selected)).c_str());
		ImGui::TextDisabled("F1/F2/F3 view  F4/F5/F6 debug  1/2/3 gizmo  I ui");
		ImGui::TextDisabled("F frame sel  Shift+F frame all");
		ImGui::TextDisabled("click=select  Alt+LMB orbit  Alt+MMB pan  Alt+RMB dolly");
		ImGui::TextDisabled("Ctrl+Z / Ctrl+Shift+Z: undo %zu / redo %zu",
			UndoDepth(), RedoDepth());
	}
	ImGui::End();
}

} // namespace editor
