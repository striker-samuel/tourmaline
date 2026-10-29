// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Bake-density visualization (see editor_app.h: the LIGHTING-mode GI
// authoring overlays). One live view answering "do the bake density
// settings fit the scene" WITHOUT baking:
//
//   - Cubemap density: the probe set the current cook settings WOULD
//     place — BuildOctreeProbePositions with the editor's octree knobs,
//     or the 8 m grid mirror (same recipe as Cook.exe) when the octree
//     is off — over a CPU BVH built from the world's retained uploads
//     (same occlusion source as the cooker, builtin backend).
//
// (The SGGI half — the baked .sggi world-field lattice dots — is
// RETIRED with the legacy octree: stage 3 of the octree retirement.)
//
// Draws in the ImGui foreground per viewport (no depth test — occluded
// markers stay visible, like the App's probe overlays); the
// baked-manifest capture overlay keeps its own (distance) colors, so
// predicted-vs-baked never share a channel: predicted cubemap probes
// use the subdivision-DEPTH ramp.

#include "editor_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <Engine/Renderer/cubemap.h> // baked-row comparison (bake manifest)

namespace editor {

// Depth/level ramp, coarse red -> fine pink (the shared density
// language the App's probe overlay used to speak; the octree-level
// twin retired with the .sggi lattice, the ramp stays for the cubemap
// prediction depth).
// Namespace-scope + extern in editor_app.h: the GI-viz window's legend
// swatches share it (declared in editor_app.h next to DrawProbeOctahedron).
const ImU32 kDensityLevelColors[8] = {
	IM_COL32(255, 64, 64, 210), IM_COL32(255, 160, 32, 210),
	IM_COL32(230, 230, 60, 210), IM_COL32(70, 230, 90, 210),
	IM_COL32(60, 200, 255, 210), IM_COL32(110, 130, 255, 210),
	IM_COL32(200, 100, 255, 210), IM_COL32(255, 110, 200, 210),
};

namespace {

// Grid mirror constants (Cook.exe CLI defaults — the editor cook dialog
// carries no grid knobs, so the grid branch is untunable here; the
// status line says so).
constexpr float kDensityGridSpacing = 8.0f; // --cubemap-grid default (m)
constexpr float kDensityGridHeight = 1.5f;  // --cubemap-height default (m)
constexpr float kDensityEmbedDist = 0.3f;   // occupancy cull reach (m)
constexpr uint32_t kDensityMaxProbes = 32;  // runtime scan budget
constexpr uint32_t kDensityRingCap = 64;    // influence rings drawn (dots: all)

void DensityCaption(ImDrawList* dl, float rectX, float rectY, float rowY,
	const char* text, ImU32 col = IM_COL32_WHITE) {
	dl->AddText(ImVec2(rectX + 9.0f, rectY + rowY + 1.0f),
		IM_COL32(0, 0, 0, 200), text);
	dl->AddText(ImVec2(rectX + 8.0f, rectY + rowY), col, text);
}

} // namespace

void EditorApp::RefreshCubemapDensity() {
	m_cubemapDensity.clear();
	m_cubemapDensityBvh = smvk::gi::TriangleBvh{};
	m_cubemapDensityScene = SceneIdForHost();
	if (!m_world) {
		m_cubemapDensityInfo = "no scene";
		return;
	}
	// Scene bounds (the placement domain, exactly like the cooker).
	const ::Aabb wb = m_world->ComputeSceneBounds();
	if (!(wb.mn.x <= wb.mx.x && wb.mn.y <= wb.mx.y && wb.mn.z <= wb.mx.z)) {
		m_cubemapDensityInfo = "empty scene bounds";
		return;
	}
	// World-space triangle soup from the retained uploads (the cooker
	// recipe minus materials: occlusion only needs triangles). Nodes
	// without a valid upload contribute nothing.
	std::vector<glm::vec3> soupPos;
	std::vector<uint32_t> soupIdx;
	for (const auto& clusterPtr : m_world->clusters) {
		if (!clusterPtr)
			continue;
		const ::Scene& scene = clusterPtr->scene;
		for (const auto& [node, meshIdx] : scene.meshForNode) {
			if (node >= scene.globalTransform.size())
				continue;
			const smvk::ClusterStaticMeshHandle h =
				clusterPtr->staticMeshes.getHandle(meshIdx);
			if (!h.valid())
				continue;
			const smvk::StaticMeshUpload* up =
				clusterPtr->GetRetainedUpload(h);
			if (!up || up->vertices.empty() || up->indices.empty())
				continue;
			const glm::mat4& g = scene.globalTransform[node];
			const uint32_t base = uint32_t(soupPos.size());
			for (const smvk::ImportStaticVertex& v : up->vertices)
				soupPos.push_back(glm::vec3(g * glm::vec4(v.pos, 1.0f)));
			for (uint32_t k : up->indices)
				soupIdx.push_back(base + k);
		}
	}
	const size_t soupTris = soupIdx.size() / 3;
	// Occlusion predicate over the cached BVH (builtin backend: no
	// Embree context in the editor process). Empty soup -> null
	// predicate: the placement lib skips culling (openness 0), and the
	// status line says so instead of silently changing density.
	smvk::CubemapOcclusionRay occ;
	if (soupTris > 0) {
		m_cubemapDensityBvh.Build(soupPos, soupIdx,
			smvk::gi::TraceBackend::Builtin);
		occ = [this](const glm::vec3& o, const glm::vec3& d, float m) {
			const float l = glm::length(d);
			if (!(l > 1e-9f) || !(m > 0.0f))
				return false;
			return m_cubemapDensityBvh.AnyHit(o, d / l, m);
		};
	}
	char info[256];
	if (ExportCfg().cubemapOctree) {
		smvk::CubemapOctreeOptions oo{};
		oo.maxCellSize = ExportCfg().cubemapOctreeMax;
		oo.minCellSize = ExportCfg().cubemapOctreeMin;
		oo.maxDepth = ExportCfg().cubemapOctreeDepth;
		oo.embedDist = kDensityEmbedDist;
		oo.maxProbes = kDensityMaxProbes;
		std::vector<smvk::CubemapPlacedProbe> placed;
		std::string olog;
		smvk::BuildOctreeProbePositions(wb.mn, wb.mx, oo, occ, placed,
			olog);
		for (const auto& p : placed) {
			CubemapDensityProbe d;
			d.center = p.position;
			d.influence = p.influenceRadius;
			d.fade = p.fadeRadius;
			d.depth = p.depth;
			m_cubemapDensity.push_back(d);
		}
		std::snprintf(info, sizeof(info),
			"octree max%.1f/min%.1f/d%u -> %zu probes (%zu BVH tris%s)%s",
			oo.maxCellSize, oo.minCellSize, oo.maxDepth,
			placed.size(), soupTris, occ ? "" : "; NO OCCLUSION CULL",
			ExportCfg().cubemap ? "" : " [cubemap bake OFF: hypothetical]");
	}
	else {
		// Grid mirror (Cook.exe --cubemap-grid path, same occupancy
		// cull): the editor dialog has no grid knobs, so this branch
		// previews the cooker defaults, not tunable settings.
		const float spacing = kDensityGridSpacing;
		const float yBase = wb.mn.y + kDensityGridHeight;
		const int nx = std::max(1, int(std::ceil(
			(wb.mx.x - wb.mn.x) / spacing)));
		const int ny = std::max(1, int(std::ceil(
			(wb.mx.y - wb.mn.y) / spacing)));
		const int nz = std::max(1, int(std::ceil(
			(wb.mx.z - wb.mn.z) / spacing)));
		size_t dropped = 0;
		static const glm::vec3 kAxes[6] = { { 1, 0, 0 }, { -1, 0, 0 },
			{ 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
		for (int iz = 0; iz < nz; ++iz)
			for (int iy = 0; iy < ny; ++iy)
				for (int ix = 0; ix < nx; ++ix) {
					glm::vec3 p(
						std::clamp(wb.mn.x + (ix + 0.5f) * spacing,
							wb.mn.x, wb.mx.x),
						std::clamp(yBase + iy * spacing,
							wb.mn.y, wb.mx.y),
						std::clamp(wb.mn.z + (iz + 0.5f) * spacing,
							wb.mn.z, wb.mx.z));
					if (occ) {
						bool embedded = true;
						for (const glm::vec3& a : kAxes)
							if (!occ(p, a, kDensityEmbedDist)) {
								embedded = false;
								break;
							}
						if (embedded) {
							++dropped;
							continue;
						}
					}
					CubemapDensityProbe d;
					d.center = p;
					d.influence = spacing * 0.5f;
					d.fade = spacing * 0.25f;
					d.depth = 0;
					m_cubemapDensity.push_back(d);
				}
		std::snprintf(info, sizeof(info),
			"grid %dx%dx%d @%.0fm -> %zu probes (%zu dropped, %zu BVH tris%s)%s",
			nx, ny, nz, spacing, m_cubemapDensity.size(), dropped,
			soupTris, occ ? "" : "; NO OCCLUSION CULL",
			ExportCfg().cubemap ? "" : " [cubemap bake OFF: hypothetical]");
	}
	m_cubemapDensityInfo = info;
	Log(std::string("Cubemap density (") + m_cubemapDensityScene + "): "
		+ m_cubemapDensityInfo);
}

void EditorApp::DrawBakeDensityOverlay() {
	// MODAL: GI authoring previews belong to LIGHTING mode.
	if (m_mode != EditorMode::Lighting)
		return;
	if (!m_world)
		return;
	if (!m_cubemapDensityOn)
		return; // (the sggi half is retired with the octree)
	// Baked manifest rows (the comparison half of the cubemap caption;
	// read live like DrawCaptureBallOverlay so the two never disagree).
	std::vector<CubemapProbeRow> bakedRows;
	CubemapProbeRowsFromFile(bakedRows);
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	for (int i = 0; i < ViewportCount(); ++i) {
		const EditorViewport& vp = m_viewports[i];
		const glm::mat4 vpm = vp.viewProj;
		glm::vec3 eye(0.0f);
		if (vp.camIdx >= 0 && size_t(vp.camIdx) < m_editorCams.size())
			eye = EditorCamEye(m_editorCams[size_t(vp.camIdx)]);
		const auto project = [&](const glm::vec3& w, ImVec2& out) {
			const glm::vec4 c = vpm * glm::vec4(w, 1.0f);
			if (c.w <= 1e-6f)
				return false; // behind the camera
			const glm::vec3 ndc = glm::vec3(c) / c.w;
			if (std::abs(ndc.x) > 1.0f || std::abs(ndc.y) > 1.0f)
				return false; // off-screen
			// The projection's near plane (reversed-Z ZO: ndc z = 1) and
			// the far side (z = 0): between-eye-and-near markers used to
			// draw over the view through the eye-plane-only test.
			if (ndc.z < 0.0f || ndc.z > 1.0f)
				return false;
			out.x = vp.rect.x + (ndc.x * 0.5f + 0.5f) * float(vp.rect.w);
			out.y = vp.rect.y + (ndc.y * 0.5f + 0.5f) * float(vp.rect.h);
			return true;
		};
		dl->PushClipRect(ImVec2(vp.rect.x, vp.rect.y),
			ImVec2(vp.rect.x + vp.rect.w, vp.rect.y + vp.rect.h), true);
		// ---- cubemap density: predicted probes, depth-colored ----
		{
			uint32_t rings = 0;
			uint32_t idx = 0;
			for (const CubemapDensityProbe& pr : m_cubemapDensity) {
				const uint32_t pi = idx++;
				ImVec2 cpt;
				if (!project(pr.center, cpt))
					continue;
			const ImU32 col =
				kDensityLevelColors[std::min(pr.depth, 7u)];
			if (m_showProbeOctahedra)
				DrawProbeOctahedron(dl, cpt, m_probeMarkerSize, col);
			else
				dl->AddCircleFilled(cpt, 2.5f, col);
			if (pi >= kDensityRingCap)
				continue;
			// Influence ring: project a world-X offset of one
			// radius (the App's trick); fade ring scales from it.
			if (m_probeRings) {
				ImVec2 ept;
				if (project(pr.center + glm::vec3(
						std::max(pr.influence, 0.01f), 0.0f, 0.0f), ept)) {
					const float dx = ept.x - cpt.x, dy = ept.y - cpt.y;
					const float rPx = std::sqrt(dx * dx + dy * dy);
					if (rPx > 2.0f && rPx < 4000.0f) {
						dl->AddCircle(cpt, rPx,
							col & IM_COL32(255, 255, 255, 90), 48);
						if (pr.fade > 0.0f) {
							const float fr = rPx * (pr.influence + pr.fade)
								/ std::max(pr.influence, 1e-3f);
							if (fr < 4000.0f)
								dl->AddCircle(cpt, fr,
									col & IM_COL32(255, 255, 255, 45), 48);
						}
						++rings;
					}
				}
			}
			if (m_probeLabels) {
				char plab[16];
				std::snprintf(plab, sizeof(plab), "P%u", pi);
				dl->AddText(ImVec2(cpt.x + 6.0f, cpt.y - 7.0f), col, plab);
			}
			}
			char ccap[256];
			if (m_cubemapDensity.empty())
				std::snprintf(ccap, sizeof(ccap),
					"CubeDensity: no prediction (%s) — Refresh in GI panel",
					m_cubemapDensityInfo.empty()
						? "toggle on to build" : m_cubemapDensityInfo.c_str());
			else
				std::snprintf(ccap, sizeof(ccap),
					"CubeDensity: %zu predicted vs %zu baked manifest "
					"(depth red->pink, rings first %u)",
					m_cubemapDensity.size(), bakedRows.size(),
					kDensityRingCap);
			DensityCaption(dl, vp.rect.x, vp.rect.y, 52.0f, ccap);
			if (!m_cubemapDensityInfo.empty())
				DensityCaption(dl, vp.rect.x, vp.rect.y, 68.0f, m_cubemapDensityInfo.c_str(),
					IM_COL32(180, 200, 220, 255));
		}
		// (The SGGI density half — the baked .sggi lattice dots — is
		// RETIRED with the octree: stage 3 of the octree retirement. The
		// cubemap density preview above is the live half.)
		dl->PopClipRect();
	}
}

} // namespace editor
