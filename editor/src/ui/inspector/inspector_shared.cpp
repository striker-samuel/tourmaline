// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Editor UI: menu bar, outliner (hierarchy), inspector (transform /
// material / flags), cook panel, console, viewport overlay.
//
// Every edit goes through the CRUD command stack (Do/Undo/Redo); drags
// coalesce into one command per gesture via m_pending.

#include "editor_app.h"
#include "editor_brush_presets.h"
#include "modal_bindings.h"
#include "editor_attr_ui.h" // the attribute-widget stubs (attr::Bool/...)
#include "editor_asset_browser_nav.h" // collection folders belong at root
#include "editor_asset_inspect.h" // generic file/directory inspector
#include "editor_asset_creation.h"
#include "editor_asset_dcc.h" // DCC source link + export build stage

#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <unordered_map>

#include <glm/gtc/type_ptr.hpp>

#include <Engine/Asset/shape_asset.h>
#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Asset/pak.h>
#include <Engine/Profile/profile.h>

#include <Engine/Asset/spriteset.h>
#include <imgui.h>
#include <imgui_internal.h> // DockBuilder: initial workspace graph + user reset
#include <SDL3/SDL_misc.h> // SDL_OpenURL: launch the local asset history UI
#include <SDL3/SDL_filesystem.h> // SDL_GetBasePath (staged export scripts)
#include <imgui_impl_vulkan.h> // ImGui_ImplVulkan_AddTexture (TextureThumb)
#include <stb_image.h>         // stbi_load (AssetSidecarThumb; impl in editor_branding)

#include <ktx.h>

#include <cmath>
#include <cstdint>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h> // ShellExecuteA (reveal-in-explorer)
#endif


namespace editor {
	const EditorApp::TexPreview& EditorApp::ResolveDiffusePreview(uint32_t matWorld) {
		TexPreview& pv = m_texPreview;
		smvk::Material* mat = m_world ? m_world->GetMaterial(matWorld) : nullptr;
		tour::Cluster* cluster = nullptr;
		int slot = -1;
		if (mat && matWorld < m_world->materialLocations.size()) {
			cluster = m_world->materialLocations[matWorld].cluster;
			slot = mat->values.textures.x;
		}
		std::string uri;
		const std::vector<uint8_t>* src = nullptr;
		if (cluster && slot >= 0) {
			for (const auto& [u, h] : cluster->ListTextureUris()) {
				if (int(h.index()) == slot) {
					uri = u.Str();
					break;
				}
			}
			if (!uri.empty())
				src = cluster->GetTextureSource(smvk::StringViewT(uri));
		}
		const size_t srcBytes = src ? src->size() : 0;
		if (pv.cluster == cluster && pv.slot == slot && pv.srcBytes == srcBytes &&
			(!pv.status.empty() || pv.ok))
			return pv; // cache hit (status is always set after a resolve)
		pv = TexPreview{};
		pv.cluster = cluster;
		pv.slot = slot;
		pv.srcBytes = srcBytes;
		if (!mat) {
			pv.status = "no material";
			return pv;
		}
		if (slot < 0) {
			pv.status = "no diffuse texture (factor only)";
			return pv;
		}
		if (!cluster) {
			pv.status = "no owning cluster";
			return pv;
		}
		if (uri.empty()) {
			pv.status = "slot has no registered texture";
			return pv;
		}
		pv.uri = uri;
		if (!src || src->empty()) {
			pv.status = "no stashed source bytes for '" + uri + "'";
			return pv;
		}
		ktxTexture2* raw = nullptr;
		if (ktxTexture2_CreateFromMemory(src->data(),
			static_cast<ktx_size_t>(src->size()),
			KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &raw) != KTX_SUCCESS) {
			pv.status = "KTX parse failed";
			return pv;
		}
		struct Guard {
			ktxTexture2* t;
			~Guard() { if (t) ktxTexture2_Destroy(t); }
		} guard{ raw };
		if (ktxTexture2_NeedsTranscoding(raw)) {
			if (ktxTexture2_TranscodeBasis(raw, KTX_TTF_RGBA32, 0) != KTX_SUCCESS) {
				pv.status = "basis transcode failed";
				return pv;
			}
		}
		// 8-bit 1-4 channel data (transcoded RGBA32 lands here too: 4
		// comps, 4-byte texels). NOTE: GetElementSize is bytes per TEXEL,
		// not per component — RGBA8 reports 4.
		const ktx_uint32_t comps = ktxTexture2_GetNumComponents(raw);
		const ktx_size_t elemSize =
			ktxTexture_GetElementSize(reinterpret_cast<ktxTexture*>(raw));
		if (comps < 1 || comps > 4 || elemSize != ktx_size_t(comps)) {
			char fbuf[128];
			std::snprintf(fbuf, sizeof(fbuf),
				"unsupported pixel format (comps=%u elem=%u B)",
				comps, unsigned(elemSize));
			pv.status = fbuf;
			return pv;
		}
		// Smallest mip that still covers the thumbnail: less to downsample.
		ktx_uint32_t level = 0;
		while (level + 1 < raw->numLevels) {
			const ktx_uint32_t nw =
				std::max<ktx_uint32_t>(raw->baseWidth >> (level + 1), 1u);
			const ktx_uint32_t nh =
				std::max<ktx_uint32_t>(raw->baseHeight >> (level + 1), 1u);
			if (std::max(nw, nh) < 48)
				break;
			++level;
		}
		const ktx_uint32_t w =
			std::max<ktx_uint32_t>(raw->baseWidth >> level, 1u);
		const ktx_uint32_t h =
			std::max<ktx_uint32_t>(raw->baseHeight >> level, 1u);
		ktx_size_t offset = 0;
		if (ktxTexture2_GetImageOffset(raw, level, 0, 0, &offset) != KTX_SUCCESS) {
			pv.status = "mip offset failed";
			return pv;
		}
		const ktx_size_t rowPitch =
			ktxTexture_GetRowPitch(reinterpret_cast<ktxTexture*>(raw), level);
		if (rowPitch < ktx_size_t(w) * comps) {
			pv.status = "bad mip stride";
			return pv;
		}
		const uint8_t* base = raw->pData + offset;
		// Thumbnail grid: fit the long side into 44 texels, box-filtered.
		const float scale = std::min(1.0f, 44.0f / float(std::max(w, h)));
		pv.gridW = std::max<uint32_t>(uint32_t(std::round(float(w) * scale)), 1u);
		pv.gridH = std::max<uint32_t>(uint32_t(std::round(float(h) * scale)), 1u);
		pv.rgba.assign(size_t(pv.gridW) * pv.gridH, 0xFF000000u);
		for (uint32_t gy = 0; gy < pv.gridH; ++gy) {
			const uint32_t y0 = gy * h / pv.gridH, y1 = (gy + 1) * h / pv.gridH;
			for (uint32_t gx = 0; gx < pv.gridW; ++gx) {
				const uint32_t x0 = gx * w / pv.gridW, x1 = (gx + 1) * w / pv.gridW;
				uint64_t r = 0, g = 0, b = 0, a = 0, n = 0;
				for (uint32_t y = y0; y < y1; ++y) {
					const uint8_t* row = base + size_t(y) * size_t(rowPitch);
					for (uint32_t x = x0; x < x1; ++x) {
						const uint8_t* px = row + size_t(x) * comps;
						// Missing channels replicate sensibly (R/RG data shows
						// as grayscale-ish rather than black).
						r += px[0];
						g += (comps >= 2) ? px[1] : px[0];
						b += (comps >= 3) ? px[2] : px[0];
						a += (comps >= 4) ? px[3] : 255;
						++n;
					}
				}
				if (n == 0)
					n = 1;
				pv.rgba[size_t(gy) * pv.gridW + gx] =
					(uint32_t(r / n)) | (uint32_t(g / n) << 8) |
					(uint32_t(b / n) << 16) | (uint32_t(a / n) << 24);
			}
		}
		pv.imgW = w;
		pv.imgH = h;
		pv.ok = true;
		char buf[256];
		std::snprintf(buf, sizeof(buf), "%s (%ux%u mip %u)", uri.c_str(),
			raw->baseWidth, std::max<ktx_uint32_t>(raw->baseHeight, 1u),
			level);
		pv.status = buf;
		return pv;
	}

	void EditorApp::DrawDiffusePreview(uint32_t matWorld, const glm::vec4& tint) {
		const TexPreview& pv = ResolveDiffusePreview(matWorld);
		if (!pv.ok) {
			ImGui::ColorButton("##matPreview",
				ImVec4(tint.x, tint.y, tint.z, tint.w),
				ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip,
				ImVec2(64, 64));
			return;
		}
		// Aspect-kept thumbnail (long side 120 px) over a checkerboard so
		// alpha reads honestly; pixels are sRGB bytes, tinted by the
		// baseColor factor exactly like the lit shader multiplies map * factor.
		const float maxPx = 120.0f;
		const float dw = (pv.gridW >= pv.gridH)
			? maxPx : maxPx * float(pv.gridW) / float(pv.gridH);
		const float dh = (pv.gridH >= pv.gridW)
			? maxPx : maxPx * float(pv.gridH) / float(pv.gridW);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 p0 = ImGui::GetCursorScreenPos();
		const float cw = dw / float(pv.gridW), ch = dh / float(pv.gridH);
		for (uint32_t y = 0; y < pv.gridH; ++y) {
			for (uint32_t x = 0; x < pv.gridW; ++x) {
				const uint32_t px = pv.rgba[size_t(y) * pv.gridW + x];
				const float a = float((px >> 24) & 255) / 255.0f;
				const float sr = float(px & 255) / 255.0f * tint.x;
				const float sg = float((px >> 8) & 255) / 255.0f * tint.y;
				const float sb = float((px >> 16) & 255) / 255.0f * tint.z;
				const bool light =
					((int(float(x) * cw / 12.0f) + int(float(y) * ch / 12.0f)) & 1) == 0;
				const float cb = light ? 0.65f : 0.3f;
				dl->AddRectFilled(
					ImVec2(p0.x + float(x) * cw, p0.y + float(y) * ch),
					ImVec2(p0.x + float(x + 1) * cw + 0.5f,
						p0.y + float(y + 1) * ch + 0.5f),
					IM_COL32(
						int((sr * a + cb * (1.0f - a)) * 255.0f),
						int((sg * a + cb * (1.0f - a)) * 255.0f),
						int((sb * a + cb * (1.0f - a)) * 255.0f), 255));
			}
		}
		ImGui::Dummy(ImVec2(dw, dh));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", pv.status.c_str());
	}

	static const char* AlphaModeName(smvk::AlphaMode m) {
		switch (m) {
		case smvk::AlphaMode::Opaque: return "Opaque";
		case smvk::AlphaMode::Mask: return "Mask (cutout)";
		case smvk::AlphaMode::Blend: return "Blend";
		}
		return "?";
	}

	// ---- material editor page (inspector travel) -------------------------
	// m_materialFocus switches the Inspector to a material editor: full
	// attribute set + layer slots + preview, with a back-trail to the node
	// page ("travel between inspectors": layer slots jump focus too).
	void EditorApp::DrawMaterialFocusPage() {
		if (!m_world || m_materialFocus >= m_world->materialLocations.size()) {
			m_materialFocus = UINT32_MAX;
			return;
		}
		const uint32_t mat = m_materialFocus;
		// Breadcrumb: Back to node | the trail of visited materials.
		if (ImGui::Button("< Back to node")) {
			m_materialFocus = UINT32_MAX;
			if (!m_materialTrail.empty()) {
				m_materialFocus = m_materialTrail.back();
				m_materialTrail.pop_back();
			}
		}
		ImGui::SameLine();
		ImGui::Text("Material mat.%u", mat);
		if (!m_materialTrail.empty()) {
			ImGui::TextDisabled("trail:");
			ImGui::SameLine();
			for (size_t i = 0; i < m_materialTrail.size() && i < 6; ++i) {
				ImGui::Text("mat.%u%s", m_materialTrail[i],
					i + 1 < m_materialTrail.size() ? " >" : "");
				ImGui::SameLine();
			}
			ImGui::Text("");
		}
		// THE SHARED BODY: the same material surface the Material tab and
		// the node page render (the consolidation — this page used to
		// carry its own old-presentation copy).
		DrawMaterialBody(mat);
	}

} // namespace editor
