// Editor environment (IBL) settings: viewport lighting lives in the
// SCENE (smvk::SceneEnv blocks — the scene default, plus per-node
// ENVIRONMENT NODES; skybox + EV/volumetrics authoring; position = node
// translate, size = node scale; convex-volume rendering comes later).
// The current source feeds every viewport straight from the scene.
// Tour persistence uses the same env.* keys as before; the cook resolves
// the active block into the cluster scene (pak v9 section).

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
	auto& table = m_world->scene.envForNode;
	if (!table.contains(node)) {
		table[node] = CurrentEnv(); // start from the current source
		Log("Node " + std::to_string(node) + " marked as Environment.");
	}
	ResolveSceneEnv();
}

void EditorApp::UnmarkEnvNode(uint32_t node) {
	if (!m_world)
		return;
	if (m_world->scene.envForNode.erase(node) > 0 && m_envCurrent == node)
		m_envCurrent = UINT32_MAX; // fall back to the editor default
	ResolveSceneEnv();
}

void EditorApp::ResolveSceneEnv() {
	// Resolve the current selection (default block or selected node
	// block) into scene.environment — the rendered look. The renderer
	// and the legacy frame both consume the scene; this is the only
	// writer besides the tour/cook loaders.
	if (!m_world)
		return;
	const SceneEnv* src = &m_world->scene.defaultEnvironment;
	if (m_envCurrent != UINT32_MAX) {
		auto it = m_world->scene.envForNode.find(m_envCurrent);
		if (it != m_world->scene.envForNode.end())
			src = &it->second;
		else
			m_envCurrent = UINT32_MAX;
	}
	m_world->scene.environment = *src;
	m_world->scene.hasEnvironment = true;
}

void EditorApp::DrawEnvPanel() {
	// ONE Environment section, ONE sun (the earlier layout drew two sun
	// sub-blocks with duplicate labels — doubled display + ImGui ID
	// collisions; the ##-tagged rows below can never collide again).
	// The ENV BLOCK owns the sun and sky (LUX-NATIVE: sun 10000 / sky
	// 600 defaults — the units the runtime and every bake use, no
	// conversions): one authored value drives the editor viewport, the
	// GI/lightmap/cubemap bakes, the synthesized pak directional and the
	// App. A scene DIRECTIONAL light node (content) still wins when one
	// exists — the sub-block below edits it (SetLightAttr commands).
	ImGui::Separator();
	ImGui::Text("Environment (scene definition)");
	ImGui::TextDisabled(
		"Scene-attached look — every view renders it straight from the "
		"scene. The sun/sky rows are LUX-NATIVE and drive everything: "
		"the viewport, every bake, the pak directional and the App.");
	// Cook now: the full cook flow from the section that owns its inputs
	// (GiBaker from 0, then PakCooker — the pak, the bake assets and
	// this tour's bake references update; see CookToPak).
	if (ImGui::Button("Cook to .pak now##env"))
		CookToPak();
	ImGui::SameLine();
	ImGui::TextDisabled("(full cook: GI from 0 + pak)");
	if (!m_world) {
		ImGui::TextDisabled("No scene — open a scene to author the look.");
		return;
	}
	// Source combo: editor default + every env node.
	std::string curLabel = "Editor default";
	if (m_envCurrent != UINT32_MAX)
		curLabel = "Node " + std::to_string(m_envCurrent);
	if (ImGui::BeginCombo("Source##env", curLabel.c_str())) {
		if (ImGui::Selectable("Editor default", m_envCurrent == UINT32_MAX))
			m_envCurrent = UINT32_MAX;
		for (const auto& [node, env] : m_world->scene.envForNode) {
			const std::string label = "Node " + std::to_string(node);
			if (ImGui::Selectable(label.c_str(), m_envCurrent == node))
				m_envCurrent = node;
		}
		ImGui::EndCombo();
	}
	// ---- sun: scene light (content — only when a directional node exists) ----
	ImGui::Separator();
	ImGui::Text("Sun (scene light)");
	const uint32_t sun = FindSunNode();
	if (sun == UINT32_MAX) {
		// No scene directional: the ENVIRONMENT sun below IS the sun —
		// the cooker synthesizes the pak's WorldSun directional from it,
		// every bake reads it (EnvForBake) and the App renders it. One
		// authored sun, no duplicates.
		ImGui::TextDisabled("No scene directional light: the environment "
			"sun below drives the pak, every bake and the App (the "
			"cooker synthesizes its WorldSun from it). Import a KHR "
			"punctual directional for a per-node scene light instead.");
	}
	else if (glm::vec3 aimWs(0.0f);
		const ::SceneLight* li = FindNodeLight(sun, &aimWs)) {
		ImGui::Text("'%s' (#%u)", li->name.c_str(), sun);
		ImGui::SameLine();
		if (ImGui::Button("Select node##sun"))
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
		if (ImGui::ColorEdit3("Sun color##sun", &scol.x)) {
			beginSunDrag("color");
			WriteNodeLightAttr(sun, "color", glm::vec4(scol, 1.0f), 0, false, {});
			m_pending.cmd.vec4 = glm::vec4(scol, 1.0f);
		}
		endSunDrag("color");
		float sint = li->intensity;
		if (ImGui::DragFloat("Sun intensity (lux)##sun", &sint, 10.0f,
				0.0f, 2e5f, "%.0f")) {
			beginSunDrag("intensity");
			WriteNodeLightAttr(sun, "intensity", glm::vec4(0), sint, false, {});
			m_pending.cmd.number = sint;
		}
		endSunDrag("intensity");
		bool sstatic = (li->flags & ::SceneLight::kSceneLightFlag_Static) != 0;
		if (ImGui::Checkbox("Static sun (bake-only)##sun", &sstatic)) {
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
	// ---- sun (environment — THE sun, lux-native) ----
	ImGui::Separator();
	ImGui::Text("Sun (environment)");
	EditorEnv& e = CurrentEnv();
	ImGui::SliderFloat("Sun azimuth##envsun", &e.sunAzimuth, 0.0f, 360.0f);
	ImGui::SliderFloat("Sun elevation##envsun", &e.sunElevation, 0.0f, 90.0f);
	ImGui::DragFloat("Sun intensity (lux)##envsun", &e.sunIntensity, 10.0f,
		0.0f, 2e5f, "%.0f");
	ImGui::ColorEdit3("Sun color##envsun", glm::value_ptr(e.sunColor));
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(
			"THE sun, in lux (no conversions): drives the editor viewport "
			"light + sky disc, every bake (GI/lightmap/cubemap) and the "
			"App — the cooker synthesizes the pak's WorldSun directional "
			"from it. The LIGHTING-mode sun nudges edit these.");
	// ---- sky & atmosphere ----
	ImGui::Separator();
	ImGui::Text("Sky & atmosphere");
	ImGui::ColorEdit3("Sky color", glm::value_ptr(e.skyColor));
	ImGui::DragFloat("Sky intensity (lux)##envsky", &e.skyIntensity, 5.0f,
		0.0f, 10000.0f, "%.0f");
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
	// Every slider/button above writes the scene-backed source (default
	// block or selected node block) — resolve into the rendered look.
	// Cheap (one struct copy + flag); keeps the viewport live per tweak.
	ResolveSceneEnv();
}

void EditorApp::DrawWorldPanel() {
	ImGui::Text("World");
	ImGui::TextDisabled(
		"Not a scene node: global render settings. No transform, no "
		"reparent, no delete. The sun + environment live in ONE "
		"section below — scene-light sun edits are undoable light "
		"commands; the look/sky ride editorSet tour lines.");
	// ---- sun + environment + atmosphere: ONE section ----
	// This used to be a separate "Sun (directional)" block above the
	// environment rows — the two sun halves doubled the display and
	// collided their ImGui IDs ("Sun color" etc.); DrawEnvPanel now
	// carries both sun blocks with ##-tagged rows.
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
	// Storage is the SCENE (default block + per-node blocks + the
	// selection); the key layout is unchanged.
	auto put = [&](const std::string& key, const std::string& val) {
		doc.editorSettings.emplace_back(key, val);
	};
	auto saveEnv = [&](const std::string& prefix, uint32_t clusterNode,
		const SceneEnv& e) {
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
	// Sceneless save (shouldn't happen — tours save scenes): struct
	// defaults stand in for the scene blocks.
	const SceneEnv sceneDefault{};
	const SceneEnv& def = m_world
		? m_world->scene.defaultEnvironment : sceneDefault;
	saveEnv("env.default", UINT32_MAX, def);
	// Sort env nodes for deterministic files.
	std::vector<uint32_t> nodes;
	if (m_world) {
		for (const auto& [n, e] : m_world->scene.envForNode)
			nodes.push_back(n);
	}
	std::sort(nodes.begin(), nodes.end());
	for (uint32_t n : nodes) {
		const int cn = WorldToCluster(n);
		if (cn < 0)
			continue; // unmapped node: keep the file loadable
		saveEnv("env.n", uint32_t(cn), m_world->scene.envForNode.at(n));
	}
	if (m_envCurrent != UINT32_MAX) {
		const int cn = WorldToCluster(m_envCurrent);
		if (cn >= 0)
			put("env.current", std::to_string(cn));
	}
	// Bake references (the .tour carries its bake — see
	// RecordBakeAssetsFromCook): the GiCook manifest asset + the cubemap
	// capture-set manifest a cook minted/updated for this scene. Written
	// on EVERY incremental save so the .tour and the assetdb always
	// agree on which bake belongs to which definition.
	if (!m_bakeGiCookGuid.empty())
		put("bake.gicook.guid", m_bakeGiCookGuid);
	if (!m_bakeCubemapGuid.empty())
		put("bake.cubemap.guid", m_bakeCubemapGuid);
}

void EditorApp::LoadEnvFromDoc(const smvk::tour::Doc& doc) {
	// Tour env applies into the LIVE scene (cluster-space ids remap to
	// world ids like every other tour id). Runs post-build (the replay
	// needs ClusterToWorld + the hierarchy); without a world there is
	// no scene to seed.
	if (!m_world)
		return;
	auto& scene = m_world->scene;
	scene.envForNode.clear();
	m_envCurrent = UINT32_MAX;
	EditorEnv def = scene.defaultEnvironment;
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
		// Bake references (see SaveEnvToDoc): restored into the members
		// so the next save carries them forward.
		if (key == "bake.gicook.guid")
			m_bakeGiCookGuid = val;
		else if (key == "bake.cubemap.guid")
			m_bakeCubemapGuid = val;
		else if (key.rfind("env.default.", 0) == 0) {
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
	scene.defaultEnvironment = def;
	for (const auto& [cn, env] : nodeEnvs) {
		const uint32_t wn = ClusterToWorld(cn);
		if (wn != UINT32_MAX && wn < scene.hierarchy.size())
			scene.envForNode[wn] = env;
	}
	if (haveCurrent) {
		const uint32_t wn = ClusterToWorld(currentCluster);
		if (wn != UINT32_MAX && scene.envForNode.contains(wn))
			m_envCurrent = wn;
	}
	// Publish the loaded selection into the rendered look.
	ResolveSceneEnv();
	if (!scene.envForNode.empty())
		Log("tour: restored " + std::to_string(scene.envForNode.size()) +
			" environment node(s) + editor default IBL.");
}

} // namespace editor
