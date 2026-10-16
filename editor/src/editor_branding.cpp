// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Tourmaline Editor branding: window title, window icon
// (tourmaline_logo.png — generated here on first run when absent) and
// the menu-bar gem mark (vector-drawn, no texture interop needed).
//
// The logo doubles as the SDL window icon. It is editor chrome only —
// never part of the scene, the tour or the cook.

#include "editor_app.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image_write.h>
#include <stb_image.h>

#include <SDL3/SDL.h>

#include <imgui.h>

namespace editor {
namespace {

// 64x64 RGBA: faceted tourmaline-green gem on transparent background.
constexpr int kLogoW = 64;
constexpr int kLogoH = 64;

void Put(int x, int y, unsigned char r, unsigned char g, unsigned char b,
	unsigned char a, std::vector<unsigned char>& img) {
	if (x < 0 || y < 0 || x >= kLogoW || y >= kLogoH)
		return;
	unsigned char* p = &img[(size_t(y) * kLogoW + x) * 4];
	p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

// Half-space triangle rasterizer (barycentric-free, good enough).
void FillTri(float x0, float y0, float x1, float y1, float x2, float y2,
	unsigned char r, unsigned char g, unsigned char b,
	std::vector<unsigned char>& img) {
	const float minX = std::max(0.0f, std::floor(std::min({ x0, x1, x2 })));
	const float maxX = std::min(float(kLogoW - 1), std::max({ x0, x1, x2 }));
	const float minY = std::max(0.0f, std::floor(std::min({ y0, y1, y2 })));
	const float maxY = std::min(float(kLogoH - 1), std::max({ y0, y1, y2 }));
	const float d = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
	if (std::fabs(d) < 1e-6f)
		return;
	for (int y = int(minY); y <= int(maxY); ++y) {
		for (int x = int(minX); x <= int(maxX); ++x) {
			const float px = float(x) + 0.5f, py = float(y) + 0.5f;
			const float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / d;
			const float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / d;
			const float w2 = 1.0f - w0 - w1;
			if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f)
				Put(x, y, r, g, b, 255, img);
		}
	}
}

std::vector<unsigned char> MakeLogoRgba() {
	std::vector<unsigned char> img(size_t(kLogoW) * kLogoH * 4, 0);
	// Elongated gem (vertical rhombus, tourmaline green with two facet
	// tones), classic tourmaline crystal shape.
	const float cx = 32.0f;
	FillTri(cx, 4, 12, 30, 52, 30, 0x2E, 0xB8, 0x8A, img);  // crown
	FillTri(cx, 4, 52, 30, cx, 60, 0x27, 0xA8, 0x7C, img);  // right pav.
	FillTri(cx, 4, cx, 60, 12, 30, 0x1F, 0x93, 0x6C, img);   // left pav.
	FillTri(cx, 4, 12, 30, cx, 32, 0x4F, 0xD6, 0xA8, img);  // left table
	FillTri(cx, 4, cx, 32, 52, 30, 0x3C, 0xC9, 0x97, img);  // right table
	return img;
}

std::string LogoPath() {
	namespace fs = std::filesystem;
	// Prefer the exe dir (portable, next to Editor.exe + content).
	std::error_code ec;
	fs::path exe = fs::current_path(ec);
	for (const char* cand : { "tourmaline_logo.png",
			"content/tourmaline_logo.png" }) {
		fs::path p = exe / cand;
		if (!fs::exists(p, ec))
			continue;
		return p.string();
	}
	return (exe / "tourmaline_logo.png").string();
}

} // namespace

void EditorApp::ApplyTourmalineBranding() {
	if (!window)
		return;
	UpdateWindowTitle();

	// Generate the logo once (idempotent), then load it as the icon.
	const std::string path = LogoPath();
	{
		std::error_code ec;
		if (!std::filesystem::is_regular_file(path, ec)) {
			std::vector<unsigned char> img = MakeLogoRgba();
			stbi_write_png(path.c_str(), kLogoW, kLogoH, 4, img.data(),
				kLogoW * 4);
			Log("Tourmaline logo written: " + path);
		}
	}
	int w = 0, h = 0, comp = 0;
	stbi_uc* data = stbi_load(path.c_str(), &w, &h, &comp, 4);
	if (data && w > 0 && h > 0) {
		SDL_Surface* surf = SDL_CreateSurfaceFrom(
			w, h, SDL_PIXELFORMAT_RGBA8888, data, w * 4);
		if (surf) {
			SDL_SetWindowIcon(window, surf);
			SDL_DestroySurface(surf);
		}
	}
	else if (data == nullptr) {
		Log("Tourmaline logo could not be read from '" + path +
			"' (icon skipped).");
	}
	stbi_image_free(data);
}

// Window title: [project — ] current scene plus a Maya-style dirty
// star ("... *" while the working tour is ahead of the last explicit
// save). Change-detected: SDL only hears about it when the text changes.
void EditorApp::UpdateWindowTitle() {
	if (!window)
		return;
	std::string title = "Tourmaline Editor";
	if (!m_projectName.empty())
		title += " [" + m_projectName + "]";
	if (m_world) {
		// The TOUR is the document (not the source asset): prefer its
		// filename; fall back to the loaded source when no tour target
		// exists yet.
		std::string scene;
		if (!TourTarget().empty())
			scene = std::filesystem::path(TourTarget()).filename().string();
		else if (!m_gltfPath.empty())
			scene = std::filesystem::path(m_gltfPath).filename().string();
		if (!scene.empty()) {
			title += " — " + scene;
			if (m_tourDirty)
				title += " *";
		}
	}
	if (title != m_lastWindowTitle) {
		m_lastWindowTitle = title;
		SDL_SetWindowTitle(window, title.c_str());
	}
}

void DrawTourmalineGem(ImDrawList* dl, const ImVec2& p, float h) {
	// Vector twin of the PNG logo: two-tone green gem silhouette.
	const float w = h * 0.62f;
	const ImVec2 c(p.x + w * 0.5f, p.y + h * 0.5f);
	const ImU32 crown = IM_COL32(0x4F, 0xD6, 0xA8, 255);
	const ImU32 pav = IM_COL32(0x27, 0xA8, 0x7C, 255);
	// Crown (upper rhombus halves).
	dl->AddTriangleFilled(ImVec2(c.x, p.y), ImVec2(p.x, c.y - h * 0.04f),
		ImVec2(c.x, p.y + h * 0.46f), crown);
	dl->AddTriangleFilled(ImVec2(c.x, p.y), ImVec2(c.x, p.y + h * 0.46f),
		ImVec2(p.x + w, c.y - h * 0.04f), pav);
	// Pavilion (lower rhombus halves).
	dl->AddTriangleFilled(ImVec2(c.x, p.y + h), ImVec2(p.x, c.y - h * 0.04f),
		ImVec2(c.x, p.y + h * 0.46f), pav);
	dl->AddTriangleFilled(ImVec2(c.x, p.y + h), ImVec2(c.x, p.y + h * 0.46f),
		ImVec2(p.x + w, c.y - h * 0.04f), crown);
}

} // namespace editor
