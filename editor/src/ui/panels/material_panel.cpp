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
	ImTextureID EditorApp::TextureThumb(const smvk::TextureHandle& h) {
		if (h.empty() || !m_context || !m_imgui)
			return 0;
		const uint64_t key = uint64_t(h.index())
			| (uint64_t(h.gen()) << 32);
		for (const auto& [k, id] : m_matThumbs)
			if (k == key)
				return id;
		const void* vkView = m_context->GetTextureVkImageView(h);
		if (!vkView)
			return 0;
		if (m_cubeFaceSampler == UINT32_MAX) {
			m_cubeFaceSampler = m_context->GetOrCreateSampler(
				smvk::SamplerCreateInfo{
					.magFilter = smvk::Filter::Linear,
					.minFilter = smvk::Filter::Linear,
					.mipmapMode = smvk::MipmapMode::Nearest,
					.addressU = smvk::AddressMode::ClampToEdge,
					.addressV = smvk::AddressMode::ClampToEdge,
					.addressW = smvk::AddressMode::ClampToEdge,
					.maxAnisotropy = 1.0f });
		}
		const void* vkSampler =
			m_context->GetVkSamplerOpaque(m_cubeFaceSampler);
		if (!vkSampler)
			return 0;
		VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
			VkSampler(const_cast<void*>(vkSampler)),
			VkImageView(const_cast<void*>(vkView)),
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		if (set == VK_NULL_HANDLE)
			return 0;
		const ImTextureID id = ImTextureID(reinterpret_cast<ImU64>(set));
		m_matThumbs.emplace_back(key, id);
		return id;
	}

	void EditorApp::DropTextureThumbs() {
		if (m_matThumbs.empty())
			return;
		for (auto& [k, id] : m_matThumbs)
			if (id != 0)
				ImGui_ImplVulkan_RemoveTexture(
					reinterpret_cast<VkDescriptorSet>(ImU64(id)));
		m_matThumbs.clear();
	}

	// Browser thumbnail cache: material-ball sidecars and regular source images
	// share one path+mtime keyed GPU texture path. Decode is bounded to prevent
	// a huge source image from turning the asset explorer into a GPU memory sink.
	ImTextureID EditorApp::AssetSidecarThumb(const std::string& imagePath) {
		namespace fs = std::filesystem;
		std::error_code ec;
		if (!m_context || !m_imgui || !fs::is_regular_file(imagePath, ec))
			return 0;
		const auto mt = fs::last_write_time(imagePath, ec);
		if (ec)
			return 0;
		auto it = std::find_if(m_assetThumbs.begin(), m_assetThumbs.end(),
			[&](const AssetThumb& t) { return t.path == imagePath; });
		if (it != m_assetThumbs.end()) {
			if (it->hasMtime && mt == it->mtime && it->imgui != 0)
				return it->imgui;
			if (it->imgui != 0)
				ImGui_ImplVulkan_RemoveTexture(
					reinterpret_cast<VkDescriptorSet>(ImU64(it->imgui)));
			it->tex = {};
			m_assetThumbs.erase(it);
		}
		// Do not decode arbitrarily large/untrusted image files into memory.
		constexpr int maxSourceSide = 8192;
		constexpr int side = 128;
		const auto bytes = fs::file_size(imagePath, ec);
		if (ec || bytes > 32ull * 1024ull * 1024ull)
			return 0;
		int w = 0, h = 0, comp = 0;
		if (!stbi_info(imagePath.c_str(), &w, &h, &comp)
			|| w <= 0 || h <= 0 || w > maxSourceSide || h > maxSourceSide
			|| uint64_t(w) * uint64_t(h) > 16000000ull)
			return 0;
		stbi_uc* data = stbi_load(imagePath.c_str(), &w, &h, &comp, 4);
		if (!data)
			return 0;
		// Letterbox into a fixed-size RGBA tile, preserving aspect ratio.
		const float scale = std::min(float(side) / float(w),
			float(side) / float(h));
		const int dw = std::max(1, int(float(w) * scale));
		const int dh = std::max(1, int(float(h) * scale));
		const int ox = (side - dw) / 2, oy = (side - dh) / 2;
		std::vector<unsigned char> rgba(size_t(side) * side * 4, 0);
		for (int y = 0; y < dh; ++y) {
			const int sy = std::min(h - 1, y * h / dh);
			for (int x = 0; x < dw; ++x) {
				const int sx = std::min(w - 1, x * w / dw);
				const size_t from = (size_t(sy) * size_t(w) + size_t(sx)) * 4;
				const size_t to = (size_t(y + oy) * side + size_t(x + ox)) * 4;
				for (int c = 0; c < 4; ++c)
					rgba[to + c] = data[from + c];
			}
		}
		stbi_image_free(data);
		smvk::ImageCreateInfo ci{};
		ci.extent = smvk::Extent3D{ side, side, 1 };
		ci.format = smvk::Format::RGBA8_SRGB;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = "asset browser thumbnail";
		AssetThumb t;
		t.tex = m_context->CreateTexture(ci, rgba.data());
		if (t.tex.empty())
			return 0;
		const void* vkView = m_context->GetTextureVkImageView(t.tex);
		if (vkView) {
			if (m_cubeFaceSampler == UINT32_MAX) {
				m_cubeFaceSampler = m_context->GetOrCreateSampler(
					smvk::SamplerCreateInfo{
						.magFilter = smvk::Filter::Linear,
						.minFilter = smvk::Filter::Linear,
						.mipmapMode = smvk::MipmapMode::Nearest,
						.addressU = smvk::AddressMode::ClampToEdge,
						.addressV = smvk::AddressMode::ClampToEdge,
						.addressW = smvk::AddressMode::ClampToEdge,
						.maxAnisotropy = 1.0f });
			}
			const void* vkSampler =
				m_context->GetVkSamplerOpaque(m_cubeFaceSampler);
			if (vkSampler) {
				VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
					VkSampler(const_cast<void*>(vkSampler)),
					VkImageView(const_cast<void*>(vkView)),
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				if (set != VK_NULL_HANDLE)
					t.imgui = ImTextureID(reinterpret_cast<ImU64>(set));
			}
		}
		if (t.imgui == 0)
			return 0;
		// A fixed-size LRU-like FIFO bounds descriptor and GPU resource count.
		constexpr size_t maxThumbs = 128;
		if (m_assetThumbs.size() >= maxThumbs) {
			AssetThumb& oldest = m_assetThumbs.front();
			if (oldest.imgui)
				ImGui_ImplVulkan_RemoveTexture(
					reinterpret_cast<VkDescriptorSet>(ImU64(oldest.imgui)));
			oldest.tex = {};
			m_assetThumbs.erase(m_assetThumbs.begin());
		}
		t.path = imagePath;
		t.mtime = mt;
		t.hasMtime = true;
		m_assetThumbs.push_back(std::move(t));
		return m_assetThumbs.back().imgui;
	}

	// ---- THE SHARED MATERIAL BODY (one surface, three hosts) ---------
	// Everything a material can author, in ONE function: the texture
	// slots (the project registry picker + thumbnails), the scalar
	// look (base/metal/rough/emissive, the shared-drag command path),
	// the attribute flags (chips) + alpha, the vertex-color layers, the
	// diffuse preview + the ball-preview sidecar button + the layer
	// travel. The hosts own ONLY their headers:
	//   - the Inspector's node block: the assignment combo + travel
	//   - the Inspector's focus page: the breadcrumb + trail
	//   - the right column's Material tab: the picker (auto-follow)
	// (Before the consolidation the three each carried a subset — the
	// tab had slots+layers but no scalars, the node page had scalars+
	// flags but no slots, the focus page had scalars+layers in the old
	// pre-revamp presentation. Now one body, the attr stubs everywhere.)
	void EditorApp::DrawMaterialBody(uint32_t mat) {
		const attr::ScopedID materialScope("material_body");
		const attr::ScopedID materialId{int(mat)};
		smvk::Material* m = m_world ? m_world->GetMaterial(mat) : nullptr;
		if (!m) {
			ImGui::TextDisabled("Material resolve failed.");
			return;
		}
		tour::Cluster* cl = m_world->clusters.empty()
			? nullptr : m_world->clusters[0].get();

		// ---- Preview + the ball sidecar --------------------------------
		ImGui::Separator();
		DrawDiffusePreview(mat, m->values.diffuse);
		ImGui::SameLine();
		ImGui::BeginGroup();
		{
			const TexPreview& pv = ResolveDiffusePreview(mat);
			if (pv.ok)
				ImGui::TextWrapped("map: %s", pv.uri.c_str());
			else
				ImGui::TextDisabled("map: %s", pv.status.c_str());
			if (ImGui::IsItemHovered()) {
				const glm::ivec4& t0 = m->values.textures;
				const glm::ivec4& t1 = m->values.textures2;
				ImGui::SetTooltip("slots: base %d | mr %d | normal %d "
					"| occ %d | emis %d\nbase %.2f %.2f %.2f a %.2f | "
					"metal %.2f rough %.2f | emis x %.2f",
					t0.x, t0.y, t0.z, t0.w, t1.x,
					m->values.diffuse.x, m->values.diffuse.y,
					m->values.diffuse.z, m->values.diffuse.w,
					m->values.params.y, m->values.params.z,
					m->values.params2.z);
			}
		}
		ImGui::EndGroup();
		ImGui::SameLine();
		{
			// The BALL PREVIEW sidecar: renders the material on a sphere
			// under the fixed default sky into a 256x256 PNG next to the
			// source .material asset (editor_matball.cpp).
			if (ImGui::SmallButton("Ball preview##body")) {
				const auto& loc = m_world->materialLocations[mat];
				std::string uri = loc.cluster
					? std::string(loc.cluster->GetMaterialUri(loc.handle).Str())
					: "";
				std::string sidecar;
				if (!uri.empty()) {
					namespace fs = std::filesystem;
					fs::path host = fs::path(ResolveVfsRoot()) / uri;
					if (uri.size() > 9
						&& host.extension().generic_string() == ".material")
						sidecar = host.generic_string() + ".png";
				}
				if (sidecar.empty())
					sidecar = "exports/matball_"
						+ std::to_string(mat) + ".png";
				QueueMaterialBallPreview(mat, sidecar);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Renders this material on a sphere "
					"under the default sky; the PNG lands next to the "
					"source .material asset (the browser's tile).");
		}

		// ---- PBR texture slots (the registry picker + thumbnails) -----
		ImGui::Separator();
		ImGui::Text("Texture slots");
		struct SlotDef { const char* label; const char* attr; int* slot; };
		int tBC = m->values.textures.x, tMR = m->values.textures.y;
		int tN = m->values.textures.z, tO = m->values.textures.w;
		int tE = m->values.textures2.x, tC = m->values.textures2.y;
		const SlotDef slots[] = {
			{ "Base color", "baseColorTex", &tBC },
			{ "Metallic-rough", "metallicRoughTex", &tMR },
			{ "Normal", "normalTex", &tN },
			{ "Occlusion", "occlusionTex", &tO },
			{ "Cavity", "cavityTex", &tC },
			{ "Emissive", "emissiveTex", &tE },
		};
		std::vector<std::pair<smvk::StringViewT, smvk::TextureHandle>> texList;
		if (cl)
			texList = cl->ListTextureUris();
		struct TexAsset { std::string name; std::string abs; std::string guid; };
		std::vector<TexAsset> texAssets;
		if (m_assetDb.IsOpen()) {
			std::vector<smvk::cache::AssetRecord> rows;
			std::string derr;
			if (m_assetDb.All(rows, derr)) {
				texAssets.reserve(rows.size());
				for (const auto& r : rows) {
					if (r.kind != "texture")
						continue;
					const std::string abs = m_assetDb.ResolvePath(r.path);
					if (abs.empty())
						continue;
					TexAsset t;
					t.name = std::filesystem::path(abs)
						.filename().generic_string();
					t.abs = abs;
					t.guid = r.guid;
					texAssets.push_back(std::move(t));
				}
				std::sort(texAssets.begin(), texAssets.end(),
					[](const TexAsset& a, const TexAsset& b) {
						return a.name < b.name;
					});
			}
		}
		for (const SlotDef& s : slots) {
			ImGui::PushID(s.attr);
			std::string preview = *s.slot >= 0
				? ("tex." + std::to_string(*s.slot)) : std::string("(none)");
			smvk::TextureHandle slotTex{};
			if (*s.slot >= 0)
				for (const auto& [uri, th] : texList)
					if (int(th.index()) == *s.slot) {
						slotTex = th;
						preview = std::filesystem::path(uri.Str())
							.filename().generic_string();
						break;
					}
			if (!slotTex.empty()) {
				const ImTextureID tid = TextureThumb(slotTex);
				if (tid != 0) {
					ImGui::Image(tid, ImVec2(48, 48));
					ImGui::SameLine();
				}
			}
			if (ImGui::BeginCombo(s.label, preview.c_str())) {
				if (ImGui::Selectable("(none)", *s.slot < 0)) {
					EditCmd c{};
					c.verb = EditVerb::SetMatAttr;
					c.matWorld = mat;
					c.attr = s.attr;
					c.number = -1.0;
					ReadSharedMatAttr(mat, s.attr, c.beforeVec4, c.beforeNum);
					Do(c);
				}
				if (texAssets.empty())
					ImGui::TextDisabled("(no texture assets in the project"
						" — import or create them in the asset browser)");
				for (const TexAsset& ta : texAssets) {
					if (ImGui::Selectable(ta.name.c_str(),
							preview == ta.name)) {
						int poolIdx = -1;
						for (const auto& [uri, th] : texList)
							if (uri.Str() == ta.abs) {
								poolIdx = int(th.index());
								break;
							}
						if (poolIdx < 0)
							poolIdx = AddTextureFromFile(ta.abs);
						if (poolIdx < 0)
							continue;
						EditCmd c{};
						c.verb = EditVerb::SetMatAttr;
						c.matWorld = mat;
						c.attr = s.attr;
						c.number = double(poolIdx);
						ReadSharedMatAttr(mat, s.attr, c.beforeVec4,
							c.beforeNum);
						Do(c);
					}
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s\n%s", ta.abs.c_str(),
							ta.guid.c_str());
				}
				ImGui::EndCombo();
			}
			ImGui::PopID();
		}

		// ---- The scalar look (shared edit, the drag command path) ----
		ImGui::Separator();
		{
			glm::vec4 bc = m->values.diffuse;
			if (attr::Color("Base color", &bc.x,
					("Shared edit — affects every node using mat."
						+ std::to_string(mat)).c_str())) {
				if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetMatAttr ||
					m_pending.cmd.matWorld != mat || m_pending.cmd.attr != "baseColor"))
					FinalizePending();
				if (!m_pending.active) {
					m_pending.active = true;
					m_pending.cmd = EditCmd{};
					m_pending.cmd.verb = EditVerb::SetMatAttr;
					m_pending.cmd.matWorld = mat;
					m_pending.cmd.attr = "baseColor";
					ReadSharedMatAttr(mat, "baseColor",
						m_pending.cmd.beforeVec4, m_pending.cmd.beforeNum);
				}
				WriteSharedMatAttr(mat, "baseColor", bc, 0.0);
				m_pending.cmd.vec4 = bc;
			}
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
				m_pending.cmd.attr == "baseColor")
				FinishDrag();
			float metallic = m->values.params.y;
			if (attr::Slider("Metallic", &metallic, 0.0f, 1.0f, "%.2f",
					"Metallic factor (0 dielectric .. 1 metal)")) {
				if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetMatAttr ||
					m_pending.cmd.matWorld != mat || m_pending.cmd.attr != "metallic"))
					FinalizePending();
				if (!m_pending.active) {
					m_pending.active = true;
					m_pending.cmd = EditCmd{};
					m_pending.cmd.verb = EditVerb::SetMatAttr;
					m_pending.cmd.matWorld = mat;
					m_pending.cmd.attr = "metallic";
					ReadSharedMatAttr(mat, "metallic",
						m_pending.cmd.beforeVec4, m_pending.cmd.beforeNum);
				}
				WriteSharedMatAttr(mat, "metallic", glm::vec4(0), metallic);
				m_pending.cmd.number = metallic;
			}
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
				m_pending.cmd.attr == "metallic")
				FinishDrag();
			float rough = m->values.params.z;
			if (attr::Slider("Roughness", &rough, 0.04f, 1.0f, "%.2f",
					"Perceptual roughness (0.04 mirror .. 1 diffuse)")) {
				if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetMatAttr ||
					m_pending.cmd.matWorld != mat || m_pending.cmd.attr != "roughness"))
					FinalizePending();
				if (!m_pending.active) {
					m_pending.active = true;
					m_pending.cmd = EditCmd{};
					m_pending.cmd.verb = EditVerb::SetMatAttr;
					m_pending.cmd.matWorld = mat;
					m_pending.cmd.attr = "roughness";
					ReadSharedMatAttr(mat, "roughness",
						m_pending.cmd.beforeVec4, m_pending.cmd.beforeNum);
				}
				WriteSharedMatAttr(mat, "roughness", glm::vec4(0), rough);
				m_pending.cmd.number = rough;
			}
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
				m_pending.cmd.attr == "roughness")
				FinishDrag();
			// ---- Fine-scale cavity strength (indirect lighting only) ----
            float cavityStrength = m->values.textures2.z < 0
                ? 1.0f : float(m->values.textures2.z) / 1000.0f;
            if (attr::Slider("Cavity strength", &cavityStrength, 0.0f, 1.0f,
                    "%.2f", "Fine-scale cavity map strength on indirect lighting")) {
                if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetMatAttr ||
                    m_pending.cmd.matWorld != mat || m_pending.cmd.attr != "cavityStrength"))
                    FinalizePending();
                if (!m_pending.active) {
                    m_pending.active = true;
                    m_pending.cmd = EditCmd{};
                    m_pending.cmd.verb = EditVerb::SetMatAttr;
                    m_pending.cmd.matWorld = mat;
                    m_pending.cmd.attr = "cavityStrength";
                    ReadSharedMatAttr(mat, "cavityStrength",
                        m_pending.cmd.beforeVec4, m_pending.cmd.beforeNum);
                }
                WriteSharedMatAttr(mat, "cavityStrength", glm::vec4(0), cavityStrength);
                m_pending.cmd.number = cavityStrength;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
                m_pending.cmd.attr == "cavityStrength")
                FinishDrag();
            // ---- EMISSIVE COLOR + STRENGTH (HDR — the BLOOM link) ----
			// The strength is the HDR radiance multiplier, the SAME
			// scale the bloom threshold thresholds (pre-exposure HDR;
			// the sunlit default thr=400, the panel slider to 4000):
			// emissive * strength must CROSS the threshold for the glow
			// to bloom (the bloom pass prefilters at threshold/knee —
			// the exposure-relative scaling is the renderer's). The OLD
			// combined control rode the strength on ColorEdit4's A
			// slider — capped at 1.0, an emissive could NEVER cross a
			// 400-threshold: "emissive not considered in lit view" —
			// it read as a faint additive tint and never glowed.
			glm::vec4 em(m->values.emissiveFactor.x,
				m->values.emissiveFactor.y, m->values.emissiveFactor.z,
				1.0f);
			if (attr::Color("Emissive color", &em.x,
					"The emissive TINT (RGB); the intensity is the "
					"strength row")) {
				if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetMatAttr ||
					m_pending.cmd.matWorld != mat || m_pending.cmd.attr != "emissive"))
					FinalizePending();
				if (!m_pending.active) {
					m_pending.active = true;
					m_pending.cmd = EditCmd{};
					m_pending.cmd.verb = EditVerb::SetMatAttr;
					m_pending.cmd.matWorld = mat;
					m_pending.cmd.attr = "emissive";
					ReadSharedMatAttr(mat, "emissive",
						m_pending.cmd.beforeVec4, m_pending.cmd.beforeNum);
				}
				// The color edit PRESERVES the current strength: the
				// picker's own A slider (0..1) would clobber an HDR
				// strength back to a faint tint — the strength row owns
				// that field; write the picker's RGB with the live
				// strength as the w.
				WriteSharedMatAttr(mat, "emissive",
					glm::vec4(em.x, em.y, em.z,
						m->values.params2.z), 0.0);
				m_pending.cmd.vec4 = glm::vec4(em.x, em.y, em.z,
					m->values.params2.z);
			}
			if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active &&
				m_pending.cmd.attr == "emissive")
				FinishDrag();
			{
				float strength = m->values.params2.z;
				if (attr::Float("Emissive strength", &strength, 5.0f, 0.0f,
						4000.0f, "%.1f",
						"HDR radiance multiplier — same HDR scale as the "
						"bloom threshold: past it, the emissive BLOOMS "
						"(sunlit scenes read ~400; 1.0 is a faint tint)")) {
					if (m_pending.active && (m_pending.cmd.verb != EditVerb::SetMatAttr ||
						m_pending.cmd.matWorld != mat
						|| m_pending.cmd.attr != "emissiveStrength"))
						FinalizePending();
					if (!m_pending.active) {
						m_pending.active = true;
						m_pending.cmd = EditCmd{};
						m_pending.cmd.verb = EditVerb::SetMatAttr;
						m_pending.cmd.matWorld = mat;
						m_pending.cmd.attr = "emissiveStrength";
						ReadSharedMatAttr(mat, "emissiveStrength",
							m_pending.cmd.beforeVec4,
							m_pending.cmd.beforeNum);
					}
					WriteSharedMatAttr(mat, "emissiveStrength",
						glm::vec4(0), strength);
					m_pending.cmd.number = strength;
				}
				if (ImGui::IsItemDeactivatedAfterEdit() && m_pending.active
					&& m_pending.cmd.attr == "emissiveStrength")
					FinishDrag();
			}
		}

		// ---- Attribute flags + alpha ------------------------------------
		ImGui::SeparatorText("Material properties");
		{
			if (attr::Bool("DS", m->doubleSided,
					"DOUBLE SIDED — both faces shade")) {
				EditCmd c{};
				c.verb = EditVerb::SetMatAttr;
				c.matWorld = mat;
				c.attr = "doubleSided";
				c.number = m->doubleSided ? 0.0 : 1.0;
				ReadSharedMatAttr(mat, "doubleSided",
					c.beforeVec4, c.beforeNum);
				Do(c);
			}
			if (attr::Bool("Shadow", m->castsShadows,
					"CASTS SHADOWS — off excludes it from the shadow "
					"passes (decal-style)")) {
				EditCmd c{};
				c.verb = EditVerb::SetMatAttr;
				c.matWorld = mat;
				c.attr = "castsShadows";
				c.number = m->castsShadows ? 0.0 : 1.0;
				ReadSharedMatAttr(mat, "castsShadows",
					c.beforeVec4, c.beforeNum);
				Do(c);
			}
			// Sun contact-shadow receive gate (the emissiveFactor.w flags
			// word - see mesh.h); a NEW row (the one above is three wide).
			{
				const bool recvContact = (smvk::MaterialFlagsFromWord(
					m->values.emissiveFactor.w)
					& smvk::kMaterialFlagReceiveContactShadows) != 0;
				if (attr::Bool("Contact", recvContact,
						"RECEIVES SUN CONTACT SHADOWS — off excludes this "
						"material from the contact march's visibility (the "
						"cascade shadow stays)")) {
					EditCmd c{};
					c.verb = EditVerb::SetMatAttr;
					c.matWorld = mat;
					c.attr = "contactShadows";
					c.number = recvContact ? 0.0 : 1.0;
					ReadSharedMatAttr(mat, "contactShadows",
						c.beforeVec4, c.beforeNum);
					Do(c);
				}
			}
			int alpha = int(m->alphaMode);
			if (attr::Enum("Alpha", &alpha, "Opaque\0Mask (cutout)\0Blend\0",
					"Alpha: the blend/cutout mode (the cut value reads "
					"here once the Mask editor lands)")) {
				EditCmd c{};
				c.verb = EditVerb::SetMatAttr;
				c.matWorld = mat;
				c.attr = "alphaMode";
				c.number = double(alpha);
				ReadSharedMatAttr(mat, "alphaMode",
					c.beforeVec4, c.beforeNum);
				Do(c);
			}
		}

		// Layered materials are a distinct editor presentation. Plain
		// materials never expose inactive layer slots.
		const bool layered = m->values.layerMaterials.w > 0;
		bool editLayers = layered;
		if (attr::Check("Layered material", &editLayers,
				"Enable vertex-color blending with up to three other materials")) {
			glm::ivec4 lm = m->values.layerMaterials;
			lm.w = editLayers ? std::max(lm.w, 1) : 0;
			EditCmd c{};
			c.verb = EditVerb::SetMatAttr;
			c.matWorld = mat;
			c.attr = "layers";
			ReadSharedMatAttr(mat, "layers", c.beforeVec4, c.beforeNum);
			c.vec4 = glm::vec4(float(lm.x), float(lm.y),
				float(lm.z), float(lm.w));
			c.number = double(lm.w);
			Do(c);
		}
		if (editLayers) {
			ImGui::SeparatorText("Layered material");
			ImGui::TextDisabled("Vertex-color layer weights");

			glm::ivec4 lm = m->values.layerMaterials;
			const uint32_t count = uint32_t(m_world->materialLocations.size());
			for (int slot = 0; slot < 3; ++slot) {
				ImGui::PushID(slot);
				int cur = lm[slot];
				std::string preview = (cur >= 0 && uint32_t(cur) < count)
					? ("mat." + std::to_string(cur)) : std::string("(none)");
				char comboLabel[32];
				std::snprintf(comboLabel, sizeof(comboLabel), "Layer %d",
					slot + 1);
				if (ImGui::BeginCombo(comboLabel, preview.c_str())) {
					if (ImGui::Selectable("(none)", cur < 0))
						cur = -1;
					for (uint32_t i = 0; i < count; ++i) {
						if (i == mat)
							continue; // this material is layer 0
						const auto& loc = m_world->materialLocations[i];
						std::string uri = loc.cluster
							? std::string(loc.cluster->GetMaterialUri(loc.handle).Str())
							: "";
						char item[320];
						std::snprintf(item, sizeof(item), "%u: %s", i,
							uri.empty() ? "(unnamed)" : uri.c_str());
						if (ImGui::Selectable(item, cur == int(i)))
							cur = int(i);
					}
					ImGui::EndCombo();
				}
				ImGui::PopID();
				if (cur != lm[slot]) {
					glm::ivec4 nl = lm;
					nl[slot] = cur;
					EditCmd c{};
					c.verb = EditVerb::SetMatAttr;
					c.matWorld = mat;
					c.attr = "layers";
					glm::vec4 v4;
					double num = 0.0;
					ReadSharedMatAttr(mat, "layers", v4, num);
					c.beforeVec4 = v4;
					c.beforeNum = num;
					c.vec4 = glm::vec4(float(nl.x), float(nl.y),
						float(nl.z), float(nl.w));
					c.number = double(nl.w);
					Do(c);
				}
			}
			int layerCount = lm.w;
			if (attr::Int("Active layers", &layerCount, 0, 4,
					"RGBA vertex weights blend layers 1..3 over this "
					"material (layer 0); count 0 = the legacy plain tint")) {
				glm::ivec4 nl = lm;
				nl.w = layerCount;
				EditCmd c{};
				c.verb = EditVerb::SetMatAttr;
				c.matWorld = mat;
				c.attr = "layers";
				glm::vec4 v4;
				double num = 0.0;
				ReadSharedMatAttr(mat, "layers", v4, num);
				c.beforeVec4 = v4;
				c.beforeNum = num;
				c.vec4 = glm::vec4(float(nl.x), float(nl.y),
					float(nl.z), float(nl.w));
				c.number = double(nl.w);
				Do(c);
			}
			// Travel: jump to a layer material (the trail keeps the way
			// back; the jump raises the Inspector's focus page).
			for (int slot = 0; slot < 3; ++slot) {
				const int target = m->values.layerMaterials[slot];
				if (target < 0 || uint32_t(target) >=
					m_world->materialLocations.size())
					continue;
				ImGui::PushID(100 + slot);
				if (ImGui::SmallButton("Go to layer material >>")) {
					m_materialTrail.push_back(m_materialFocus);
					m_materialFocus = uint32_t(target);
				}
				ImGui::PopID();
			}
		}
	}

	void EditorApp::DrawMaterialEditor() {
		if (!m_world || m_world->materialLocations.empty()) {
			ImGui::TextDisabled("No materials (open a scene).");
			return;
		}
		// AUTO-FOLLOW THE SELECTION: the tab positions itself on the
		// CURRENT SELECTION's material — picking a node in the viewport
		// (or the outliner) lands its material here (the manual combo
		// still works between selections: the follow only fires when the
		// SELECTION's material CHANGED, not on every frame).
		{
			uint32_t selMat = UINT32_MAX;
			if (m_selected != UINT32_MAX) {
				if (const MaterialRow* row =
					FindMaterial(m_world->scene, m_selected))
					selMat = row->material;
			}
			if (selMat != m_matEditorSelFromSel) {
				m_matEditorSelFromSel = selMat;
				if (selMat != UINT32_MAX
					&& selMat < m_world->materialLocations.size())
					m_matEditorSel = selMat;
			}
		}
		// Material picker.
		if (m_matEditorSel == UINT32_MAX
			|| m_matEditorSel >= m_world->materialLocations.size())
			m_matEditorSel = m_defaultMatWorld != UINT32_MAX
			? m_defaultMatWorld : 0;
		{
			const auto& loc = m_world->materialLocations[m_matEditorSel];
			std::string preview = "mat." + std::to_string(m_matEditorSel);
			if (loc.cluster) {
				const std::string uri =
					std::string(loc.cluster->GetMaterialUri(loc.handle).Str());
				if (!uri.empty())
					preview += " " + uri;
			}
			if (ImGui::BeginCombo("Material", preview.c_str())) {
				for (uint32_t i = 0; i < m_world->materialLocations.size(); ++i) {
					const auto& l2 = m_world->materialLocations[i];
					std::string uri = l2.cluster
						? std::string(l2.cluster->GetMaterialUri(l2.handle).Str())
						: "";
					char item[320];
					std::snprintf(item, sizeof(item), "%u: %s", i,
						uri.empty() ? "(unnamed)" : uri.c_str());
					if (ImGui::Selectable(item, m_matEditorSel == i))
						m_matEditorSel = i;
				}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Focus >>")) {
				m_materialTrail.push_back(m_materialFocus);
				m_materialFocus = m_matEditorSel;
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Opens the full material page in the "
					"Inspector (emissive, alpha, flags, layer travel).");
		}
		// THE SHARED BODY: slots + scalars + flags + layers + previews —
		// the same surface the node page and the focus page render.
		DrawMaterialBody(m_matEditorSel);
	}

} // namespace editor
