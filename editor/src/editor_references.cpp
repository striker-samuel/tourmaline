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

namespace {

// Displacement placeholder names: "REFPH_<ref>_<origParentWorldId>".
// One placeholder per (reference, original parent): every displaced
// created child that shared the parent gathers under it. The name
// carries the restore mapping, so toggling survives save/load with no
// extra persistence (the createTransform + parent commands replay).
std::string RefPlaceholderName(const std::string& ref, uint32_t parent) {
	return "REFPH_" + ref + "_" + std::to_string(parent);
}
bool ParseRefPlaceholder(const std::string& ref, const std::string& name,
	uint32_t& parentOut) {
	const std::string prefix = "REFPH_" + ref + "_";
	if (name.rfind(prefix, 0) != 0)
		return false;
	const std::string tail = name.substr(prefix.size());
	if (tail.empty())
		return false;
	try {
		const size_t pos = tail.find_first_not_of("0123456789");
		if (pos != std::string::npos)
			return false;
		parentOut = uint32_t(std::stoull(tail));
		return true;
	}
	catch (...) {
		return false;
	}
}

} // namespace

void EditorApp::DisplaceCreatedUnderRef(size_t refIdx) {
	// A created node (light, terrain group) parented under a reference
	// that gets DISABLED would inherit the hidden subtree. Move each
	// affected node under a placeholder transform at ROOT carrying the
	// original parent's local TRS values instead. Re-enable restores
	// the original parenting and deletes the placeholder
	// (RestoreRefPlaceholders).
	if (!m_world || refIdx >= m_references.size())
		return;
	const uint32_t ph = RefPlaceholderNode(refIdx);
	if (ph == UINT32_MAX)
		return;
	auto underRef = [&](uint32_t n) {
		for (int a = (n < m_world->scene.hierarchy.size())
				? m_world->scene.hierarchy[n].parent : -1;
			a >= 0 && size_t(a) < m_world->scene.hierarchy.size();
			a = m_world->scene.hierarchy[size_t(a)].parent)
			if (uint32_t(a) == ph)
				return true;
		return false;
	};
	// Created lights, cvolumes + terrain groups under this reference's
	// subtree. (Displacement placeholders themselves always live at
	// root, so they never qualify.)
	std::vector<uint32_t> displaced;
	for (const auto& [n, li] : m_world->scene.lightForNode) {
		(void)li;
		if (CreatedLightIndex(n) >= 0 && underRef(n))
			displaced.push_back(n);
	}
	for (const auto& [n, cv] : m_world->scene.cvolForNode) {
		(void)cv;
		if (CreatedCvolIndex(n) >= 0 && underRef(n))
			displaced.push_back(n);
	}
	for (const auto& [n, cm] : m_world->scene.cubemapForNode) {
		(void)cm;
		if (CreatedCubemapIndex(n) >= 0 && underRef(n))
			displaced.push_back(n);
	}
	for (const auto& [g, tg] : m_terrainGroups) {
		(void)tg;
		if (underRef(g))
			displaced.push_back(g);
	}
	if (displaced.empty())
		return;
	// Group by original direct parent: one placeholder each.
	std::map<uint32_t, std::vector<uint32_t>> byParent;
	for (uint32_t n : displaced)
		byParent[uint32_t(m_world->scene.hierarchy[n].parent)]
			.push_back(n);
	const std::string& refName = m_references[refIdx].name;
	for (auto& [parent, nodes] : byParent) {
		// Always mint a fresh placeholder (deterministic replay: the
		// name may duplicate an older tombstoned one — harmless, the
		// restore pass handles each independently).
		glm::vec3 t, rDeg, s;
		ReadLocalTRS(parent, t, rDeg, s);
		EditCmd c{};
		c.verb = EditVerb::CreateTransform;
		c.node = UINT32_MAX; // assigned at forward execution
		c.node2 = 0;         // world root
		c.afterText = RefPlaceholderName(refName, parent);
		c.afterVec = t;
		c.vec4 = glm::vec4(rDeg, 0.0f);
		c.svec = s;
		Do(c);
		const uint32_t holder = c.node;
		for (uint32_t n : nodes) {
			EditCmd p{};
			p.verb = EditVerb::Parent;
			p.node = n;
			p.node2 = holder;
			p.beforeParent = parent;
			Do(p);
		}
	}
	Log("Reference '" + refName + "': " + std::to_string(displaced.size())
		+ " created node(s) moved under placeholder(s) at root.");
}

void EditorApp::RestoreRefPlaceholders(size_t refIdx) {
	// Inverse of DisplaceCreatedUnderRef: every "REFPH_<ref>_<parent>"
	// placeholder's children move back under <parent>; the placeholder
	// itself deletes (tombstone — its createTransform stays silent in
	// the file while deleted). Runs BEFORE the unhide so nothing pops.
	if (!m_world || refIdx >= m_references.size())
		return;
	const std::string& refName = m_references[refIdx].name;
	bool any = false;
	for (uint32_t n = 0;
		n < uint32_t(m_world->scene.hierarchy.size()); ++n) {
		uint32_t origParent = UINT32_MAX;
		if (!ParseRefPlaceholder(refName,
				getNodeName(m_world->scene, int(n)), origParent))
			continue;
		if (origParent >= m_world->scene.hierarchy.size()) {
			Log("Reference '" + refName + "': placeholder #"
				+ std::to_string(n) + " names a gone parent (skipped).");
			continue;
		}
		std::vector<uint32_t> kids;
		for (int c = m_world->scene.hierarchy[n].firstChild; c >= 0;
			c = m_world->scene.hierarchy[size_t(c)].nextSibling)
			kids.push_back(uint32_t(c));
		for (uint32_t k : kids) {
			if (m_deletedNodes.contains(k))
				continue; // deleted while displaced: stays deleted
			EditCmd p{};
			p.verb = EditVerb::Parent;
			p.node = k;
			p.node2 = origParent;
			p.beforeParent = n;
			Do(p);
		}
		if (!m_deletedNodes.contains(n)) {
			EditCmd d{};
			d.verb = EditVerb::Delete;
			d.node = n;
			Do(d);
		}
		any = true;
	}
	if (any)
		Log("Reference '" + refName
			+ "': placeholder(s) gone, original parenting restored.");
}

void EditorApp::RebuildDisabledRefHides() {
	m_disabledRefRoots.clear();
	if (!m_world)
		return;
	for (size_t i = 0; i < m_references.size(); ++i) {
		const uint32_t ph = RefPlaceholderNode(i);
		if (ph == UINT32_MAX || ph >= m_world->scene.hierarchy.size()) {
			if (m_world)
				Log("Reference '" + m_references[i].source
					+ "': placeholder not bound — "
					"checkbox toggles land on rebind.");
			continue;
		}
		// The checkbox flag is the AUTHORITY for a reference
		// placeholder's visibility — it runs AFTER the tour replay, so
		// a stale/legacy visibility command on the placeholder can
		// never outvote it. Disabled: hidden subtree on the RESOLVED
		// world (the nodes stay — ids stable; the per-pass visibility
		// filters stop drawing/picking and the outliner drops the
		// tree; the runtime draw-list build is untouched). Enabled:
		// clear any inherited hidden state.
		if (m_references[i].enabled) {
			ClearHidden(m_world->scene, int(ph));
			continue;
		}
		MarkHidden(m_world->scene, int(ph));
		m_disabledRefRoots.insert(ph);
	}
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
	if (HasPakExtension(source)) {
		// Sources must be .gltf or .tour: paks are cooked blobs, not
		// referenceable scenes (no import, no tour to replay).
		Log("References: sources must be .gltf or .tour (paks are "
			"cooked, not referenceable).");
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
	// Asset identity travels with the path: moves/renames of the source
	// keep binding through the GUID (path stays as the fallback).
	r.guid = ProjectAssetGuid(hp.generic_string());
	r.name = name.empty()
		? std::filesystem::path(hp).stem().string() : name;
	m_references.push_back(std::move(r));
	SaveTour(); // persist the reference list
	Log("Reference added: '" + r.source + "' under cluster node " +
		std::to_string(r.parent) + " — reloading to bind.");
	// References bind at import: rebind through a temp snapshot of the
	// WORKING doc (the on-disk target may be a save-as tour far from
	// the source; the snapshot always carries the live list + edits).
	return ReloadReferencesViaTemp();
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
	if (m_references[idx].root) {
		Log("Remove: reference '" + m_references[idx].name
			+ "' is the scene source — the scene IS it (open a"
			" different scene instead).");
		return false;
	}
	const std::string src = m_references[idx].source;
	m_references.erase(m_references.begin() + ptrdiff_t(idx));
	SaveTour();
	Log("Reference removed: '" + src + "' — reloading to rebind.");
	if (m_gltfPath.empty())
		return true;
	// Rebind through the temp snapshot (live list + unsaved edits; the
	// on-disk target may be a save-as tour far from the source).
	return ReloadReferencesViaTemp();
}

bool EditorApp::RemoveSceneReferencesAt(std::vector<size_t> idxs) {
	// Dedupe, drop out-of-range + the root entry (the scene source is
	// not removable — same guard as the single path above).
	std::sort(idxs.begin(), idxs.end());
	idxs.erase(std::unique(idxs.begin(), idxs.end()), idxs.end());
	std::vector<size_t> doomed;
	for (size_t idx : idxs) {
		if (idx >= m_references.size())
			continue;
		if (m_references[idx].root) {
			Log("Remove: reference '" + m_references[idx].name
				+ "' is the scene source — the scene IS it (open a"
				" different scene instead).");
			continue;
		}
		doomed.push_back(idx);
	}
	if (doomed.empty())
		return false;
	// Erase descending so earlier indices stay valid.
	std::sort(doomed.begin(), doomed.end(), std::greater<size_t>());
	std::vector<std::string> srcs;
	for (size_t idx : doomed) {
		srcs.push_back(m_references[idx].source);
		m_references.erase(m_references.begin() + ptrdiff_t(idx));
	}
	SaveTour();
	for (const std::string& src : srcs)
		Log("Reference removed: '" + src + "' — reloading to rebind.");
	if (m_gltfPath.empty())
		return true;
	return ReloadReferencesViaTemp();
}

bool EditorApp::SetSceneReferenceEnabled(size_t idx, bool enabled) {
	if (idx >= m_references.size())
		return false;
	if (m_references[idx].root) {
		Log("The scene source is the scene — it cannot be"
			" disabled (that would hide the entire scene).");
		return false;
	}
	if (m_references[idx].enabled == enabled)
		return true;
	// Keep-in-scene toggle: DISABLE NEVER REMOVES the reference's nodes
	// — it hides the placeholder subtree. No re-import happens at all,
	// so every cluster id (and therefore every edit made on top of the
	// referenced nodes, master included) stays exactly where it was.
	// The flag persists for the next import (ApplySceneReferences inlines
	// disabled references too and marks the subtree hidden); the runtime
	// state rides the ordinary undoable visibility command.
	m_references[idx].enabled = enabled;
	const uint32_t ph = RefPlaceholderNode(idx);
	if (!enabled) {
		// Displace FIRST (subtree still addressable): created content
		// under the reference moves to root placeholders so it never
		// inherits the hidden subtree; then the visibility command.
		DisplaceCreatedUnderRef(idx);
	}
	else {
		// Restore FIRST (still hidden, no visual pop): placeholders go
		// away and original parenting returns; then the unhide.
		RestoreRefPlaceholders(idx);
	}
	if (m_world && ph != UINT32_MAX) {
		// The ENABLED flag (the reference line's bool attr) is the
		// single source of truth — deliberately NOT a SetVisibility
		// command (those belong to user-authored visibility edits and
		// would fight the flag on every toggle/reload). Apply the
		// hide/show straight onto the scene instead.
		if (enabled)
			ClearHidden(m_world->scene, int(ph));
		else
			MarkHidden(m_world->scene, int(ph));
		AutosaveTour(); // the flag rides the same doc
		Log(std::string("Reference ") + (enabled ? "shown" : "hidden")
			+ ": '" + m_references[idx].source + "' (placeholder #"
			+ std::to_string(ph) + ", kept in the scene).");
	}
	else {
		AutosaveTour();
		Log(std::string("Reference ") + (enabled ? "shown" : "hidden")
			+ ": '" + m_references[idx].source
			+ "' (placeholder not bound — enable/hide lands on rebind).");
	}
	// Outliner + world hide state follow the flag immediately.
	RebuildDisabledRefHides();
	return true;
}

uint32_t EditorApp::RefPlaceholderNode(size_t idx) const {
	if (!m_world || idx >= m_references.size())
		return UINT32_MAX;
	// Root references (the scene source) carry no placeholder by
	// design — their sentinel entry maps to UINT32_MAX.
	if (m_references[idx].root)
		return UINT32_MAX;
	// Name match first (robust when a failed bind shifted the parallel
	// index assumption), index fallback second.
	const std::string want = "REF " + m_references[idx].name;
	for (uint32_t ph : m_refPlaceholders)
		if (ph != UINT32_MAX && getNodeName(m_world->scene, int(ph)) == want)
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

bool EditorApp::ReloadReferencesViaTemp() {
	if (m_gltfPath.empty() || !m_world)
		return false;
	// Snapshot the UI + save state a full rebind would otherwise reset:
	// cameras (the loader refits viewport 0), selection (ids are
	// deterministic when the reference set is unchanged), the save-as
	// target (the loader resets it to the default) and the dirty flag
	// (the replay clears it, but unsaved edits are still unsaved).
	const std::vector<EditorCam> savedCams = m_editorCams;
	const uint32_t savedSelected = m_selected;
	const std::vector<uint32_t> savedSelection = m_selection;
	const std::string savedTourPath = m_tourPath;
	const bool savedDirty = m_tourDirty;
	// The working doc — history, orphans, overrides, env, placement,
	// images, reference list + flags — lands in a TEMP tour (never the
	// real file: reload must not save, and the on-disk file may be
	// stale behind the autosave). SaveTourTo retargets m_tourPath and
	// clears the dirty flag as a side effect — restore both at once.
	namespace fs = std::filesystem;
	std::string stem = "scene";
	try {
		stem = fs::path(m_gltfPath).stem().string();
		if (stem.empty())
			stem = "scene";
	}
	catch (...) {}
	std::error_code ec;
	fs::path tmp = fs::temp_directory_path(ec) /
		("tourmaline_" + stem + "_reload.tour");
	if (ec)
		return false;
	if (!SaveTourTo(tmp.generic_string())) {
		Log("Reference reload: snapshot failed — reload cancelled.");
		m_tourPath = savedTourPath;
		m_tourDirty = savedDirty;
		return false;
	}
	m_tourPath = savedTourPath;
	m_tourDirty = savedDirty;
	Log("Reference reload: rebinding through a temp snapshot (the tour file is untouched).");
	const bool ok = LoadGltfIntoWorld(m_gltfPath, tmp.generic_string());
	std::error_code rmEc;
	fs::remove(tmp, rmEc);
	// Restore the session frame around the fresh bind (ids are stable
	// when the reference set did not structurally change; anything
	// out of bounds after a source edit is dropped, not crashed on).
	m_tourPath = savedTourPath;
	m_tourDirty = savedDirty;
	if (!savedCams.empty())
		m_editorCams = savedCams;
	if (ok && m_world) {
		const uint32_t n = uint32_t(m_world->scene.hierarchy.size());
		if (savedSelected != UINT32_MAX && savedSelected < n) {
			m_selected = savedSelected;
			m_selection.clear();
			for (uint32_t s : savedSelection)
				if (s < n)
					m_selection.push_back(s);
		}
		else {
			ClearSelection();
		}
	}
	return ok;
}

void EditorApp::DrawReferencesPanel() {
	if (!m_showReferencesPanel)
		return;
	ImGui::SetNextWindowSize(ImVec2(720, 420), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Reference editor", &m_showReferencesPanel)) {
		ImGui::End();
		return;
	}
	// Add = a file browser (.gltf/.tour pick; the name defaults to the
	// path stem — Maya-style namespaces apply at bind time).
	if (ImGui::Button("Add reference...")) {
		std::string picked;
		if (NativeOpenFile(picked) && !picked.empty())
			AddSceneReference(picked, {});
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%zu reference(s)%s", m_references.size(),
		m_gltfPath.empty() ? " (no scene open)" : "");
	ImGui::SameLine();
	ImGui::TextDisabled("read-only trees; edits layer ON TOP (visibility/"
		"TRS/attrs) — root namespace = this tour");
	// Mutating actions rebind the world (reload): never inside the row
	// loop — collect and apply after the table.
	int pendingRemove = -1;
	int pendingToggle = -1;
	bool pendingToggleVal = false;
	int pendingReplace = -1;
	int pendingReload = -1;
	if (ImGui::BeginTable("##refed",
			6,
			ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
				ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
		ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed,
			58.0f);
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
			const bool isRoot = r.root; // the scene's own source
			const uint32_t ph = RefPlaceholderNode(i);
			const bool bound = ph != UINT32_MAX;
			const bool fileOk = RefSourceExists(m_gltfHostPath, r.source);
			const bool isTour = RefExt(r.source) == "tour";
			// Canonical RESOLVED source (bind-time): what the raw
			// tour string actually bound to — path-solver/symlink
			// users verify here. Empty = it did not resolve.
			const std::string resolved = (i < m_refResolved.size())
				? m_refResolved[i] : std::string();
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::PushID(int(i));
			if (isRoot) {
				// The root reference IS the scene — no toggle.
				ImGui::TextDisabled("(root)");
			}
			else {
				bool en = r.enabled;
				if (ImGui::Checkbox("##en", &en)) {
					pendingToggle = int(i);
					pendingToggleVal = en;
				}
			}
			ImGui::TableSetColumnIndex(1);
			if (isRoot)
				ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f),
					"source");
			else if (!r.enabled)
				ImGui::TextDisabled("hidden (kept)");
			else if (!fileOk)
				ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
					"missing");
			else if (!bound)
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
					"unbound");
			else
				ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f),
					"loaded");
			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%s", r.name.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("[%s]", isTour ? "tour" : "gltf");
			ImGui::TableSetColumnIndex(3);
			ImGui::TextDisabled("%s", r.source.c_str());
			if (resolved.empty())
				ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
					"(unresolved)");
			else if (resolved != r.source)
				ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f),
					"-> %s", resolved.c_str());
			ImGui::TableSetColumnIndex(4);
			ImGui::Text("%d",
				(bound && m_world)
					? RefSubtreeSize(m_world->scene, ph)
					: 0);
			ImGui::TableSetColumnIndex(5);
			if (!isRoot) {
				if (bound && ImGui::SmallButton("Select"))
					m_selected = ph;
				ImGui::SameLine();
				if (ImGui::SmallButton("Reload")) {
					pendingReload = int(i);
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("Replace..."))
					pendingReplace = int(i);
				ImGui::SameLine();
				if (ImGui::SmallButton("Remove"))
					pendingRemove = int(i);
			}
			else {
				ImGui::TextDisabled("the scene itself");
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (pendingRemove >= 0)
		RemoveSceneReferenceAt(size_t(pendingRemove));
	if (pendingToggle >= 0)
		SetSceneReferenceEnabled(size_t(pendingToggle), pendingToggleVal);
	if (pendingReload >= 0
		&& !m_gltfPath.empty()
		&& size_t(pendingReload) < m_references.size()) {
		// Rebind through a temp snapshot of the WORKING doc (never the
		// stale on-disk file, never an implicit save): unsaved edits,
		// reference flags, cameras and selection all survive.
		Log("Reference reload: '" + m_references[size_t(pendingReload)].source
			+ "' — rebinding.");
		ReloadReferencesViaTemp();
	}
	if (pendingReplace >= 0) {
		std::string picked;
		if (NativeOpenFile(picked) && !picked.empty()
			&& size_t(pendingReplace) < m_references.size()) {
			m_references[size_t(pendingReplace)].source = picked;
			// A new source means new identity: never keep the old GUID.
			m_references[size_t(pendingReplace)].guid =
				ProjectAssetGuid(picked);
			SaveTour();
			Log("Reference replaced: '" + picked + "' — reloading to rebind.");
			if (!m_gltfPath.empty())
				ReloadReferencesViaTemp();
		}
	}
	// Failed reference commands (from the last bind): a partially-
	// broken reference must never take the scene down — it lands here.
	if (!m_refIssues.empty()) {
		ImGui::Separator();
		ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
			"%zu reference issue(s) from the last bind:",
			m_refIssues.size());
		ImGui::BeginChild("##refissues", ImVec2(0, 120), true);
		for (const std::string& issue : m_refIssues)
			ImGui::BulletText("%s", issue.c_str());
		ImGui::EndChild();
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
			r.enabled ? "shown" : "hidden (kept in the scene)");
		ImGui::TextDisabled(
			"read-only tree; edits layer on top (visibility/TRS/attrs); "
			"toggling never removes nodes (stable ids, edits survive)");
		if (ImGui::Button("Remove reference (reload)"))
			RemoveSceneReference(node);
		ImGui::SameLine();
		if (ImGui::Button(r.enabled ? "Hide (keep)" : "Show"))
			SetSceneReferenceEnabled(idx, !r.enabled);
	}
	ImGui::TextDisabled(
		"Add more: File > Add scene reference... (parent = selection)");
}

} // namespace editor
