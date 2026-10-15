// Editor environment (IBL) settings: viewport lighting lives in the
// SCENE (smvk::SceneEnv blocks — the scene default, plus per-node
// ENVIRONMENT NODES; skybox + EV/volumetrics authoring; position = node
// translate, size = node scale; convex-volume rendering comes later).
// The current source feeds every viewport straight from the scene.
// Tour persistence uses the same env.* keys as before; the cook resolves
// the active block into the cluster scene (pak v9 section).

#include "editor_app.h"

#include <algorithm>
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
	ResolveSceneEnv();}

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
	m_world->scene.envAuthored = true;
	// Persist the selection (default block = none): cluster resolve
	// merges it first-valid-wins like defaultCameraNode, so the pick
	// survives pak round-trips instead of living only in editor state.
	m_world->scene.activeEnvironment = m_envCurrent;
	// Render settings resolve alongside (the split look is scene-global:
	// no source selection — the default block IS the look).
	m_world->scene.renderSettings = m_world->scene.defaultRenderSettings;
	m_world->scene.hasRenderSettings = true;
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
	// (the GI bake re-run in place, then PakCooker — the pak, the bake
	// assets and this tour's bake references update; see CookToPak).
	if (ImGui::Button("Cook to .pak now##env"))
		CookToPak();
	ImGui::SameLine();
	ImGui::TextDisabled("(full cook: bake update in place + pak)");
	// Update bakes now: the QUICK refresh of exactly what this panel
	// drives — the GI sidecars (GiBaker in place) + the cubemap capture
	// set (PakCooker --quick-bakes: loose sidecars under
	// textures/cubemaps/, no pak write). One click re-captures the pins
	// after a sun/sky edit without the minutes-long product cook; the
	// last pak stays and the fresh loose sidecars override its blobs
	// through the VFS overlay.
	if (ImGui::Button("Update bakes now##env"))
		CookToPak(true);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Quick refresh, no pak write: GiBaker re-runs\n"
			"the GI sidecars in place, then PakCooker --quick-bakes\n"
			"re-captures the cubemap set (authored pins + placement)\n"
			"as loose sidecars under textures/cubemaps/. The last pak\n"
			"stays untouched; its blobs are overridden by the fresh\n"
			"loose files through the VFS overlay.");
	ImGui::SameLine();
	ImGui::TextDisabled("(quick: GI + captures, no pak)");
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
	// ZONE controls (node sources only — the node IS the zone: coverage
	// from its transform, see ZoneCovers; the default block has no zone).
	if (m_envCurrent != UINT32_MAX) {
		EditorEnv& e = CurrentEnv();
		ImGui::DragFloat("Zone priority##envzone", &e.zonePriority, 0.25f,
			0.0f, 100.0f, "%.0f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(
				"Legacy zone priority (the look block's): applies while the\n"
				"node has no volume entry below (the volume priority wins\n"
				"when present). Overlap: higher wins; ties go innermost.");
		// VOLUME settings (Unreal-style bounds + blending over the
		// transform box; see EnvVolume). Created on first edit below
		// (inheriting the look's priority, so legacy zones don't jump);
		// absent = legacy zone behavior.
		auto& scene = m_world->scene;
		const auto vit =
			scene.envVolumeForNode.find(m_envCurrent);
		const bool hasEntry = vit != scene.envVolumeForNode.end();
		EnvVolume tmp = hasEntry ? vit->second : EnvVolume{};
		bool volChanged = false;
		bool priEdited = false;
		volChanged |= ImGui::Checkbox("Volume enabled##envvol",
			&tmp.enabled);
		// Per-face infinity: a set face pushes that box face to
		// -/+infinity (half-spaces and slabs; all six = infinite
		// everywhere, the Unreal unbound equivalent).
		{
			struct FaceBit { const char* label; uint32_t bit; };
			const FaceBit faces[6] = { {"X-##envvol", kEnvInfNegX},
				{"X+##envvol", kEnvInfPosX}, {"Y-##envvol", kEnvInfNegY},
				{"Y+##envvol", kEnvInfPosY}, {"Z-##envvol", kEnvInfNegZ},
				{"Z+##envvol", kEnvInfPosZ} };
			ImGui::TextDisabled("Infinite faces (box face -> +/-inf):");
			for (int i = 0; i < 6; ++i) {
				if (i > 0)
					ImGui::SameLine();
				bool set = (tmp.infMask & faces[i].bit) != 0;
				if (ImGui::Checkbox(faces[i].label, &set)) {
					if (set)
						tmp.infMask |= faces[i].bit;
					else
						tmp.infMask &= ~faces[i].bit;
					volChanged = true;
				}
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					"Y- + box top at y=0 covers y <= 0 (underwater);\n"
					"Y+ + box bottom at y=50 covers y >= 50 (high haze).\n"
					"All six = infinite everywhere. Y-varying zones don't\n"
					"bake into the XZ weight map (cook warns) — the exact\n"
					"3D test still covers them.");
		}
		priEdited = ImGui::DragFloat("Volume priority##envvol",
			&tmp.priority, 0.25f, 0.0f, 100.0f, "%.0f");
		volChanged |= priEdited;
		volChanged |= ImGui::DragFloat("Blend weight##envvol",
			&tmp.blendWeight, 0.01f, 0.0f, 1.0f, "%.2f");
		volChanged |= ImGui::DragFloat("Blend radius (m)##envvol",
			&tmp.blendRadius, 0.1f, 0.0f, 64.0f, "%.1f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(
				"Unreal-style blending: weight scales the zone's map\n"
				"contribution; radius softens the XZ footprint edge at\n"
				"bake. Radius/weight on Y faces apply to the exact 3D\n"
				"test only (map bake is XZ-projected).");
		if (volChanged) {
			if (!hasEntry && !priEdited) {
				// Inherit the look's priority so creating the entry
				// (e.g. to tick an infinite face) doesn't reorder it.
				tmp.priority = e.zonePriority;
			}
			tmp.blendWeight =
				std::clamp(tmp.blendWeight, 0.0f, 1.0f);
			if (!(tmp.blendRadius >= 0.0f))
				tmp.blendRadius = 0.0f;
			scene.envVolumeForNode[m_envCurrent] = tmp;
		}
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
	// Fog quality tiers (see SceneEnv): the combo picks what the App
	// renders — the volumetric raymarch (with the analytic fallback when
	// the pass cannot run), the cheap analytic exp2 fog, or no fog at
	// all. Tiered rows below (the editor viewport previews none of them).
	{
		const char* tiers[3] = { "Off", "Analytic (exp2, fallback)",
			"Volumetric (raymarched GI-lit)" };
		ImGui::Combo("Fog##envfog", &e.fogQuality, tiers, 3);
		if (e.fogQuality == 1) {
			// The analytic tier: the exp2 distance fog (cheap).
			ImGui::SliderFloat("Fog density##envfog", &e.fogDensity,
				0.0f, 1.0f, "%.3f");
			ImGui::ColorEdit3("Fog color##envfog",
				glm::value_ptr(e.fogColor));
		}
		if (e.fogQuality == 2) {
			// The volumetric tier: the SAME settings the App's panel
			// exposes (its pass seeds from these at scene load).
			// "Volumetrics" is the master: 0 = off (the analytic
			// fallback renders), else the scattering intensity.
			ImGui::DragFloat("Volumetric intensity##envvol",
				&e.volumetrics, 0.01f, 0.0f, 4.0f, "%.2f");
			if (e.volumetrics > 0.0f) {
				ImGui::Indent();
				int steps = int(e.volumetricSteps);
				if (ImGui::SliderInt("March steps##envvol", &steps, 4, 128))
					e.volumetricSteps = uint32_t(steps);
				ImGui::SliderFloat("Fog max distance (m)##envvol",
					&e.volumetricMaxDistance, 5.0f, 200.0f, "%.0f");
				ImGui::SliderFloat("Density (1/m)##envvol",
					&e.volumetricDensity, 0.0f, 0.1f, "%.4f");
				ImGui::SliderFloat("Anisotropy g##envvol",
					&e.volumetricAnisotropy, -0.9f, 0.9f);
				ImGui::SliderFloat("Sun strength##envvol",
					&e.volumetricSun, 0.0f, 4.0f);
				ImGui::SliderFloat("GI strength##envvol",
					&e.volumetricGi, 0.0f, 4.0f);
				ImGui::Checkbox("Sample GI probes##envvol",
					&e.volumetricUseGi);
				ImGui::Checkbox("Sun scatter##envvol",
					&e.volumetricUseSun);
				ImGui::SliderFloat("Dither##envvol",
					&e.volumetricJitter, 0.0f, 1.0f);
				ImGui::SliderFloat("Height falloff (1/m)##envvol",
					&e.volumetricHeightFalloff, 0.0f, 1.0f, "%.3f");
				ImGui::SliderFloat("Height base (m)##envvol",
					&e.volumetricHeightBase, -50.0f, 100.0f, "%.1f");
				ImGui::Unindent();
			}
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(
				"Fog quality tiers (authored, saved forward tour -> pak -> "
				"App): volumetric renders the raymarched GI-lit fog (the "
				"App falls back to the analytic tier when the pass cannot "
				"run); analytic is the cheap exp2 distance fog; off "
				"renders\ no\ fog\ at\ all\.\ The\ editor\ viewport\ previews\ "
				"none of them — the App renders them.");
	}
	// Postfx look: SPLIT OUT — see DrawRenderSettingsPanel (the look is
	// a scene-global grade; this panel owns environment only).
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
	// Cheap (one struct copy + flag); keeps the scene live per tweak.
	ResolveSceneEnv();
}

void EditorApp::DrawRenderSettingsPanel() {
	// RENDER SETTINGS (the split postfx look — SceneRenderSettings): the
	// scene-global render grade. NOT environment, NOT zoned — one block
	// (no source combo: env zones blend sky/fog; the look applies to the
	// whole frame). Rows write the default block; the tail resolves into
	// Scene::renderSettings (what the renderer seeds from, once per load
	// — the App's panels/keys stay live afterwards).
	ImGui::Separator();
	ImGui::Text("Render settings (bloom / tonemap / exposure / AO / SSR)");
	ImGui::TextDisabled("Scene-global render grade — saves forward into the "
		"tour (render.* keys), the pak and the App (which seeds its "
		"passes once per load). Zones never touch the look.");
	if (!m_world) {
		ImGui::TextDisabled("No scene — open a scene to author the look.");
		return;
	}
	SceneRenderSettings& r = m_world->scene.defaultRenderSettings;
	// Bloom (complete): enable + tonemap add scale + pre-exposure HDR
	// threshold (the knee derives at 0.25x threshold, as always).
	ImGui::Checkbox("Bloom##render", &r.bloomEnabled);
	if (r.bloomEnabled) {
		ImGui::SliderFloat("Bloom intensity##render",
			&r.bloomIntensity, 0.0f, 2.0f, "%.2f");
		ImGui::DragFloat("Bloom threshold (HDR)##render",
			&r.bloomThreshold, 1.0f, 0.0f, 16000.0f, "%.0f");
	}
	// Tonemap operator.
	{
		const char* modes[4] = { "Reinhard", "ACES", "Uncharted2", "GT7" };
		int tm = int(std::min(r.tonemapMode, 3u));
		if (ImGui::Combo("Tonemap##render", &tm, modes, 4))
			r.tonemapMode = uint32_t(tm);
	}
	// Exposure (histogram meter + eye adaptation). The env panel's
	// "Exposure" row (the viewport brightness multiplier) folds in as
	// EV at the seed — see PushPostfxEnv.
	ImGui::Checkbox("Auto exposure##render", &r.autoExposure);
	if (r.autoExposure) {
		ImGui::SliderFloat("EV bias##render",
			&r.exposureEvBias, -4.0f, 4.0f, "%.2f");
		ImGui::DragFloat("Adapt to dark (/s)##render",
			&r.exposureSpeedToDark, 0.05f, 0.01f, 8.0f, "%.2f");
		ImGui::DragFloat("Adapt to bright (/s)##render",
			&r.exposureSpeedToBright, 0.05f, 0.01f, 8.0f, "%.2f");
		ImGui::DragFloat("Exposure min##render",
			&r.exposureMin, 0.000001f, 0.000001f, 1.0f, "%.6f");
		ImGui::DragFloat("Exposure max##render",
			&r.exposureMax, 0.0005f, 0.00001f, 1.0f, "%.5f");
	}
	else {
		ImGui::DragFloat("Manual exposure##render",
			&r.manualExposure, 0.00005f, 0.00001f, 0.05f, "%.5f");
	}
	// AO (SSGI post chain).
	{
		const char* aoModes[3] = { "None", "SSAO", "SAO" };
		int am = std::clamp(r.aoMode, 0, 2);
		if (ImGui::Combo("AO mode (O)##render", &am, aoModes, 3))
			r.aoMode = am;
		if (r.aoMode != 0) {
			ImGui::SliderFloat("AO radius (m)##render",
				&r.aoRadius, 0.05f, 2.0f, "%.2f");
			ImGui::SliderFloat("AO bias##render",
				&r.aoBias, 0.0f, 0.1f, "%.3f");
			ImGui::SliderFloat("AO intensity##render",
				&r.aoIntensity, 0.0f, 2.0f, "%.2f");
			ImGui::SliderFloat("AO max distance (m)##render",
				&r.aoMaxDistance, 2.0f, 100.0f, "%.0f");
			if (r.aoMode == 1) {
				int s = r.aoSsaoSamples;
				if (ImGui::SliderInt("SSAO samples##render", &s, 1, 32))
					r.aoSsaoSamples = s;
				ImGui::SliderFloat("Blur sharpness##render",
					&r.aoBlurSharpness, 0.0f, 32.0f, "%.1f");
			}
			else {
				int s = r.aoSaoSamples;
				if (ImGui::SliderInt("SAO samples##render", &s, 1, 32))
					r.aoSaoSamples = s;
				int t = r.aoSaoSpiralTurns;
				if (ImGui::SliderInt("SAO spiral turns##render", &t, 1, 16))
					r.aoSaoSpiralTurns = t;
			}
		}
	}
	// SSR + the remaining runtime toggles.
	ImGui::Checkbox("SSR reflections##render", &r.ssrEnabled);
	if (r.ssrEnabled) {
		ImGui::SliderFloat("SSR intensity##render",
			&r.ssrIntensity, 0.0f, 4.0f, "%.2f");
	}
	ImGui::Checkbox("Motion blur (M)##rendermisc", &r.motionBlurEnabled);
	ImGui::Checkbox("Depth of field##rendermisc", &r.dofEnabled);
	{
		const char* aaModes[4] = { "Off", "MSAA 2x", "SMAA 1x", "SMAA T2x" };
		int am = std::clamp(r.aaMode, 0, 3);
		if (ImGui::Combo("Anti-aliasing##rendermisc", &am, aaModes, 4))
			r.aaMode = am;
	}
	ImGui::Checkbox("Cubemap specular IBL##rendermisc", &r.cubemapEnabled);
	if (r.cubemapEnabled) {
		ImGui::SliderFloat("Cubemap intensity##rendermisc",
			&r.cubemapIntensity, 0.0f, 4.0f, "%.2f");
	}
	// Rows write the default block — resolve into the rendered look
	// (the renderer seeds from scene.renderSettings).
	m_world->scene.renderSettings = r;
	m_world->scene.hasRenderSettings = true;
}

void EditorApp::DrawWorldPanel() {
	ImGui::Text("World");
	ImGui::TextDisabled(
		"Not a scene node: global settings. No transform, no "
		"reparent, no delete. The sun + environment (and their "
		"layered zones) live in the Environment section below — "
		"scene-light sun edits are undoable light commands; the "
		"render grade (bloom/tonemap/exposure/AO/SSR) is its own "
		"section; both ride editorSet tour lines.");
	// ---- sun + environment + atmosphere: ONE section ----
	// This used to be a separate "Sun (directional)" block above the
	// environment rows — the two sun halves doubled the display and
	// collided their ImGui IDs ("Sun color" etc.); DrawEnvPanel now
	// carries both sun blocks with ##-tagged rows.
	DrawEnvPanel();
	// ---- render settings (the split postfx look) ----
	// The scene-global grade: bloom on/off + tonemap + exposure + AO +
	// SSR + motion blur + DOF + AA + cubemap IBL. Separated from the
	// environment on purpose (a grade is never zoned).
	DrawRenderSettingsPanel();
	// ---- default camera (typed scene-object field) ----
	// The runtime App spawns at this camera (pose + lens) instead of
	// the generic bounds framing; the editor orbits freely (per-
	// viewport look-through stays a viewport chip, never persisted).
	// NOT editor-exclusive: the setting rides the tour's
	// editorSettings (camera.default), resolves into the cluster
	// scene at cook and lands in the pak (v10) for the App.
	{
		std::vector<uint32_t> cams;
		if (m_world) {
			for (const auto& [n, idx] : m_world->scene.cameraForNode)
				cams.push_back(n);
			std::sort(cams.begin(), cams.end());
		}
		// Dangling selection (camera deleted/detached) reads as none.
		if (m_defaultCameraNode != UINT32_MAX
			&& (!m_world
				|| m_defaultCameraNode
					>= m_world->scene.hierarchy.size()
				|| !m_world->scene.cameraForNode.contains(
					m_defaultCameraNode)))
			m_defaultCameraNode = UINT32_MAX;
		const std::string none = "None (generic framing)";
		std::string cur = (m_defaultCameraNode == UINT32_MAX)
			? none : getNodeName(m_world->scene,
				int(m_defaultCameraNode));
		if (cur.empty())
			cur = "Camera #" + std::to_string(m_defaultCameraNode);
		ImGui::SetNextItemWidth(-60.0f);
		if (ImGui::BeginCombo("Default camera##worlddefcam",
				cur.c_str())) {
			if (ImGui::Selectable(none.c_str(),
					m_defaultCameraNode == UINT32_MAX)) {
				m_defaultCameraNode = UINT32_MAX;
				AutosaveTour();
			}
			for (uint32_t n : cams) {
				const bool sel = n == m_defaultCameraNode;
				std::string label = getNodeName(m_world->scene, int(n));
				if (label.empty())
					label = "Camera #" + std::to_string(n);
				if (ImGui::Selectable(label.c_str(), sel)) {
					m_defaultCameraNode = n;
					AutosaveTour();
				}
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"The camera the cooked App spawns at (pose + lens),\n"
				"replacing the generic bounds framing. Select from the\n"
				"scene's camera objects (Create > Camera); not editor-\n"
				"exclusive — it rides the tour and the pak (v10).");
		}
	}
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
		// Fog quality tiers (see SceneEnv): 0 = off, 1 = analytic (the
		// cheap fallback), 2 = volumetric (the raymarched pass).
		put(prefix + ".fogQuality", std::to_string(e.fogQuality));
		put(prefix + ".fogDensity", std::to_string(e.fogDensity));
		put(prefix + ".fogColor", Vec3Str(e.fogColor));
		// Volumetric fog (the App's pass seeds from these — the same
		// settings its panel exposes; "volumetrics" is the master:
		// 0 = off, else the scattering intensity).
		put(prefix + ".volumetrics", std::to_string(e.volumetrics));
		put(prefix + ".volSteps", std::to_string(e.volumetricSteps));
		put(prefix + ".volMaxDistance", std::to_string(e.volumetricMaxDistance));
		put(prefix + ".volDensity", std::to_string(e.volumetricDensity));
		put(prefix + ".volAnisotropy", std::to_string(e.volumetricAnisotropy));
		put(prefix + ".volSun", std::to_string(e.volumetricSun));
		put(prefix + ".volGi", std::to_string(e.volumetricGi));
		put(prefix + ".volUseGi", e.volumetricUseGi ? "1" : "0");
		put(prefix + ".volUseSun", e.volumetricUseSun ? "1" : "0");
		put(prefix + ".volJitter", std::to_string(e.volumetricJitter));
		put(prefix + ".volHeightFalloff", std::to_string(e.volumetricHeightFalloff));
		put(prefix + ".volHeightBase", std::to_string(e.volumetricHeightBase));
		// Postfx look: SPLIT OUT — env blocks save environment only
		// (the look saves once, scene-level, as render.* below). Zone
		// priority rides the env block (it IS environment).
		put(prefix + ".zonePriority", std::to_string(e.zonePriority));
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
	// Env-volume settings (envvol.n groups, one per node carrying an
	// entry; see EnvVolume). Sorted + cluster-remapped like env.n above.
	{
		std::vector<uint32_t> vnodes;
		if (m_world) {
			for (const auto& [n, v] : m_world->scene.envVolumeForNode)
				vnodes.push_back(n);
		}
		std::sort(vnodes.begin(), vnodes.end());
		for (uint32_t n : vnodes) {
			const int cn = WorldToCluster(n);
			if (cn < 0)
				continue; // unmapped node: keep the file loadable
			const EnvVolume& v = m_world->scene.envVolumeForNode.at(n);
			put("envvol.n.node", std::to_string(uint32_t(cn)));
			put("envvol.n.mask", std::to_string(v.infMask & 63u));
			put("envvol.n.priority", std::to_string(v.priority));
			put("envvol.n.weight", std::to_string(v.blendWeight));
			put("envvol.n.radius", std::to_string(v.blendRadius));
			put("envvol.n.enabled", v.enabled ? "1" : "0");
		}
	}
	// RENDER SETTINGS (the split postfx look — SceneRenderSettings):
	// ONE scene-level block (render.* keys, never per-node). The look
	// publishes with the env (ResolveSceneEnv resolves both).
	{
		const SceneRenderSettings sceneRenderDefault{};
		const SceneRenderSettings& r = m_world
			? m_world->scene.defaultRenderSettings : sceneRenderDefault;
		put("render.bloomEnabled", r.bloomEnabled ? "1" : "0");
		put("render.bloomIntensity", std::to_string(r.bloomIntensity));
		put("render.bloomThreshold", std::to_string(r.bloomThreshold));
		put("render.tonemapMode", std::to_string(r.tonemapMode));
		put("render.autoExposure", r.autoExposure ? "1" : "0");
		put("render.manualExposure", std::to_string(r.manualExposure));
		put("render.exposureEvBias", std::to_string(r.exposureEvBias));
		put("render.exposureSpeedToDark", std::to_string(r.exposureSpeedToDark));
		put("render.exposureSpeedToBright", std::to_string(r.exposureSpeedToBright));
		put("render.exposureMin", std::to_string(r.exposureMin));
		put("render.exposureMax", std::to_string(r.exposureMax));
		put("render.aoMode", std::to_string(r.aoMode));
		put("render.aoRadius", std::to_string(r.aoRadius));
		put("render.aoBias", std::to_string(r.aoBias));
		put("render.aoIntensity", std::to_string(r.aoIntensity));
		put("render.aoMaxDistance", std::to_string(r.aoMaxDistance));
		put("render.aoSsaoSamples", std::to_string(r.aoSsaoSamples));
		put("render.aoBlurSharpness", std::to_string(r.aoBlurSharpness));
		put("render.aoSaoSamples", std::to_string(r.aoSaoSamples));
		put("render.aoSaoSpiralTurns", std::to_string(r.aoSaoSpiralTurns));
		put("render.ssrEnabled", r.ssrEnabled ? "1" : "0");
		put("render.ssrIntensity", std::to_string(r.ssrIntensity));
		put("render.motionBlurEnabled", r.motionBlurEnabled ? "1" : "0");
		put("render.dofEnabled", r.dofEnabled ? "1" : "0");
		put("render.aaMode", std::to_string(r.aaMode));
		put("render.cubemapEnabled", r.cubemapEnabled ? "1" : "0");
		put("render.cubemapIntensity", std::to_string(r.cubemapIntensity));
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
	// Env-volume settings (envvol.n groups, same grouping discipline
	// as env.n above; applied after the look blocks so priority can
	// default from them — mirrors the tour replay).
	struct VolEntry {
		uint32_t node = UINT32_MAX;
		EnvVolume vol{};
	};
	std::vector<VolEntry> nodeVols;
	bool inVol = false;
	uint32_t volCluster = UINT32_MAX;
	EnvVolume volCur{};

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
		// Fog quality tiers (see SceneEnv) + the volumetric knobs (the
		// App's pass seeds from them — "volumetrics" is the master:
		// 0 = off, else the scattering intensity).
		else if (f == "fogQuality") e.fogQuality = int(StrFloat(v, float(e.fogQuality)));
		else if (f == "fogDensity") e.fogDensity = StrFloat(v, e.fogDensity);
		else if (f == "volumetrics") e.volumetrics = StrFloat(v, e.volumetrics);
		else if (f == "volSteps") e.volumetricSteps = uint32_t(StrFloat(v, float(e.volumetricSteps)));
		else if (f == "volMaxDistance") e.volumetricMaxDistance = StrFloat(v, e.volumetricMaxDistance);
		else if (f == "volDensity") e.volumetricDensity = StrFloat(v, e.volumetricDensity);
		else if (f == "volAnisotropy") e.volumetricAnisotropy = StrFloat(v, e.volumetricAnisotropy);
		else if (f == "volSun") e.volumetricSun = StrFloat(v, e.volumetricSun);
		else if (f == "volGi") e.volumetricGi = StrFloat(v, e.volumetricGi);
		else if (f == "volUseGi") e.volumetricUseGi = StrFloat(v, e.volumetricUseGi ? 1.0f : 0.0f) != 0.0f;
		else if (f == "volUseSun") e.volumetricUseSun = StrFloat(v, e.volumetricUseSun ? 1.0f : 0.0f) != 0.0f;
		else if (f == "volJitter") e.volumetricJitter = StrFloat(v, e.volumetricJitter);
		else if (f == "volHeightFalloff") e.volumetricHeightFalloff = StrFloat(v, e.volumetricHeightFalloff);
		else if (f == "volHeightBase") e.volumetricHeightBase = StrFloat(v, e.volumetricHeightBase);
		// Postfx look: SPLIT OUT — env blocks carry environment only;
		// the look rides the scene-level render.* keys (renderField
		// below). Old tours carrying postfx keys on env blocks ignore
		// them (the sanctioned break).
		else if (f == "zonePriority") e.zonePriority = StrFloat(v, e.zonePriority);
		else if (f == "skyboxUri") e.skyboxUri = v;
	};
	// Render-settings keys (render.* — scene-level, the postfx look).
	auto renderField = [](SceneRenderSettings& r, const std::string& f,
			const std::string& v) {
		if (f == "bloomEnabled") r.bloomEnabled = StrFloat(v, r.bloomEnabled ? 1.0f : 0.0f) != 0.0f;
		else if (f == "bloomIntensity") r.bloomIntensity = StrFloat(v, r.bloomIntensity);
		else if (f == "bloomThreshold") r.bloomThreshold = StrFloat(v, r.bloomThreshold);
		else if (f == "tonemapMode") r.tonemapMode = uint32_t(StrFloat(v, float(r.tonemapMode)));
		else if (f == "autoExposure") r.autoExposure = StrFloat(v, r.autoExposure ? 1.0f : 0.0f) != 0.0f;
		else if (f == "manualExposure") r.manualExposure = StrFloat(v, r.manualExposure);
		else if (f == "exposureEvBias") r.exposureEvBias = StrFloat(v, r.exposureEvBias);
		else if (f == "exposureSpeedToDark") r.exposureSpeedToDark = StrFloat(v, r.exposureSpeedToDark);
		else if (f == "exposureSpeedToBright") r.exposureSpeedToBright = StrFloat(v, r.exposureSpeedToBright);
		else if (f == "exposureMin") r.exposureMin = StrFloat(v, r.exposureMin);
		else if (f == "exposureMax") r.exposureMax = StrFloat(v, r.exposureMax);
		else if (f == "aoMode") r.aoMode = int(StrFloat(v, float(r.aoMode)));
		else if (f == "aoRadius") r.aoRadius = StrFloat(v, r.aoRadius);
		else if (f == "aoBias") r.aoBias = StrFloat(v, r.aoBias);
		else if (f == "aoIntensity") r.aoIntensity = StrFloat(v, r.aoIntensity);
		else if (f == "aoMaxDistance") r.aoMaxDistance = StrFloat(v, r.aoMaxDistance);
		else if (f == "aoSsaoSamples") r.aoSsaoSamples = int(StrFloat(v, float(r.aoSsaoSamples)));
		else if (f == "aoBlurSharpness") r.aoBlurSharpness = StrFloat(v, r.aoBlurSharpness);
		else if (f == "aoSaoSamples") r.aoSaoSamples = int(StrFloat(v, float(r.aoSaoSamples)));
		else if (f == "aoSaoSpiralTurns") r.aoSaoSpiralTurns = int(StrFloat(v, float(r.aoSaoSpiralTurns)));
		else if (f == "ssrEnabled") r.ssrEnabled = StrFloat(v, r.ssrEnabled ? 1.0f : 0.0f) != 0.0f;
		else if (f == "ssrIntensity") r.ssrIntensity = StrFloat(v, r.ssrIntensity);
		else if (f == "motionBlurEnabled") r.motionBlurEnabled = StrFloat(v, r.motionBlurEnabled ? 1.0f : 0.0f) != 0.0f;
		else if (f == "dofEnabled") r.dofEnabled = StrFloat(v, r.dofEnabled ? 1.0f : 0.0f) != 0.0f;
		else if (f == "aaMode") r.aaMode = int(StrFloat(v, float(r.aaMode)));
		else if (f == "cubemapEnabled") r.cubemapEnabled = StrFloat(v, r.cubemapEnabled ? 1.0f : 0.0f) != 0.0f;
		else if (f == "cubemapIntensity") r.cubemapIntensity = StrFloat(v, r.cubemapIntensity);
	};
	SceneRenderSettings render = scene.defaultRenderSettings;
	bool haveRender = false;

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
		else if (key.rfind("envvol.n.", 0) == 0) {
			if (field == "node") {
				if (inVol)
					nodeVols.push_back({ volCluster, volCur });
				inVol = true;
				volCluster = uint32_t(StrFloat(val, -1.0f));
				volCur = EnvVolume{};
			}
			else if (inVol) {
				if (field == "mask") {
					const int m = int(StrFloat(val, 0.0f));
					volCur.infMask = uint32_t(m < 0 ? 0 : (m > 63 ? 63 : m));
				}
				else if (field == "priority")
					volCur.priority = StrFloat(val, volCur.priority);
				else if (field == "weight")
					volCur.blendWeight = StrFloat(val, volCur.blendWeight);
				else if (field == "radius")
					volCur.blendRadius = StrFloat(val, volCur.blendRadius);
				else if (field == "enabled")
					volCur.enabled =
						StrFloat(val, volCur.enabled ? 1.0f : 0.0f) != 0.0f;
			}
		}
		else if (key.rfind("render.", 0) == 0) {
			// Scene-level render settings (the postfx look).
			haveRender = true;
			renderField(render, field, val);
		}
	}
	if (inNode)
		nodeEnvs.emplace_back(nodeCluster, cur);
	if (inVol)
		nodeVols.push_back({ volCluster, volCur });
	scene.defaultEnvironment = def;
	if (haveRender) {
		scene.defaultRenderSettings = render;
		scene.hasRenderSettings = true;
	}
	for (const auto& [cn, env] : nodeEnvs) {
		const uint32_t wn = ClusterToWorld(cn);
		if (wn != UINT32_MAX && wn < scene.hierarchy.size())
			scene.envForNode[wn] = env;
	}
	for (const auto& entry : nodeVols) {
		const uint32_t wn = ClusterToWorld(entry.node);
		if (wn == UINT32_MAX || wn >= scene.hierarchy.size())
			continue; // dangling volume: drop (never fatal)
		EnvVolume vol = entry.vol;
		vol.blendWeight = std::clamp(vol.blendWeight, 0.0f, 1.0f);
		if (!(vol.blendRadius >= 0.0f))
			vol.blendRadius = 0.0f;
		scene.envVolumeForNode[wn] = vol;
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

void EditorApp::SaveDefaultCameraToDoc(smvk::tour::Doc& doc) const {
	// Typed scene-object reference field (see tour.h's editorSettings
	// note): "ref <k>" (the k-th createCamera in the doc — the same
	// family ref space setCamAttr -ref uses, replay-stable across
	// imports) or "node <clusterId>" (an imported glTF camera).
	// Absent when unset/dangling (the world panel's combo keeps the
	// member valid; the cook warns + drops unresolvable values).
	if (m_defaultCameraNode == UINT32_MAX || !m_world)
		return;
	if (m_defaultCameraNode >= m_world->scene.hierarchy.size()
		|| !m_world->scene.cameraForNode.contains(m_defaultCameraNode))
		return; // dangling (deleted/detached camera): save as none
	const int k = CreatedCameraIndex(m_defaultCameraNode);
	std::string val;
	if (k >= 0) {
		val = "ref " + std::to_string(k);
	}
	else {
		const int cn = WorldToCluster(m_defaultCameraNode);
		if (cn < 0)
			return; // neither ref nor node addressable: drop
		val = "node " + std::to_string(uint32_t(cn));
	}
	doc.editorSettings.emplace_back("camera.default", std::move(val));
}

void EditorApp::LoadDefaultCameraFromDoc(const smvk::tour::Doc& doc) {
	// Resolve AFTER the command replay (the caller ordering guarantees
	// it: a "ref k" indexes m_tourCreatedCameraNodes, rebuilt by the
	// replayed createCamera commands). Dangling values warn and leave
	// the member at none — never fatal, like the env blocks.
	m_defaultCameraNode = UINT32_MAX;
	if (!m_world)
		return;
	Scene& scene = m_world->scene;
	for (const auto& [key, val] : doc.editorSettings) {
		if (key != "camera.default" || val.empty())
			continue;
		uint32_t target = UINT32_MAX;
		if (val.rfind("ref ", 0) == 0) {
			uint32_t k = 0;
			if (!sscanf(val.c_str() + 4, "%u", &k)
				|| k >= m_tourCreatedCameraNodes.size()) {
				Log("tour: camera.default ref out of range: '" + val
					+ "' (none stands).");
				continue;
			}
			target = m_tourCreatedCameraNodes[k];
		}
		else {
			// "node <clusterId>" (a bare number reads as one too — the
			// env.n.node convention).
			const char* num =
				val.rfind("node ", 0) == 0 ? val.c_str() + 5 : val.c_str();
			uint32_t cn = 0;
			if (!sscanf(num, "%u", &cn)) {
				Log("tour: camera.default unparsable: '" + val
					+ "' (none stands).");
				continue;
			}
			target = ClusterToWorld(cn);
		}
		if (target == UINT32_MAX || target >= scene.hierarchy.size()
			|| !scene.cameraForNode.contains(target)) {
			Log("tour: camera.default target does not resolve: '" + val
				+ "' (none stands).");
			continue;
		}
		m_defaultCameraNode = target;
	}
}

} // namespace editor
