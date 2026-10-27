// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Editor environment (IBL) settings: viewport lighting lives in the
// SCENE (smvk::SceneEnv blocks — the scene default, plus per-node
// ENVIRONMENT NODES; skybox + EV/volumetrics authoring; position = node
// translate, size = node scale; convex-volume rendering comes later).
// The current source feeds every viewport straight from the scene.
// Tour persistence uses the same env.* keys as before; the cook resolves
// the active block into the cluster scene (pak v9 section).

#include "editor_app.h"
#include "editor_attr_ui.h" // attribute widgets for numeric, enum and color fields

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
		// One environment section. Scene-light and environment sun controls
		// retain the same visible names but use separate ImGui ID scopes so
		// labels such as "Sun color" and "Sun intensity" cannot collide.
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
		// Cook flows live in the BottomDock Cook tab (File > Export...),
		// not here: this panel owns the look inputs, the tab owns the
		// product step (full cook + quick bakes + cubemap refresh).
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
		// (The scene-level "Enable environment zones" master switch is
		// RETIRED with the always-on rework: zones ALWAYS run — the scene's
		// DEFAULT environment packs as the infinite base zone (all six faces
		// unbounded) and the node zones override it by priority/innermost.
		// The env.zonesEnabled tour/pak key stays on the wire, forced true.)
		// ZONE controls (node sources only — the node IS the zone: coverage
		// from its transform, see ZoneCovers; the default block's zone is the
		// built-in infinite one).
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
				const FaceBit faces[6] = { { "X-##envvol", kEnvInfNegX },
					{ "X+##envvol", kEnvInfPosX },{ "Y-##envvol", kEnvInfNegY },
					{ "Y+##envvol", kEnvInfPosY },{ "Z-##envvol", kEnvInfNegZ },
					{ "Z+##envvol", kEnvInfPosZ } };
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
						"All six = infinite everywhere. Coverage is analytic\n"
						"3D, so Y-varying zones work exactly.");
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
					"Unreal-style blending: weight scales the whole zone;\n"
					"radius softens the edge OUTSIDE the box (3D distance).\n"
					"Both apply per pixel in the terrain + volumetric fog.");
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
		ImGui::PushID("scene_sun");
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
			const ::SceneLight * li = FindNodeLight(sun, &aimWs)) {
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
			{
				float col4[4] = { scol.x, scol.y, scol.z, 1.0f };
				if (attr::Color("Sun color", col4,
					"The scene directional's color (the sub-block edits the "
					"LIGHT node; the env block below owns the bake sun)")) {
					beginSunDrag("color");
					const glm::vec3 c3(col4[0], col4[1], col4[2]);
					WriteNodeLightAttr(sun, "color", glm::vec4(c3, 1.0f), 0, false, {});
					m_pending.cmd.vec4 = glm::vec4(c3, 1.0f);
				}
				endSunDrag("color");
			}
			float sint = li->intensity;
			if (attr::Float("Sun intensity", &sint, 10.0f, 0.0f, 2e5f, "%.0f",
				"The scene directional's intensity (lux)")) {
				beginSunDrag("intensity");
				WriteNodeLightAttr(sun, "intensity", glm::vec4(0), sint, false, {});
				m_pending.cmd.number = sint;
			}
			endSunDrag("intensity");
			bool sstatic = (li->flags & ::SceneLight::kSceneLightFlag_Static) != 0;
			// Checkbox edits the proposed new value; persist it as an undoable
			// light command rather than mutating the scene light directly.
			if (ImGui::Checkbox("Static##scene_sun_static", &sstatic)) {
				EditCmd c{};
				c.verb = EditVerb::SetLightAttr;
				c.node = sun;
				c.lightAttr = "static";
				c.afterFlag = sstatic ? 1 : 0; // checkbox now contains NEW state
				ReadNodeLightAttr(sun, "static", c.beforeVec4, c.beforeNum,
					c.beforeFlag, c.beforeText);
				Do(c);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("STATIC: bake-only (excluded from runtime Forward+)");
		}
		ImGui::PopID();

		// ---- sun (environment — THE sun, lux-native) ----------------
		ImGui::Separator();
		ImGui::Text("Sun (environment)");
		ImGui::PushID("environment_sun");
		EditorEnv& e = CurrentEnv();
		attr::Slider("Sun azimuth", &e.sunAzimuth, 0.0f, 360.0f, "%.0f",
			"The sun's compass direction (deg)");
		attr::Slider("Sun elevation", &e.sunElevation, 0.0f, 90.0f, "%.0f",
			"The sun's height above horizon (deg)");
		attr::Float("Sun intensity", &e.sunIntensity, 10.0f, 0.0f, 2e5f, "%.0f",
			"THE sun, in lux (no conversions): drives the editor viewport "
			"light + sky disc, every bake (GI/lightmap/cubemap) and the App "
			"— the cooker synthesizes the pak's WorldSun directional from it. "
			"The LIGHTING-mode sun nudges edit these.");
		{
			float col4[4] = { e.sunColor.x, e.sunColor.y, e.sunColor.z, 1.0f };
			if (attr::Color("Sun color", col4,
				"THE sun's color (drives every bake + the pak directional)"))
				e.sunColor = glm::vec3(col4[0], col4[1], col4[2]);
		}
		ImGui::PopID();
		// ---- sky & atmosphere ----
		ImGui::Separator();
		ImGui::Text("Sky & atmosphere");
		{
			float col4[4] = { e.skyColor.x, e.skyColor.y, e.skyColor.z, 1.0f };
			if (attr::Color("Sky color", col4,
				"The zenith sky color (drives the ambient + every bake)"))
				e.skyColor = glm::vec3(col4[0], col4[1], col4[2]);
		}
		attr::Float("Sky intensity", &e.skyIntensity, 5.0f, 0.0f, 10000.0f,
			"%.0f", "The sky ambient scale (lux)");
		attr::Slider("Exposure", &e.exposure, 0.1f, 4.0f, "%.2f",
			"The viewport brightness multiplier (folds in as EV at the seed)");
		// Fog quality selects the active renderer tier; every tier's settings
		// remain visible and editable even when fog is Off or another tier is active.
		{
			attr::Enum("Fog", &e.fogQuality,
				"Off\0Analytic (exp2, fallback)\0Volumetric (raymarched GI-lit)\0",
				"Select the rendered fog tier; all fog parameters remain editable.");
			ImGui::TextDisabled("Analytic fog parameters");
			attr::Slider("Fog density", &e.fogDensity, 0.0f, 1.0f, "%.3f",
				"The exp2 distance fog density");
			float fc4[4] = { e.fogColor.x, e.fogColor.y, e.fogColor.z, 1.0f };
			if (attr::Color("Fog color", fc4, "The analytic fog tint"))
				e.fogColor = glm::vec3(fc4[0], fc4[1], fc4[2]);

			ImGui::TextDisabled("Volumetric fog parameters");
			attr::Float("Volumetrics", &e.volumetrics, 0.01f, 0.0f, 4.0f,
				"%.2f", "The scattering intensity (0 = analytic fallback)");
			ImGui::Indent();
			int steps = int(e.volumetricSteps);
			if (attr::Int("March steps", &steps, 4, 128,
				"The raymarch step count (quality vs cost)"))
				e.volumetricSteps = uint32_t(steps);
			attr::Slider("Fog max distance", &e.volumetricMaxDistance,
				5.0f, 200.0f, "%.0f", "The march ray clamp (m)");
			attr::Slider("Density", &e.volumetricDensity, 0.0f, 0.1f,
				"%.4f", "The extinction (1/m) — heavy densities make "
				"the output pure in-scatter");
			attr::Slider("Anisotropy g", &e.volumetricAnisotropy,
				-0.9f, 0.9f, "%.2f", "The HG phase g (-1 back .. 1 "
				"forward scatter)");
			attr::Slider("Sun strength", &e.volumetricSun, 0.0f, 4.0f,
				"%.2f", "The sun single-scatter strength");
			attr::Slider("GI strength", &e.volumetricGi, 0.0f, 4.0f,
				"%.2f", "The probe ambient scatter strength");
			ImGui::Checkbox("Sample GI probes##vol_gi", &e.volumetricUseGi);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("On: sample SG-GI probes; off: sky IBL");
			ImGui::Checkbox("Sun scatter##vol_sun", &e.volumetricUseSun);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Enable analytic sun scattering");
			attr::Slider("Dither", &e.volumetricJitter, 0.0f, 1.0f,
				"%.2f", "The IGN dither (banding guard)");
			attr::Slider("Height falloff", &e.volumetricHeightFalloff,
				0.0f, 1.0f, "%.3f", "The vertical density falloff (1/m; "
				"0 = uniform)");
			attr::Slider("Height base", &e.volumetricHeightBase,
				-50.0f, 100.0f, "%.1f", "The falloff reference height (m)");
			ImGui::Unindent();
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
		// RENDER SETTINGS: the renderer options (top — GI/shadow knobs,
		// live on the WYSIWYG view per edit, session-only) + the postfx
		// look (SceneRenderSettings — the scene-global grade the tour/
		// pak/App carry). Numeric/enum rows use the attribute stubs;
		// switches use native ImGui checkboxes so changes write the
		// backing bool exactly once. Both halves always show all
		// parameter rows regardless of whether the corresponding effect
		// is active; users can edit inactive presets.
		ImGui::Separator();
		ImGui::Text("Render settings (GI / shadows / bloom / tonemap / exposure / AO / SSR)");
		{
			engine::RenderQualityConfig& q = m_renderQuality;
			ImGui::PushID("render_quality");
			ImGui::TextDisabled("Global illumination");
			int gi = std::clamp(q.giMode, 0, 5);
			if (attr::Enum("GI mode", &gi,
				"All (baked GI; sky fallback)\0Lightmaps only\0"
				"Probes only (grids + volume)\0Sky IBL only\0"
				"Off (direct only)\0Baked (no sky fallback)\0",
				"The ambient source (the App's N cycle). Single-source "
				"modes degrade at load when their bake is missing; "
				"Baked isolates the baked field (uncovered pixels go "
				"direct-only, no analytic sky anywhere in the ambient)."))
				q.giMode = gi;
			attr::Slider("GI intensity", &q.giIntensity, 0.0f, 2.0f, "%.2f",
				"The baked-GI + sky-IBL ambient scale (0 = direct sun "
				"only, 1 = authored, 2 = double)");
			ImGui::Checkbox("Lightmaps##gi_lm", &q.lightmapsEnabled);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Baked 2D lightmap branch (static "
					"geometry; needs a --lightmap bake)");
			ImGui::Checkbox("Regular probe volumes##gi_reg", &q.regProbesEnabled);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Baked regular probe grids "
					"(dynamic-object GI)");
			ImGui::Checkbox("Convex GI volumes##gi_cv", &q.giVolumesEnabled);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Confined-space dense probe volumes, "
					"composited over the octree field by priority");
			ImGui::Checkbox("Probe pre-mix##gi_premix", &q.probePremixEnabled);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("GPU pre-mixed probe blend (one cubemap "
					"fetch per pixel); off = the per-pixel probe walk");
			ImGui::TextDisabled("Shadows");
			ImGui::Checkbox("EVSM moment shadows##evsm", &q.evsmEnabled);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("EVSM moment shadow maps (soft shadows; "
					"falls back to PCF when unsupported)");
			attr::Slider("EVSM softness", &q.evsmSoftness, 0.0f, 4.0f, "%.2f",
				"Scales shadow softness relative to the light's "
				"physical size (0 = hard).");
			ImGui::PopID();
			// Live push per edit (plain member writes — the same pattern
			// as the look's PushPostfxEnv at the tail; session-only, and
			// LaunchWysiwyg re-applies after every attach so relaunches
			// keep the session's choices).
			if (m_wysiwyg && m_wysiwygValid)
				m_wysiwyg->PushRenderQuality(q);
		}
		if (!m_world) {
			ImGui::TextDisabled("No scene — open a scene to author the look.");
			return;
		}
		SceneRenderSettings& r = m_world->scene.defaultRenderSettings;
		// Keep Render controls independent from same-named widgets in other tabs.
		ImGui::PushID("render_settings_panel");
		{
			// Native ImGui controls here avoid relying on the attr::Bool stub's
			// click/toggle semantics. The section is ID-scoped, and the ## suffixes
			// keep the visible labels while guaranteeing unique control IDs.
			ImGui::Separator();
			ImGui::TextDisabled("Post-processing look");
			ImGui::PushID("bloom");
			ImGui::Checkbox("Bloom##enabled", &r.bloomEnabled);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Enable the HDR bright-pass bloom effect");
			// Keep the editable parameters visible even when Bloom is disabled.
			r.bloomIntensity = std::clamp(r.bloomIntensity, 0.0f, 2.0f);
			r.bloomThreshold = std::clamp(r.bloomThreshold, 0.0f, 16000.0f);
			ImGui::SliderFloat("Bloom intensity##intensity", &r.bloomIntensity,
				0.0f, 2.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Bloom contribution (0..2)");
			ImGui::DragFloat("Bloom threshold##threshold", &r.bloomThreshold,
				0.05f, 0.0f, 16000.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Pre-exposure HDR bright-pass threshold. "
					"Ctrl-click to enter a precise value.");
			ImGui::PopID();
			int tm = int(std::min(r.tonemapMode, 3u));
			if (attr::Enum("Tonemap", &tm,
				"Reinhard\0ACES\0Uncharted2\0GT7\0",
				"The HDR->LDR operator"))
				r.tonemapMode = uint32_t(tm);
			ImGui::Checkbox("Auto exposure##enabled", &r.autoExposure);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Auto: histogram metering and eye adaptation. "
					"Off: use manual exposure.");
			// Both auto and manual parameters are always visible and editable.
			ImGui::TextDisabled("Auto exposure parameters");
			attr::Slider("EV bias", &r.exposureEvBias, -4.0f, 4.0f, "%.2f",
				"The metered target's exposure bias (stops)");
			attr::Float("Adapt to dark", &r.exposureSpeedToDark, 0.05f,
				0.01f, 8.0f, "%.2f", "Adaptation speed toward dark (/s)");
			attr::Float("Adapt to bright", &r.exposureSpeedToBright, 0.05f,
				0.01f, 8.0f, "%.2f", "Adaptation speed toward bright (/s)");
			attr::Float("Exposure min", &r.exposureMin, 0.000001f,
				0.000001f, 1.0f, "%.6f", "The adaptation clamp floor");
			attr::Float("Exposure max", &r.exposureMax, 0.0005f, 0.00001f,
				1.0f, "%.5f", "The adaptation clamp ceiling");
			ImGui::TextDisabled("Manual exposure parameter");
			attr::Float("Manual exposure", &r.manualExposure, 0.00005f,
				0.00001f, 0.05f, "%.5f", "The fixed exposure multiplier");
			int am = std::clamp(r.aoMode, 0, 2);
			if (attr::Enum("AO mode", &am, "None\0SSAO\0SAO\0",
				"Screen-space ambient occlusion (O cycles at runtime)"))
				r.aoMode = am;
			// None/SSAO/SAO only selects the active algorithm.
			// All AO settings can be configured regardless of the selected mode.
			attr::Slider("AO radius", &r.aoRadius, 0.05f, 2.0f, "%.2f",
				"Occlusion sample radius (m)");
			attr::Slider("AO bias", &r.aoBias, 0.0f, 0.1f, "%.3f",
				"Range-check bias");
			attr::Slider("AO intensity", &r.aoIntensity, 0.0f, 2.0f, "%.2f",
				"The occlusion darkening scale");
			attr::Slider("AO max distance", &r.aoMaxDistance, 2.0f, 100.0f,
				"%.0f", "The sample distance clamp (m)");
			ImGui::TextDisabled("SSAO parameters");
			int ssaoSamples = r.aoSsaoSamples;
			if (attr::Int("SSAO samples", &ssaoSamples, 1, 32,
				"The per-pixel sample count"))
				r.aoSsaoSamples = ssaoSamples;
			attr::Slider("Blur sharpness", &r.aoBlurSharpness, 0.0f,
				32.0f, "%.1f", "The bilateral blur edge preservation");
			ImGui::TextDisabled("SAO parameters");
			int saoSamples = r.aoSaoSamples;
			if (attr::Int("SAO samples", &saoSamples, 1, 32,
				"The per-pixel sample count"))
				r.aoSaoSamples = saoSamples;
			int saoTurns = r.aoSaoSpiralTurns;
			if (attr::Int("SAO spiral turns", &saoTurns, 1, 16,
				"The spiral sampling turns"))
				r.aoSaoSpiralTurns = saoTurns;
			{
				ImGui::Checkbox("SSR##enabled", &r.ssrEnabled);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Screen-space reflections");
				attr::Slider("SSR intensity", &r.ssrIntensity, 0.0f, 4.0f,
					"%.2f", "The reflection blend scale");
			}
			{
				ImGui::Checkbox("Motion blur##enabled", &r.motionBlurEnabled);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Camera motion blur (M)");
				ImGui::Checkbox("Depth of field##enabled", &r.dofEnabled);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Bokeh depth of field");
				int aam = std::clamp(r.aaMode, 0, 3);
				if (attr::Enum("Anti-aliasing", &aam,
					"Off\0MSAA 2x\0SMAA 1x\0SMAA T2x\0",
					"The edge AA strategy"))
					r.aaMode = aam;
				ImGui::Checkbox("Cubemap IBL##enabled", &r.cubemapEnabled);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Baked cubemap specular ambient");
				attr::Slider("Cubemap intensity", &r.cubemapIntensity,
					0.0f, 4.0f, "%.2f", "The specular ambient scale");
			}
		}
		ImGui::PopID(); // render_settings_panel
		// Rows write the default block — resolve into the rendered look
		// (the renderer seeds from scene.renderSettings).
		m_world->scene.renderSettings = r;
		m_world->scene.hasRenderSettings = true;
		// THE LIVE VIEW PUSH: the Lit-mode WYSIWYG renderer seeds once per
		// scene LOAD (PushPostfxEnv at attach) — panel edits after that never
		// reached it (the "the live view doesn't get affected by render
		// settings" report). Push the SAME seed live per edit: the function
		// is plain member writes, the renderer reads them per frame, so the
		// knobs hit the WYSIWYG view immediately (and the App-parity contract
		// holds — the seed and the live push are one code path).
		if (m_wysiwyg && m_wysiwygValid)
			m_wysiwyg->PushPostfxEnv(r, m_world->scene.environment);
	}

	void EditorApp::DrawWorldPanel() {
		ImGui::Text("World");
		ImGui::TextDisabled(
			"Not a scene node: global settings. No transform, no "
			"reparent, no delete. The sun + environment (and their "
			"layered zones) live in the Environment section below — "
			"scene-light sun edits are undoable light commands; the "
			"render grade lives in the Inspector's Render tab; both "
			"ride editorSet tour lines.");
		// ---- sun + environment + atmosphere: ONE section ----
		// This used to be a separate "Sun (directional)" block above the
		// environment rows — the two sun halves doubled the display and
		// collided their ImGui IDs ("Sun color" etc.); DrawEnvPanel now
		// gives each sun block a distinct ImGui ID scope.
		DrawEnvPanel();
		// NOTE: the render grade (bloom/tonemap/exposure/AO/SSR) is NOT
		// here anymore — it has its own Inspector "Render" tab
		// (DrawRenderSettingsPanel), separated on purpose (a grade is
		// never zoned).
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
					// Multiple camera nodes may share a display name.
					// Scope each selectable to its stable node ID.
					ImGui::PushID(static_cast<int>(n));
					if (ImGui::Selectable(label.c_str(), sel)) {
						m_defaultCameraNode = n;
						AutosaveTour();
					}
					ImGui::PopID();
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
			// (The GI/shadow quality knobs never ride render.* keys —
			// editor.prefs [renderQuality] owns them; see
			// DrawRenderSettingsPanel's split note.)
		}
		// Analytic env-zone master switch (scene-level; see
		// Rendergraph/envzones.h). Absent = the Scene default ON.
		put("env.zonesEnabled",
			(m_world && m_world->scene.envZonesEnabled) ? "1" : "0");
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
		scene.envZonesEnabled = true; // absent key = auto-on (Scene default)
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
			// (The GI/shadow quality knobs are editor.prefs state — never
			// render.* keys; unknown fields fall through ignored.)
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
			else if (key == "env.zonesEnabled") {
				scene.envZonesEnabled =
					StrFloat(val, scene.envZonesEnabled ? 1.0f : 0.0f) != 0.0f;
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
