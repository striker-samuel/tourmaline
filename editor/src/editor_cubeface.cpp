// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Cubemap capture face strip (cook dialog): live KTX2 face previews.
// While a cubemap cook runs, finished captures appear here as tonemapped
// face strips parsed from the loose sidecars (never PNG); with no cook
// running the strip shows the last-baked set. Manifest-independent, so
// single captures (no manifest row) display like multi-capture sets.
//
// Pixels: the cooker writes uncompressed RGBA32F KTX2 (level 0 per face);
// this TU tonemaps face 0..5 level 0 into one RGBA8 strip (6 faces wide)
// and registers it like the SVG icons (CreateTexture + backend user
// texture). Progress fractions come from [CUBE] lines (see PollCookProcess),
// not from here.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include <ktx.h>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include "editor_app.h"

namespace editor {

namespace {
constexpr int kFaceStripFaces = 6;
constexpr size_t kMaxStripCaptures = 24;
} // namespace

void EditorApp::DropCubeFaces() {
	for (CubeFaceView& v : m_cubeFaces) {
		if (v.imgui != 0) {
			ImGui_ImplVulkan_RemoveTexture(
				reinterpret_cast<VkDescriptorSet>(ImU64(v.imgui)));
			v.imgui = 0;
		}
		v.tex = {};
	}
	m_cubeFaces.clear();
}

// Sidecar roots + capture base, mirroring the manifest reader
// (CubemapProbeRowsFromFile) and the completion adopter
// (RecordBakeAssetsFromCook: the out-pak stem - the legacy naming rule;
// capture sets are NAMELESS now).
bool EditorApp::CubeWatchSpec(std::string& dirOut,
	std::string& baseOut) const {
	namespace fs = std::filesystem;
	std::string base;
	if (!m_cook.outPak.empty()) {
		base = fs::path(m_cook.outPak).stem().generic_string();
	}
	if (base.empty())
		return false;
	std::string dir;
	if (!m_projectRoot.empty()) {
		const fs::path cand =
			fs::path(m_projectRoot) / "content" / "textures" / "cubemaps";
		std::error_code ec;
		if (fs::is_directory(cand, ec)) {
			dir = cand.generic_string();
		}
	}
	if (dir.empty())
		dir = (fs::path("content") / "textures" / "cubemaps").generic_string();
	dirOut = dir;
	baseOut = base;
	return true;
}

static bool LoadKtxFaceStrip(const std::string& path,
	std::vector<uint8_t>& rgbaOut, int& sizeOut) {
	namespace fs = std::filesystem;
	std::error_code ec;
	const auto bytes = fs::file_size(path, ec);
	if (ec || bytes == 0 || bytes > 512ull * 1024ull * 1024ull)
		return false;
	ktxTexture2* tex = nullptr;
	if (ktxTexture2_CreateFromNamedFile(path.c_str(),
			KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &tex)
		!= KTX_SUCCESS || !tex) {
		return false;
	}
	struct Guard {
		ktxTexture2* t;
		~Guard() { if (t) ktxTexture2_Destroy(t); }
	} guard{ tex };
	// Uncompressed HDR captures only (the cooker's KTX2 payloads);
	// transcoded/compressed rows have no CPU-readable pixels.
	if (tex->vkFormat != (ktx_uint32_t)VK_FORMAT_R32G32B32A32_SFLOAT)
		return false;
	if (tex->numFaces != kFaceStripFaces || tex->baseWidth == 0
		|| tex->baseWidth != tex->baseHeight
		|| tex->baseWidth > 1024 || !tex->pData)
		return false;
	const uint32_t S = tex->baseWidth;
	// Auto-exposure: mean luminance over a stride, map to mid gray.
	double lumSum = 0.0;
	uint64_t lumN = 0;
	for (uint32_t face = 0; face < uint32_t(kFaceStripFaces); ++face) {
		ktx_size_t off = 0;
		if (ktxTexture_GetImageOffset(reinterpret_cast<ktxTexture*>(tex),
				0, 0, face, &off) != KTX_SUCCESS)
			return false;
		const float* px =
			reinterpret_cast<const float*>(tex->pData + off);
		for (uint32_t y = 0; y < S; y += 4) {
			for (uint32_t x = 0; x < S; x += 4) {
				const float* t = px + (size_t(y) * S + x) * 4;
				lumSum += 0.2126 * t[0] + 0.7152 * t[1] + 0.0722 * t[2];
				++lumN;
			}
		}
	}
	const double mean = lumN > 0 ? lumSum / double(lumN) : 0.0;
	const float scale =
		mean > 1e-6 ? float(0.35 / mean) : 1.0f;
	rgbaOut.resize(size_t(S) * S * kFaceStripFaces * 4);
	for (uint32_t face = 0; face < uint32_t(kFaceStripFaces); ++face) {
		ktx_size_t off = 0;
		if (ktxTexture_GetImageOffset(reinterpret_cast<ktxTexture*>(tex),
				0, 0, face, &off) != KTX_SUCCESS)
			return false;
		const float* px =
			reinterpret_cast<const float*>(tex->pData + off);
		uint8_t* dst = rgbaOut.data() + size_t(face) * S * S * 4;
		for (uint32_t i = 0; i < S * S; ++i) {
			for (int c = 0; c < 3; ++c) {
				const float v = 1.0f - std::exp(-px[i * 4 + c] * scale);
				const float g = std::pow(std::max(v, 0.0f), 1.0f / 2.2f);
				dst[i * 4 + c] = uint8_t(
					std::min(std::max(g, 0.0f), 1.0f) * 255.0f);
			}
			dst[i * 4 + 3] = 255;
		}
	}
	sizeOut = int(S);
	return true;
}

void EditorApp::PollCubeFaces() {
	// Throttle: a directory scan every ~30 frames; loads only
	// new/changed sidecars (mtime compare).
	if ((++m_cubeFacePollFrame % 30) != 0 || !m_context || !m_imgui)
		return;
	std::string dir, base;
	if (!CubeWatchSpec(dir, base))
		return;
	namespace fs = std::filesystem;
	std::error_code ec;
	if (!fs::is_directory(dir, ec))
		return;
	// Current files, sorted (deterministic strip order).
	std::vector<std::pair<std::string, fs::path>> found;
	for (const auto& e : fs::directory_iterator(dir, ec)) {
		if (ec)
			break;
		if (!e.is_regular_file(ec))
			continue;
		const std::string name = e.path().filename().generic_string();
		if (name.size() < base.size() + 5
			|| name.compare(0, base.size(), base) != 0
			|| name.compare(name.size() - 5, 5, ".ktx2") != 0)
			continue;
		found.emplace_back(name, e.path());
	}
	std::sort(found.begin(), found.end());
	if (found.size() > kMaxStripCaptures)
		found.resize(kMaxStripCaptures);
	// Drop views whose file vanished or changed.
	for (auto it = m_cubeFaces.begin(); it != m_cubeFaces.end();) {
		auto fit = std::find_if(found.begin(), found.end(),
			[&](const auto& f) { return f.first == it->name; });
		bool stale = fit == found.end();
		if (!stale) {
			std::error_code tec;
			const auto mt = fs::last_write_time(fit->second, tec);
			stale = tec || !it->hasMtime || mt != it->mtime;
			if (!stale) {
				found.erase(fit);
				++it;
				continue;
			}
			it->mtime = mt;
			it->hasMtime = !tec;
		}
		if (it->imgui != 0) {
			ImGui_ImplVulkan_RemoveTexture(
				reinterpret_cast<VkDescriptorSet>(ImU64(it->imgui)));
			it->imgui = 0;
		}
		it->tex = {};
		if (stale && fit == found.end()) {
			it = m_cubeFaces.erase(it);
		}
		else {
			++it;
		}
	}
	// Load what remains (new or changed).
	for (const auto& [name, path] : found) {
		auto it = std::find_if(m_cubeFaces.begin(), m_cubeFaces.end(),
			[&](const CubeFaceView& v) { return v.name == name; });
		CubeFaceView* view = nullptr;
		if (it == m_cubeFaces.end()) {
			m_cubeFaces.push_back(CubeFaceView{});
			view = &m_cubeFaces.back();
			view->name = name;
			view->ktxPath = path.generic_string();
			std::error_code tec;
			view->mtime = fs::last_write_time(path, tec);
			view->hasMtime = !tec;
		}
		else {
			view = &(*it);
		}
		if (view->imgui != 0)
			continue; // already loaded and current
		std::vector<uint8_t> rgba;
		int size = 0;
		if (!LoadKtxFaceStrip(path.generic_string(), rgba, size))
			continue; // compressed/unreadable row: skip silently
		smvk::ImageCreateInfo ci{};
		ci.extent = smvk::Extent3D{ uint32_t(size * kFaceStripFaces),
			uint32_t(size), 1 };
		ci.format = smvk::Format::RGBA8_UNorm;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = "cubeface strip";
		view->tex = m_context->CreateTexture(ci, rgba.data());
		if (view->tex.empty())
			continue;
		const void* vkView = m_context->GetTextureVkImageView(view->tex);
		if (!vkView)
			continue;
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
			continue;
		VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
			VkSampler(const_cast<void*>(vkSampler)),
			VkImageView(const_cast<void*>(vkView)),
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		if (set == VK_NULL_HANDLE)
			continue;
		view->imgui = ImTextureID(reinterpret_cast<ImU64>(set));
		view->faceSize = size;
	}
}

void EditorApp::DrawCubeFaceStrip() {
	// Cook dialog section: one strip row per baked capture (faces
	// +X -X +Y -Y +Z -Z left to right) + pending pin count. Runs its
	// own poll so faces appear while the cook runs.
	PollCubeFaces();
	if (m_cubeFaces.empty()) {
		size_t pins = m_world ? m_world->scene.cubemapForNode.size() : 0;
		ImGui::TextDisabled("capture faces: none baked yet (%zu pin%s)",
			pins, pins == 1 ? "" : "s");
		return;
	}
	ImGui::TextDisabled("capture faces (+X -X +Y -Y +Z -Z):");
	for (const CubeFaceView& v : m_cubeFaces) {
		ImGui::Text("%s", v.name.c_str());
		ImGui::SameLine();
		if (v.imgui != 0 && v.faceSize > 0) {
			const float h = 56.0f;
			ImGui::Image(v.imgui,
				ImVec2(h * float(kFaceStripFaces), h));
		}
		else {
			ImGui::TextDisabled("(unreadable row)");
		}
	}
}

} // namespace editor
