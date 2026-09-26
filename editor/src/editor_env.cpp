// Editor environment (IBL) settings: viewport lighting lives in
// EditorEnv blocks — the editor default, plus per-node ENVIRONMENT
// NODES (skybox + EV/volumetrics authoring; position = node translate,
// size = node scale; convex-volume rendering comes later). The current
// source feeds the viewport frame. NEVER cooked: persisted as editorSet
// lines in the .tour, which ApplyTourToCluster ignores entirely.

#include "editor_app.h"

#include <cstdio>
#include <sstream>

#include <imgui.h>

namespace editor {
namespace {

std::string Vec3Str(const glm::vec3& v) {
	char buf[128];
	std::snprintf(buf, sizeof(buf), "%.4f %.4f %.4f", v.x, v.y, v.z);
	return buf;
}

glm::vec3 StrVec3(const std::string& s, const glm::vec3& fallback) {
	std::istringstream in(s);
	glm::vec3 v = fallback;
	in >> v.x >> v.y >> v.z;
	return v;
}

float StrFloat(const std::string& s, float fallback) {
	try {
		return s.empty() ? fallback : std::stof(s);
	}
	catch (...) {
		return fallback;
	}
}

} // namespace

void EditorApp::MarkEnvNode(uint32_t node) {
	if (!m_world || node >= m_world->scene.hierarchy.size())
		return;
	if (!m_envByNode.contains(node)) {
		m_envByNode[node] = CurrentEnv(); // start from the current source
		Log("Node " + std::to_string(node) + " marked as Environment.");
	}
}

void EditorApp::UnmarkEnvNode(uint32_t node) {
	if (m_envByNode.erase(node) > 0 && m_envCurrent == node)
		m_envCurrent = UINT32_MAX; // fall back to the editor default
}

void EditorApp::DrawEnvPanel() {
	ImGui::Separator();
	ImGui::Text("Environment (editor IBL)");
	ImGui::TextDisabled(
		"Editor viewport lighting only — never cooked. Pick the Editor "
		"default or an Environment node as the current source.");
	// Source combo: editor default + every env node.
	std::string curLabel = "Editor default";
	if (m_envCurrent != UINT32_MAX)
		curLabel = "Node " + std::to_string(m_envCurrent);
	if (ImGui::BeginCombo("Source##env", curLabel.c_str())) {
		if (ImGui::Selectable("Editor default", m_envCurrent == UINT32_MAX))
			m_envCurrent = UINT32_MAX;
		for (const auto& [node, env] : m_envByNode) {
			const std::string label = "Node " + std::to_string(node);
			if (ImGui::Selectable(label.c_str(), m_envCurrent == node))
				m_envCurrent = node;
		}
		ImGui::EndCombo();
	}
	EditorEnv& e = CurrentEnv();
	ImGui::SliderFloat("Sun azimuth", &e.sunAzimuth, 0.0f, 360.0f);
	ImGui::SliderFloat("Sun elevation", &e.sunElevation, 0.0f, 90.0f);
	ImGui::SliderFloat("Sun intensity", &e.sunIntensity, 0.0f, 10.0f);
	ImGui::ColorEdit3("Sun color", glm::value_ptr(e.sunColor));
	ImGui::ColorEdit3("Sky color", glm::value_ptr(e.skyColor));
	ImGui::SliderFloat("Sky intensity", &e.skyIntensity, 0.0f, 2.0f);
	ImGui::SliderFloat("Exposure", &e.exposure, 0.1f, 4.0f);
	ImGui::SliderFloat("Fog density", &e.fogDensity, 0.0f, 1.0f);
	ImGui::ColorEdit3("Fog color", glm::value_ptr(e.fogColor));
	ImGui::SliderFloat("Volumetrics", &e.volumetrics, 0.0f, 1.0f);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(
			"Authored for the future volumetric pass (editor viewport "
			"does not render it yet).");
	{
		char skybox[512];
		std::snprintf(skybox, sizeof(skybox), "%s", e.skyboxUri.c_str());
		if (ImGui::InputText("Skybox URI", skybox, sizeof(skybox),
			ImGuiInputTextFlags_EnterReturnsTrue))
			e.skyboxUri = skybox;
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(
			"Skybox texture reference (sampling is TODO; stored with the "
			"environment).");
	if (m_envCurrent != UINT32_MAX) {
		ImGui::TextDisabled(
			"Env node %u: position = node translate, size = node scale "
			"(convex volume support comes later).", m_envCurrent);
		if (ImGui::Button("Stop using this node##env"))
			m_envCurrent = UINT32_MAX;
		ImGui::SameLine();
		if (ImGui::Button("Unmark node##env"))
			UnmarkEnvNode(m_envCurrent);
	}
}

void EditorApp::DrawWorldPanel() {
	ImGui::Text("World");
	ImGui::TextDisabled(
		"Not a scene node: global render settings. No transform, no "
		"reparent, no delete. Sun edits below are undoable light "
		"commands; the environment rides editorSet tour lines.");
	// ---- scene sun (directional light, if the scene has one) ----
	ImGui::Separator();
	ImGui::Text("Sun (directional)");
	const uint32_t sun = FindSunNode();
	if (sun == UINT32_MAX || !m_world) {
		// Cooked world sun (no scene directional): these ARE the pak sun
		// — the cooker synthesizes a directional from them so the world
		// sun travels to the App. Same values drive the lightmap bake.
		ImGui::TextDisabled("No directional light: cooked world sun.");
		float az = m_cook.sunAzimuth;
		if (ImGui::DragFloat("Sun azimuth", &az, 1.0f, 0.0f, 360.0f,
				"%.1f")) {
			m_cook.sunAzimuth = az;
			AutosaveTour();
		}
		float el = m_cook.sunElevation;
		if (ImGui::DragFloat("Sun elevation", &el, 1.0f, 0.0f, 90.0f,
				"%.1f")) {
			m_cook.sunElevation = el;
			AutosaveTour();
		}
		float si = m_cook.sunIntensity;
		if (ImGui::DragFloat("Sun intensity (lux)", &si, 10.0f, 0.0f,
				2e5f, "%.0f")) {
			m_cook.sunIntensity = si;
			AutosaveTour();
		}
		glm::vec3 sc = m_cook.sunColor;
		if (ImGui::ColorEdit3("Sun color", &sc.x)) {
			m_cook.sunColor = sc;
			AutosaveTour();
		}
		ImGui::TextDisabled("Import a KHR punctual directional to drive "
			"scene sunlight per-node instead; the IBL sun below always "
			"lights the viewport.");
	}
	else if (glm::vec3 aimWs(0.0f);
		const ::SceneLight* li = FindNodeLight(sun, &aimWs)) {
		ImGui::Text("'%s' (#%u)", li->name.c_str(), sun);
		ImGui::SameLine();
		if (ImGui::Button("Select node"))
			SetSelection(sun);
		ImGui::TextDisabled("toSun=(%.2f,%.2f,%.2f)%s", -aimWs.x,
			-aimWs.y, -aimWs.z,
			m_hasCookedSun && m_cookedSunName == li->name
			? " — seeds the editor IBL default" : "");
		if ((li->flags & ::SceneLight::kSceneLightFlag_Static) != 0)
			ImGui::TextDisabled("STATIC: bake-only (excluded from runtime)");
		// Same coalesced-drag SetLightAttr pattern as the node inspector
		// (lux units for directionals), so cooks replay the edits.
		auto beginSunDrag = [&](const char* attr) {
			if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetLightAttr ||
				m_pending.cmd.node != sun || m_pending.cmd.lightAttr != attr))
				FinalizePending();
			if (!m_pending.active) {
				m_pending.active = true;
				m_pending.cmd = EditCmd{};
				m_pending.cmd.verb = EditVerb::SetLightAttr;
				m_pending.cmd.node = sun;
				m_pending.cmd.lightAttr = attr;
				ReadNodeLightAttr(sun, attr, m_pending.cmd.beforeVec4,
					m_pending.cmd.beforeNum, m_pending.cmd.beforeFlag,
					m_pending.cmd.beforeText);
			}
		};
		auto endSunDrag = [&](const char* attr) {
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
				m_pending.cmd.verb == EditVerb::SetLightAttr &&
				m_pending.cmd.node == sun &&
				m_pending.cmd.lightAttr == attr)
				FinishDrag();
		};
		glm::vec3 scol = li->color;
		if (ImGui::ColorEdit3("Sun color", &scol.x)) {
			beginSunDrag("color");
			WriteNodeLightAttr(sun, "color", glm::vec4(scol, 1.0f), 0, false, {});
			m_pending.cmd.vec4 = glm::vec4(scol, 1.0f);
		}
		endSunDrag("color");
		float sint = li->intensity;
		if (ImGui::DragFloat("Sun intensity (lux)", &sint, 10.0f, 0.0f,
				2e5f, "%.0f")) {
			beginSunDrag("intensity");
			WriteNodeLightAttr(sun, "intensity", glm::vec4(0), sint, false, {});
			m_pending.cmd.number = sint;
		}
		endSunDrag("intensity");
		bool sstatic = (li->flags & ::SceneLight::kSceneLightFlag_Static) != 0;
		if (ImGui::Checkbox("Static sun (bake-only)", &sstatic)) {
			EditCmd c{};
			c.verb = EditVerb::SetLightAttr;
			c.node = sun;
			c.lightAttr = "static";
			c.afterFlag = sstatic;
			ReadNodeLightAttr(sun, "static", c.beforeVec4, c.beforeNum,
				c.beforeFlag, c.beforeText);
			Do(c);
		}
	}
	// ---- image-based lighting + atmosphere (editor viewport) ----
	DrawEnvPanel();
	// ---- active-viewport orbit camera (moved off the node pages) ----
	// Free-orbit navigation state lives here on the World page; authored
	// camera nodes (outliner [camera]) carry their own lens + pose and
	// are bound per viewport from the viewport chips.
	DrawCameraSection();
}

void EditorApp::SaveEnvToDoc(smvk::tour::Doc& doc) const {
	// Keys carry CLUSTER-space node ids (stable across the deterministic
	// import, like every other tour id). Each node group starts with
	// "env.n.node" so the loader can split groups deterministically.
	auto put = [&](const std::string& key, const std::string& val) {
		doc.editorSettings.emplace_back(key, val);
	};
	auto saveEnv = [&](const std::string& prefix, uint32_t clusterNode,
		const EditorEnv& e) {
		if (clusterNode != UINT32_MAX)
			put(prefix + ".node", std::to_string(clusterNode));
		put(prefix + ".skyColor", Vec3Str(e.skyColor));
		put(prefix + ".skyIntensity", std::to_string(e.skyIntensity));
		put(prefix + ".sunColor", Vec3Str(e.sunColor));
		put(prefix + ".sunAzimuth", std::to_string(e.sunAzimuth));
		put(prefix + ".sunElevation", std::to_string(e.sunElevation));
		put(prefix + ".sunIntensity", std::to_string(e.sunIntensity));
		put(prefix + ".exposure", std::to_string(e.exposure));
		put(prefix + ".fogDensity", std::to_string(e.fogDensity));
		put(prefix + ".fogColor", Vec3Str(e.fogColor));
		put(prefix + ".volumetrics", std::to_string(e.volumetrics));
		put(prefix + ".skyboxUri", e.skyboxUri);
	};
	saveEnv("env.default", UINT32_MAX, m_envDefault);
	// Sort env nodes for deterministic files.
	std::vector<uint32_t> nodes;
	for (const auto& [n, e] : m_envByNode)
		nodes.push_back(n);
	std::sort(nodes.begin(), nodes.end());
	for (uint32_t n : nodes) {
		const int cn = WorldToCluster(n);
		if (cn < 0)
			continue; // unmapped node: keep the file loadable
		saveEnv("env.n", uint32_t(cn), m_envByNode.at(n));
	}
	if (m_envCurrent != UINT32_MAX) {
		const int cn = WorldToCluster(m_envCurrent);
		if (cn >= 0)
			put("env.current", std::to_string(cn));
	}
}

void EditorApp::LoadEnvFromDoc(const smvk::tour::Doc& doc) {
	m_envByNode.clear();
	m_envCurrent = UINT32_MAX;
	EditorEnv def = m_envDefault;
	std::vector<std::pair<uint32_t, EditorEnv>> nodeEnvs;
	bool inNode = false;
	uint32_t nodeCluster = UINT32_MAX;
	EditorEnv cur{};
	uint32_t currentCluster = UINT32_MAX;
	bool haveCurrent = false;

	auto applyField = [](EditorEnv& e, const std::string& f,
			const std::string& v) {
		if (f == "skyColor") e.skyColor = StrVec3(v, e.skyColor);
		else if (f == "sunColor") e.sunColor = StrVec3(v, e.sunColor);
		else if (f == "fogColor") e.fogColor = StrVec3(v, e.fogColor);
		else if (f == "skyIntensity") e.skyIntensity = StrFloat(v, e.skyIntensity);
		else if (f == "sunAzimuth") e.sunAzimuth = StrFloat(v, e.sunAzimuth);
		else if (f == "sunElevation") e.sunElevation = StrFloat(v, e.sunElevation);
		else if (f == "sunIntensity") e.sunIntensity = StrFloat(v, e.sunIntensity);
		else if (f == "exposure") e.exposure = StrFloat(v, e.exposure);
		else if (f == "fogDensity") e.fogDensity = StrFloat(v, e.fogDensity);
		else if (f == "volumetrics") e.volumetrics = StrFloat(v, e.volumetrics);
		else if (f == "skyboxUri") e.skyboxUri = v;
	};

	for (const auto& [key, val] : doc.editorSettings) {
		const size_t dot = key.find_last_of('.');
		const std::string field =
			(dot == std::string::npos) ? key : key.substr(dot + 1);
		if (key.rfind("env.default.", 0) == 0) {
			if (inNode) { // close the pending node group
				nodeEnvs.emplace_back(nodeCluster, cur);
				inNode = false;
			}
			applyField(def, field, val);
		}
		else if (key.rfind("env.n.", 0) == 0) {
			if (field == "node") {
				if (inNode)
					nodeEnvs.emplace_back(nodeCluster, cur);
				inNode = true;
				nodeCluster = uint32_t(StrFloat(val, -1.0f));
				cur = EditorEnv{};
			}
			else if (inNode) {
				applyField(cur, field, val);
			}
		}
		else if (key == "env.current") {
			currentCluster = uint32_t(StrFloat(val, -1.0f));
			haveCurrent = true;
		}
	}
	if (inNode)
		nodeEnvs.emplace_back(nodeCluster, cur);
	m_envDefault = def;
	for (const auto& [cn, env] : nodeEnvs) {
		const uint32_t wn = ClusterToWorld(cn);
		if (wn != UINT32_MAX && wn < m_world->scene.hierarchy.size())
			m_envByNode[wn] = env;
	}
	if (haveCurrent) {
		const uint32_t wn = ClusterToWorld(currentCluster);
		if (wn != UINT32_MAX && m_envByNode.contains(wn))
			m_envCurrent = wn;
	}
	if (!m_envByNode.empty())
		Log("tour: restored " + std::to_string(m_envByNode.size()) +
			" environment node(s) + editor default IBL.");
}

} // namespace editor
