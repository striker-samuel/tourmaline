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
		if (!FindEnv(m_world->scene, node)) {
			SetEnv(m_world->scene, node, CurrentEnv()); // start from the current source // start from the current source
			Log("Node " + std::to_string(node) + " marked as Environment.");
		}
		ResolveSceneEnv();
	}

	void EditorApp::UnmarkEnvNode(uint32_t node) {
		if (!m_world)
			return;
		if (RemoveEnv(m_world->scene, node) && m_envCurrent == node)
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
			auto it = FindEnv(m_world->scene, m_envCurrent);
			if (it)
				src = &it->environment;
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
		ImGui::SeparatorText("Environment (scene definition)");
		// Cook flows live in the BottomDock Cook tab (File > Export...),
		// not here: this panel owns the look inputs, the tab owns the
		// product step (full cook + quick bakes + cubemap refresh).
		if (!m_world) {
			ImGui::Text("No scene - open a scene to author the look.");
			return;
		}
		// Source combo: editor default + every env node.
		std::string curLabel = "Editor default";
		if (m_envCurrent != UINT32_MAX)
			curLabel = "Node " + std::to_string(m_envCurrent);
		if (ImGui::BeginCombo("Source##env", curLabel.c_str())) {
			if (ImGui::Selectable("Editor default", m_envCurrent == UINT32_MAX))
				m_envCurrent = UINT32_MAX;
			for (const EnvRow& _row : m_world->scene.tables.envs) {
		const NodeId node = _row.node; const auto& env = _row.environment;
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
			attr::Float("Zone priority", &e.zonePriority, 0.25f, 0.0f,
				100.0f, "%.0f",
				"Legacy zone priority (the look block's): applies while "
				"the node has no volume entry below (the volume priority "
				"wins when present). Overlap: higher wins; ties go "
				"innermost.");
			// VOLUME settings (Unreal-style bounds + blending over the
			// transform box; see EnvVolume). Created on first edit below
			// (inheriting the look's priority, so legacy zones don't jump);
			// absent = legacy zone behavior.
			auto& scene = m_world->scene;
			const EnvVolumeRow* vit = FindEnvVolume(scene, m_envCurrent);
			const bool hasEntry = vit != nullptr;
			EnvVolume tmp = hasEntry ? vit->volume : EnvVolume{};
			bool volChanged = false;
			bool priEdited = false;
			if (attr::Check("Volume enabled", &tmp.enabled,
					"The Unreal-style volume bounds + blending over the "
					"node's transform box"))
				volChanged = true;
			// Per-face infinity: a set face pushes that box face to
			// -/+infinity (half-spaces and slabs; all six = infinite
			// everywhere, the Unreal unbound equivalent).
			{
				struct FaceBit { const char* label; uint32_t bit; };
				const FaceBit faces[6] = { { "X-", kEnvInfNegX },
					{ "X+", kEnvInfPosX }, { "Y-", kEnvInfNegY },
					{ "Y+", kEnvInfPosY }, { "Z-", kEnvInfNegZ },
					{ "Z+", kEnvInfPosZ } };
				ImGui::SeparatorText("Infinite faces");
				for (int i = 0; i < 6; ++i) {
					bool set = (tmp.infMask & faces[i].bit) != 0;
					if (attr::Check(faces[i].label, &set,
							"A set face pushes that box face to -/+inf: "
							"Y- + box top at y=0 covers y <= 0 (underwater), "
							"Y+ + box bottom at y=50 covers y >= 50 (high "
							"haze); all six = infinite everywhere. Coverage "
							"is analytic 3D, so Y-varying zones work "
							"exactly.")) {
						if (set)
							tmp.infMask |= faces[i].bit;
						else
							tmp.infMask &= ~faces[i].bit;
						volChanged = true;
					}
				}
			}
			if (attr::Float("Volume priority", &tmp.priority, 0.25f, 0.0f,
					100.0f, "%.0f",
					"The volume's own priority (wins over the legacy zone "
					"priority above)"))
				priEdited = true;
			volChanged |= priEdited;
			if (attr::Float("Blend weight", &tmp.blendWeight, 0.01f, 0.0f,
					1.0f, "%.2f", "Scales the whole zone"))
				volChanged = true;
			if (attr::Float("Blend radius", &tmp.blendRadius, 0.1f, 0.0f,
					64.0f, "%.1f",
					"Softens the edge OUTSIDE the box (3D distance) - "
					"applies per pixel in the terrain + volumetric fog"))
				volChanged = true;
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
				SetEnvVolume(scene, m_envCurrent, tmp);
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
			ImGui::Text("No scene directional - the environment sun below "
				"is THE sun (drives the pak, every bake and the App).");
		}
		else if (glm::vec3 aimWs(0.0f);
			const ::SceneLight * li = FindNodeLight(sun, &aimWs)) {
			ImGui::Text("'%s' (#%u)", li->name.c_str(), sun);
			ImGui::SameLine();
			if (ImGui::Button("Select node##sun"))
				SetSelection(sun);
			{
				char toSun[128];
				std::snprintf(toSun, sizeof(toSun), "(%.2f, %.2f, %.2f)%s",
					-aimWs.x, -aimWs.y, -aimWs.z,
					m_hasCookedSun && m_cookedSunName == li->name
					? " - seeds the editor IBL default" : "");
				attr::Readonly("toSun", toSun);
			}
			if ((li->flags & ::SceneLight::kSceneLightFlag_Static) != 0)
				attr::Readonly("Bake light",
					"STATIC - bake-only (excluded from runtime)");
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
			// The checkbox edits the proposed new value; persist it as an
			// undoable light command rather than mutating the scene light
			// directly.
			if (attr::Check("Static", &sstatic,
					"STATIC: bake-only (excluded from runtime Forward+)")) {
				EditCmd c{};
				c.verb = EditVerb::SetLightAttr;
				c.node = sun;
				c.lightAttr = "static";
				c.afterFlag = sstatic ? 1 : 0; // checkbox now contains NEW state
				ReadNodeLightAttr(sun, "static", c.beforeVec4, c.beforeNum,
					c.beforeFlag, c.beforeText);
				Do(c);
			}
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
			ImGui::SeparatorText("Analytic fog parameters");
			attr::Slider("Fog density", &e.fogDensity, 0.0f, 1.0f, "%.3f",
				"The exp2 distance fog density");
			float fc4[4] = { e.fogColor.x, e.fogColor.y, e.fogColor.z, 1.0f };
			if (attr::Color("Fog color", fc4, "The analytic fog tint"))
				e.fogColor = glm::vec3(fc4[0], fc4[1], fc4[2]);

			ImGui::SeparatorText("Volumetric fog parameters");
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
			attr::Check("Sample GI probes", &e.volumetricUseGi,
				"On: sample SG-GI probes; off: sky IBL");
			attr::Check("Sun scatter", &e.volumetricUseSun,
				"Enable analytic sun scattering");
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
			if (attr::Text("Skybox URI", skybox, sizeof(skybox),
					"A skybox texture reference (sampling is TODO; stored "
					"with the environment)"))
				e.skyboxUri = skybox;
		}
		if (m_envCurrent != UINT32_MAX) {
			ImGui::Text("Env node %u: position = node translate, "
				"size = node scale.", m_envCurrent);
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
		// RENDER SETTINGS: the renderer QUALITY options (top — the
		// session-owned surface: the ambient/GI block + the App Quality
		// tab parity rows, live on the WYSIWYG view per edit, session-
		// only, never tour/pak content) + the postfx look
		// (SceneRenderSettings — the scene-global grade the tour/pak/
		// App carry; edits autosave once per finished edit). Numeric/
		// enum rows use the attribute stubs; switches use native ImGui
		// checkboxes so changes write the backing bool exactly once.
		// Both halves always show all parameter rows regardless of
		// whether the corresponding effect is active; users can edit
		// inactive presets.
		ImGui::Separator();
		ImGui::Text("Render settings (GI / shadows / bloom / tonemap / exposure / AO / SSR)");
		{
			engine::RenderQualityConfig& q = m_renderQuality;
			ImGui::PushID("render_quality");
			ImGui::SeparatorText("Global illumination");
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
			attr::Check("Lightmaps", &q.lightmapsEnabled,
				"Baked 2D lightmap branch (static "
					"geometry; needs a --lightmap bake)");
			attr::Check("Regular probe volumes", &q.regProbesEnabled,
				"Baked regular probe grids "
					"(dynamic-object GI)");
			attr::Check("Convex GI volumes", &q.giVolumesEnabled,
				"Confined-space dense probe volumes, "
					"composited over the octree field by priority");
			attr::Check("Probe pre-mix", &q.probePremixEnabled,
				"GPU pre-mixed probe blend (one cubemap "
					"fetch per pixel); off = the per-pixel probe walk");
			ImGui::SeparatorText("Shadows");
			attr::Check("EVSM moment shadows", &q.evsmEnabled,
				"EVSM moment shadow maps (soft shadows; "
					"falls back to PCF when unsupported)");
			attr::Slider("EVSM softness", &q.evsmSoftness, 0.0f, 4.0f, "%.2f",
				"Scales shadow softness relative to the light's "
				"physical size (0 = hard).");
			{
				// The renderer's whole SESSION-quality surface (the App's
				// Quality tab parity): display, cascades, local-light
				// shadow budgets, EVSM extras, contact shadows, SSGI, SSR
				// trace detail. Everything here is host/session state —
				// the LOOK halves (SSR enable/intensity, the enables) stay
				// in the scene-authored block below.
				ImGui::SeparatorText("Display");
				attr::Slider("Render scale", &q.renderScale, 0.25f, 2.0f,
					"%.2fx", "The internal resolution scale (1 = the "
					"viewport's native extent)");
				{
					// Conventional order; indices remap to the sl enum.
					const char* dlssNames[] = { "Off", "Ultra Performance",
						"Performance", "Balanced", "Quality", "Ultra Quality",
						"DLAA" };
					const int dlssEnumOf[] = {
						int(smvk::DlssMode::Off),
						int(smvk::DlssMode::UltraPerformance),
						int(smvk::DlssMode::MaxPerformance),
						int(smvk::DlssMode::Balanced),
						int(smvk::DlssMode::MaxQuality),
						int(smvk::DlssMode::UltraQuality),
						int(smvk::DlssMode::DLAA),
					};
					int dlssSel = 0;
					for (int i = 0; i < 7; ++i)
						if (dlssEnumOf[i] == int(q.dlssMode))
							dlssSel = i;
					if (attr::Enum("DLSS mode", &dlssSel,
							"Off\0Ultra Performance\0Performance\0"
							"Balanced\0Quality\0Ultra Quality\0DLAA\0",
							"Streamline DLSS upscaling (Windows-only; "
							"inactive without the sl runtime)"))
						q.dlssMode = smvk::DlssMode(dlssEnumOf[dlssSel]);
				}
				{
					int tf = int(q.textureFiltering);
					if (attr::Enum("Texture filtering", &tf,
							"Trilinear\0Anisotropic 2x\0Anisotropic 4x\0"
							"Anisotropic 8x\0Anisotropic 16x\0",
							"The bindless samplers' filtering (material "
							"surfaces)"))
						q.textureFiltering =
							engine::TextureFiltering(std::clamp(tf, 0, 4));
				}
				{
					int tm = (q.transparentMode
							== smvk::TransparentMode::Blended) ? 1 : 0;
					if (attr::Enum("Transparency", &tm,
							"Dithered\0Alpha blend\0",
							"The Blend-material technique (dithered keeps "
							"opaque pipeline state; blended sorts)"))
						q.transparentMode = (tm == 1)
							? smvk::TransparentMode::Blended
							: smvk::TransparentMode::Dithered;
				}
				ImGui::SeparatorText("Directional cascades");
				{
					const uint32_t resOptions[] = { 512, 1024, 2048, 4096 };
					int resIdx = 1;
					for (int i = 0; i < 4; ++i)
						if (resOptions[i] == q.cascadeResolution)
							resIdx = i;
					if (attr::Enum("Cascade resolution", &resIdx,
							"512\0" "1024\0" "2048\0" "4096\0",
							"Per-cascade tile resolution (the atlas "
							"recreates on change - one-time hitch)"))
						q.cascadeResolution =
							resOptions[std::clamp(resIdx, 0, 3)];
				}
				attr::Slider("Split lambda", &q.cascadeLambda, 0.0f, 1.0f,
					"%.2f", "The split scheme's near/far weighting");
				attr::Check("SDSM", &q.sdsmEnabled,
					"Fit the splits to the measured depth "
						"(sampled distribution shadow maps)");
				attr::Slider("Shadow distance", &q.cascadeMaxDistance, 0.0f,
					500.0f, "%.0f", "The cascade far clamp in meters "
					"(0 = the scene far plane)");
				attr::Slider("Depth bias scale", &q.cascadeDepthBiasScale,
					0.0f, 4.0f, "%.2f", "The cascade depth-bias multiplier");
				attr::Slider("Normal bias scale", &q.cascadeNormalBiasScale,
					0.0f, 4.0f, "%.2f", "The cascade normal-offset bias "
					"multiplier");
				ImGui::SeparatorText("Local-light shadow budgets");
				attr::Check("Spot shadows", &q.spotShadowsEnabled,
					"Render the shared spot atlas (the "
						"per-light flag still gates each light)");
				{
					int m = q.spotShadowMax;
					if (attr::Int("Shadow spots max", &m, 0,
							int(smvk::kMaxSpotShadows),
							"The per-frame shadowed-spot budget"))
						q.spotShadowMax = m;
				}
				attr::Slider("Spot shadow max distance",
					&q.spotShadowMaxDistance, 5.0f, 120.0f, "%.0f",
					"Hard cutoff on camera->light distance (m)");
				attr::Check("Point shadows", &q.pointShadowsEnabled,
					"Six cube-face tiles per selected point "
						"in the shared atlas");
				{
					int m = q.pointShadowMax;
					if (attr::Int("Shadow points max", &m, 0,
							int(smvk::kMaxPointShadowLights),
							"The per-frame shadowed-point budget"))
						q.pointShadowMax = m;
				}
				attr::Slider("Point shadow max distance",
					&q.pointShadowMaxDistance, 5.0f, 120.0f, "%.0f",
					"Hard cutoff on camera->light distance (m)");
				ImGui::SeparatorText("EVSM pipeline");
				{
					int pipe = (q.evsmPipeline
							== smvk::EvsmPipelineMode::SameFrame) ? 1 : 0;
					if (attr::Enum("Pipeline", &pipe,
							"One-frame lag\0Same-frame\0",
							"The moment atlas' pipelining (lag = the async "
							"blur wins, same-frame = no lag)"))
						q.evsmPipeline = (pipe == 1)
							? smvk::EvsmPipelineMode::SameFrame
							: smvk::EvsmPipelineMode::OneFrameLag;
				}
				attr::Slider("Local light size", &q.evsmSpotLightSize, 0.0f,
					2.0f, "%.2f", "The spot PCSS light diameter (m; 0 = hard)");
				ImGui::SeparatorText("Sun contact shadows");
				attr::Check("Contact shadows", &q.contactShadowsEnabled,
					"The pre-Main depth raymarch toward the "
						"sun (the contact-hardening detail the cascades "
						"cannot resolve)");
				attr::Slider("Contact distance", &q.contactShadowMaxDistance,
					0.05f, 2.0f, "%.2f", "The march length (m)");
				attr::Slider("Contact thickness", &q.contactShadowThickness,
					0.005f, 0.2f, "%.3f",
					"The thin-occluder acceptance band (m)");
				attr::Slider("Contact start bias", &q.contactShadowStartBias,
					0.0f, 0.05f, "%.3f", "The ray start + self-hit floor (m)");
				{
					int s = q.contactShadowSteps;
					if (attr::Int("Contact steps", &s, 4, 32,
							"The march sample count"))
						q.contactShadowSteps = s;
				}
				{
					int r = (q.contactShadowResolutionDivisor == 1) ? 0 : 1;
					if (attr::Enum("Contact resolution", &r, "Full\0Half\0",
							"The visibility texture's resolution (half saves "
							"tracing but can lose thin occluders)"))
						q.contactShadowResolutionDivisor = (r == 0) ? 1 : 2;
				}
				ImGui::SeparatorText("Diffuse screen-space GI");
				attr::Check("SSGI diffuse", &q.ssgiDiffuseEnabled,
					"One cosine-hemisphere bounce that "
						"REPLACES the diffuse ambient slice (experimental; "
						"off under MSAA)");
				attr::Slider("SSGI intensity", &q.ssgiDiffuseIntensity, 0.0f,
					4.0f, "%.2f", "The replacement strength (0 = off)");
				attr::Slider("SSGI max distance", &q.ssgiDiffuseMaxDistance,
					0.5f, 50.0f, "%.1f", "The ray length budget (m) - the "
					"LOCAL bounce");
				attr::Slider("SSGI thickness", &q.ssgiDiffuseThickness, 0.01f,
					2.0f, "%.2f", "The depth acceptance band (m)");
				attr::Slider("SSGI temporal blend",
					&q.ssgiDiffuseTemporalBlend, 0.0f, 0.98f, "%.2f",
					"The history weight (motion-reprojected accumulation)");
				attr::Slider("SSGI bilateral sharpness",
					&q.ssgiDiffuseBlurSharpness, 0.0f, 32.0f, "%.1f",
					"The bilateral depth edge weight");
				{
					int s = q.ssgiDiffuseMaxIntersections;
					if (attr::Int("SSGI traversal steps", &s, 4, 128,
							"The traversal cap (exhaustion = miss)"))
						q.ssgiDiffuseMaxIntersections = s;
				}
				attr::Check("SSGI temporal filter", &q.ssgiDiffuseTaaEnabled,
					"Temporal accumulation with depth + "
						"normal rejection; falls back to spatial-only "
						"without motion vectors");
				ImGui::SeparatorText("SSR trace detail");
				{
					int s = q.ssrSteps;
					if (attr::Int("SSR ray steps", &s, 1, 128,
							"The raymarch step count (half-res trace)"))
						q.ssrSteps = s;
				}
				{
					int s = q.ssrRefineSteps;
					if (attr::Int("SSR refine steps", &s, 0, 8,
							"The binary refine iterations"))
						q.ssrRefineSteps = s;
				}
				attr::Slider("SSR max distance", &q.ssrMaxDistance, 1.0f,
					200.0f, "%.0f", "The ray length (m)");
				attr::Slider("SSR thickness", &q.ssrThickness, 0.01f, 2.0f,
					"%.2f", "The depth acceptance band (m)");
				attr::Slider("SSR F0", &q.ssrF0, 0.0f, 1.0f, "%.2f",
					"The dielectric base reflectance (glass 0.04)");
				attr::Slider("SSR bilateral sharpness", &q.ssrBlurSharpness,
					0.0f, 32.0f, "%.1f", "The bilateral depth edge weight");
				{
					int b = q.ssrBlurMode;
					if (attr::Enum("SSR blur mode", &b,
							"2D bilateral\0Separable H+V\0",
							"The blur filter (a live flip resets the "
							"temporal history)"))
						q.ssrBlurMode = std::clamp(b, 0, 1);
				}
				ImGui::SeparatorText("Cubemap IBL detail");
				attr::Slider("Cubemap LOD bias", &q.cubemapLodBias, -2.0f,
					4.0f, "%.2f", "Added to the roughness LOD");
				{
					int b = int(q.cubemapStreamBudget);
					if (attr::Int("Stream budget", &b, 0, 64,
							"Resident non-pinned rows (nearest first)"))
						q.cubemapStreamBudget = uint32_t(
							std::clamp(b, 0, 64));
				}
				ImGui::SeparatorText("Motion blur detail");
				{
					int s = q.motionBlurSamples;
					if (attr::Int("Blur samples", &s, 1, 32,
							"The gather tap count"))
						q.motionBlurSamples = s;
				}
				attr::Slider("Blur max radius", &q.motionBlurMaxRadius, 0.0f,
					64.0f, "%.0f", "The blur clamp (px)");
				attr::Slider("Blur strength", &q.motionBlurStrength, 0.0f,
					2.0f, "%.2f", "The shutter scale");
				attr::Check("Tile dilation", &q.motionBlurTileDilation,
					"CoD:AW tile max + 5x5 dilate "
						"prepasses (16px tiles)");
				ImGui::SeparatorText("Depth of field detail");
				attr::Check("HQ bokeh", &q.dofHqBokeh,
					"48-tap hex gather (off = the low "
						"single-pass Poisson)");
				attr::Slider("Focus distance", &q.dofFocusDistance, 0.5f,
					100.0f, "%.1f", "The in-focus plane (m, view distance)");
				attr::Slider("Focus range", &q.dofFocusRange, 0.0f, 20.0f,
					"%.1f", "The sharp half-depth around the focus (m)");
				attr::Slider("Low max radius", &q.dofMaxRadius, 0.0f, 64.0f,
					"%.0f", "The low-pass clamp (px)");
				attr::Slider("Bokeh max radius", &q.dofBokehMaxRadius, 0.0f,
					64.0f, "%.0f", "The HQ bokeh clamp (px)");
				attr::Slider("Highlight boost", &q.dofHighlightBoost, 1.0f,
					4.0f, "%.2f", "The HQ bright-tap weight multiplier");
				attr::Slider("Highlight threshold", &q.dofHighlightThreshold,
					0.0f, 8.0f, "%.1f", "The HQ pre-exposure luminance knee");
			}
			ImGui::PopID();
			// Live push per edit (plain member writes — the same pattern
			// as the look's PushPostfxEnv at the tail; session-only, and
			// LaunchWysiwyg re-applies after every attach so relaunches
			// keep the session's choices).
			if (m_wysiwyg && m_wysiwygValid)
				m_wysiwyg->PushRenderQuality(q);
		}
		if (!m_world) {
			ImGui::Text("No scene - open a scene to author the look.");
			return;
		}
		SceneRenderSettings& r = m_world->scene.defaultRenderSettings;
		// PERSISTENCE PARITY with every other authored value: the look
		// is SCENE state (render.* keys ride the tour), so edits must
		// mark the doc dirty like a transform edit does — the old panel
		// wrote the struct silently (a manual save caught it, autosave
		// never did). The once-per-FINISHED-edit contract (the default
		// camera's pattern): a drag stays "editing" until the values
		// settle, a toggle flips for one frame, and the frame the soup
		// stops changing is the edit's end -> ONE AutosaveTour.
		const SceneRenderSettings lookBefore = r;
		// Keep Render controls independent from same-named widgets in other tabs.
		ImGui::PushID("render_settings_panel");
		{
			// Every row rides the attr stubs (name left, value right -
			// the panel-wide convention); the section is ID-scoped and
			// the stubs PushID their own names.
			ImGui::SeparatorText("Post-processing look");
			attr::Check("Bloom", &r.bloomEnabled,
				"Enable the HDR bright-pass bloom effect");
			// Keep the editable parameters visible even when Bloom is disabled.
			r.bloomIntensity = std::clamp(r.bloomIntensity, 0.0f, 2.0f);
			r.bloomThreshold = std::clamp(r.bloomThreshold, 0.0f, 16000.0f);
			attr::Slider("Bloom intensity", &r.bloomIntensity, 0.0f, 2.0f,
				"%.2f", "Bloom contribution (0..2)");
			attr::Float("Bloom threshold", &r.bloomThreshold, 0.05f, 0.0f,
				16000.0f, "%.2f", "Pre-exposure HDR bright-pass threshold. "
				"Ctrl-click to enter a precise value.");
			int tm = int(std::min(r.tonemapMode, 3u));
			if (attr::Enum("Tonemap", &tm,
				"Reinhard\0ACES\0Uncharted2\0GT7\0",
				"The HDR->LDR operator"))
				r.tonemapMode = uint32_t(tm);
			attr::Check("Auto exposure", &r.autoExposure,
				"Auto: histogram metering and eye adaptation. "
					"Off: use manual exposure.");
			// Both auto and manual parameters are always visible and editable.
			ImGui::SeparatorText("Auto exposure parameters");
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
			ImGui::SeparatorText("Manual exposure parameter");
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
			ImGui::SeparatorText("SSAO parameters");
			int ssaoSamples = r.aoSsaoSamples;
			if (attr::Int("SSAO samples", &ssaoSamples, 1, 32,
				"The per-pixel sample count"))
				r.aoSsaoSamples = ssaoSamples;
			attr::Slider("Blur sharpness", &r.aoBlurSharpness, 0.0f,
				32.0f, "%.1f", "The bilateral blur edge preservation");
			ImGui::SeparatorText("SAO parameters");
			int saoSamples = r.aoSaoSamples;
			if (attr::Int("SAO samples", &saoSamples, 1, 32,
				"The per-pixel sample count"))
				r.aoSaoSamples = saoSamples;
			int saoTurns = r.aoSaoSpiralTurns;
			if (attr::Int("SAO spiral turns", &saoTurns, 1, 16,
				"The spiral sampling turns"))
				r.aoSaoSpiralTurns = saoTurns;
			{
				attr::Check("SSR", &r.ssrEnabled,
					"Screen-space reflections");
				attr::Slider("SSR intensity", &r.ssrIntensity, 0.0f, 4.0f,
					"%.2f", "The reflection blend scale");
			}
			{
				attr::Check("Motion blur", &r.motionBlurEnabled,
					"Camera motion blur (M)");
				attr::Check("Depth of field", &r.dofEnabled,
					"Bokeh depth of field");
				int aam = std::clamp(r.aaMode, 0, 3);
				if (attr::Enum("Anti-aliasing", &aam,
					"Off\0MSAA 2x\0SMAA 1x\0SMAA T2x\0",
					"The edge AA strategy"))
					r.aaMode = aam;
				attr::Check("Cubemap IBL", &r.cubemapEnabled,
					"Baked cubemap specular ambient");
				attr::Slider("Cubemap intensity", &r.cubemapIntensity,
					0.0f, 4.0f, "%.2f", "The specular ambient scale");
			}
		}
		ImGui::PopID(); // render_settings_panel
		// Rows write the default block — resolve into the rendered look
		// (the renderer seeds from scene.renderSettings).
		m_world->scene.renderSettings = r;
		m_world->scene.hasRenderSettings = true;
		// The finished-edit edge (see lookBefore above): one autosave
		// per settled edit, never per drag tick.
		if (std::memcmp(&lookBefore, &r, sizeof(lookBefore)) != 0)
			m_renderLookEditing = true;
		else if (m_renderLookEditing) {
			m_renderLookEditing = false;
			AutosaveTour();
		}
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
		ImGui::SeparatorText("World");
		ImGui::Text("Global settings - no transform, no reparent, no "
			"delete. The sun + environment live in the section below; "
			"the render grade lives in the Inspector's Render tab.");
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
				for (const CameraRow& _row : m_world->scene.tables.cameras) {
					const NodeId n = _row.node;
					cams.push_back(n);
				}
				std::sort(cams.begin(), cams.end());
			}
			// Dangling selection (camera deleted/detached) reads as none.
			if (m_defaultCameraNode != UINT32_MAX
				&& (!m_world
					|| m_defaultCameraNode
					>= m_world->scene.hierarchy.size()
					|| !FindCamera(m_world->scene, m_defaultCameraNode)))
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
			for (const EnvRow& _row : m_world->scene.tables.envs) {
				const NodeId n = _row.node;
				nodes.push_back(n);
			}
			std::sort(nodes.begin(), nodes.end());
			for (uint32_t n : nodes) {
				const int cn = WorldToCluster(n);
				if (cn < 0)
					continue; // unmapped node: keep the file loadable
				saveEnv("env.n", uint32_t(cn), (*FindEnv(m_world->scene, n)).environment);
			}
		}
		// Env-volume settings (envvol.n groups, one per node carrying an
		// entry; see EnvVolume). Sorted + cluster-remapped like env.n above.
		{
			std::vector<uint32_t> vnodes;
			if (m_world) {
				for (const EnvVolumeRow& _row : m_world->scene.tables.envVolumes) {
					const NodeId n = _row.node;
					vnodes.push_back(n);
				}
				std::sort(vnodes.begin(), vnodes.end());
				for (uint32_t n : vnodes) {
					const int cn = WorldToCluster(n);
					if (cn < 0)
						continue; // unmapped node: keep the file loadable
					const EnvVolume& v = (*FindEnvVolume(m_world->scene, n)).volume;
					put("envvol.n.node", std::to_string(uint32_t(cn)));
					put("envvol.n.mask", std::to_string(v.infMask & 63u));
					put("envvol.n.priority", std::to_string(v.priority));
					put("envvol.n.weight", std::to_string(v.blendWeight));
					put("envvol.n.radius", std::to_string(v.blendRadius));
					put("envvol.n.enabled", v.enabled ? "1" : "0");
				}
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
			// (The renderer QUALITY knobs never ride render.* keys —
			// they are session-only state (LivePrefs' [renderQuality]
			// section is retired; see DrawRenderSettingsPanel's split
			// note).)
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
		scene.tables.envs.clear();
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
			// (The renderer QUALITY knobs are session-only state — never
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
				SetEnv(scene, wn, env);
		}
		for (const auto& entry : nodeVols) {
			const uint32_t wn = ClusterToWorld(entry.node);
			if (wn == UINT32_MAX || wn >= scene.hierarchy.size())
				continue; // dangling volume: drop (never fatal)
			EnvVolume vol = entry.vol;
			vol.blendWeight = std::clamp(vol.blendWeight, 0.0f, 1.0f);
			if (!(vol.blendRadius >= 0.0f))
				vol.blendRadius = 0.0f;
			SetEnvVolume(scene, wn, vol);
		}
		if (haveCurrent) {
			const uint32_t wn = ClusterToWorld(currentCluster);
			if (wn != UINT32_MAX && FindEnv(scene, wn))
				m_envCurrent = wn;
		}
		// Publish the loaded selection into the rendered look.
		ResolveSceneEnv();
		if (!scene.tables.envs.empty())
			Log("tour: restored " + std::to_string(scene.tables.envs.size()) +
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
			|| !(FindCamera(m_world->scene, m_defaultCameraNode) != nullptr))
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
				|| !(FindCamera(scene, target) != nullptr)) {
				Log("tour: camera.default target does not resolve: '" + val
					+ "' (none stands).");
				continue;
			}
			m_defaultCameraNode = target;
		}
	}

} // namespace editor

