// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Tourmaline Editor branding: window title, window icon
// (tourmaline_logo.png — the checked-in branding raster, synced next to
// Editor.exe by editor/CMakeLists.txt; a hand-rasterized fallback is
// generated here only when the file is absent) and the menu-bar crystal
// mark (vector twin of the branding mark, no texture interop needed).
//
// The logo doubles as the SDL window icon. It is editor chrome only —
// never part of the scene, the tour or the cook.
//
// The mark: the "Crystal 01" brand (branding/tourmaline-icon.svg) — a
// teal rounded frame bracketing a faceted tourmaline stone that grades
// orange -> magenta -> violet. The geometry below is a flat-color
// polygonal reduction of the SVG's paths (the gradients collapse to
// per-facet average tones), small enough to stay readable at menu-bar
// size while matching the packaged logo exactly in silhouette.

#include "editor_app.h"

#include <algorithm>
#include <cmath>
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

// ---------------------------------------------------------------------------
// The mark geometry (512x512 icon space, the SVG's post-transform
// coordinates). Two pieces: the teal frame (an open bracket swept as a
// thick stroke) and the faceted stone (a hexagonal silhouette tiled by
// five convex facets).
// ---------------------------------------------------------------------------

// Frame stroke centerline (the bracket), ~36 units thick in icon space.
constexpr ImVec2 kFramePath[] = {
	{ 370.0f, 150.0f }, { 255.0f, 118.0f }, { 155.0f, 145.0f },
	{ 100.0f, 210.0f }, { 70.0f, 290.0f }, { 78.0f, 355.0f },
	{ 120.0f, 382.0f },
};
constexpr int kFramePathCount = int(sizeof(kFramePath) / sizeof(kFramePath[0]));
constexpr float kFrameThickness = 36.0f;

// Stone silhouette (hexagon, clockwise from the top tip).
constexpr ImVec2 kStoneHex[] = {
	{ 438.0f, 135.0f }, { 466.0f, 254.0f }, { 421.0f, 402.0f },
	{ 286.0f, 427.0f }, { 128.0f, 392.0f }, { 199.0f, 276.0f },
};
constexpr int kStoneHexCount = int(sizeof(kStoneHex) / sizeof(kStoneHex[0]));

// The facets (flat average tones of the SVG gradients). Union = the
// hexagon above; every facet is convex so both the ImGui path and the
// software rasterizer can fill them with the same half-space test.
struct Facet {
	ImVec2 pts[5];
	int count;
	unsigned char r, g, b;
};
constexpr Facet kFacets[] = {
	// Crown / top-left table (crown gradient ~ #FFC05C).
	{ { { 438.0f, 135.0f }, { 411.0f, 212.0f }, { 220.0f, 275.0f } }, 3,
		0xFF, 0xC0, 0x5C },
	// Right side (side gradient #E84822 -> #B22960, mid ~ #CD3A41).
	{ { { 438.0f, 135.0f }, { 469.0f, 253.0f }, { 421.0f, 403.0f },
	    { 376.0f, 363.0f }, { 411.0f, 212.0f } }, 5,
		0xCD, 0x3A, 0x41 },
	// Center (middle gradient mid ~ #E73B70).
	{ { { 220.0f, 275.0f }, { 411.0f, 212.0f }, { 376.0f, 363.0f } }, 3,
		0xE7, 0x3B, 0x70 },
	// Left (left gradient #A51C85 -> #4C086F, mid ~ #79127A).
	{ { { 220.0f, 275.0f }, { 376.0f, 363.0f }, { 127.0f, 393.0f } }, 3,
		0x79, 0x12, 0x7A },
	// Base (base gradient #5D0A75 -> #36085E, mid ~ #4A0A6A).
	{ { { 127.0f, 393.0f }, { 286.0f, 427.0f }, { 421.0f, 402.0f },
	    { 376.0f, 363.0f } }, 4,
		0x4A, 0x0A, 0x6A },
};
constexpr int kFacetCount = int(sizeof(kFacets) / sizeof(kFacets[0]));

constexpr unsigned char kTeal[3] = { 0x00, 0xA9, 0x9B }; // frame

// The mark's bounding box in icon space (frame outer left/top to the
// stone's right/bottom) — the mapping source for every draw below.
constexpr float kMarkMinX = 44.0f;
constexpr float kMarkMinY = 103.0f;
constexpr float kMarkW = 425.0f;
constexpr float kMarkH = 324.0f;

// ---------------------------------------------------------------------------
// Software rasterizer (the offline fallback PNG): convex polygons via a
// winding-agnostic half-space test, plus thick segments as rotated quads.
// ---------------------------------------------------------------------------

constexpr int kLogoSize = 64; // square fallback icon

void Put(int x, int y, unsigned char r, unsigned char g, unsigned char b,
	unsigned char a, std::vector<unsigned char>& img) {
	if (x < 0 || y < 0 || x >= kLogoSize || y >= kLogoSize)
		return;
	unsigned char* p = &img[(size_t(y) * kLogoSize + x) * 4];
	p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

void FillConvex(const ImVec2* pts, int count, unsigned char r,
	unsigned char g, unsigned char b, std::vector<unsigned char>& img) {
	if (count < 3)
		return;
	float minX = pts[0].x, maxX = pts[0].x, minY = pts[0].y, maxY = pts[0].y;
	for (int i = 1; i < count; ++i) {
		minX = std::min(minX, pts[i].x); maxX = std::max(maxX, pts[i].x);
		minY = std::min(minY, pts[i].y); maxY = std::max(maxY, pts[i].y);
	}
	const int x0 = std::max(0, int(std::floor(minX)));
	const int x1 = std::min(kLogoSize - 1, int(std::ceil(maxX)));
	const int y0 = std::max(0, int(std::floor(minY)));
	const int y1 = std::min(kLogoSize - 1, int(std::ceil(maxY)));
	for (int y = y0; y <= y1; ++y) {
		for (int x = x0; x <= x1; ++x) {
			const float px = float(x) + 0.5f, py = float(y) + 0.5f;
			// Inside iff the pixel is on the same side of every edge
			// (works for either winding of a convex polygon).
			float sign = 0.0f;
			bool inside = true;
			for (int i = 0; i < count && inside; ++i) {
				const ImVec2& a = pts[i];
				const ImVec2& b = pts[(i + 1) % count];
				const float cross = (b.x - a.x) * (py - a.y) -
					(b.y - a.y) * (px - a.x);
				if (std::fabs(cross) < 1e-6f)
					continue;
				if (sign == 0.0f)
					sign = cross;
				else if ((cross > 0.0f) != (sign > 0.0f))
					inside = false;
			}
			if (inside)
				Put(x, y, r, g, b, 255, img);
		}
	}
}

void FillThickSegment(const ImVec2& p0, const ImVec2& p1, float halfW,
	unsigned char r, unsigned char g, unsigned char b,
	std::vector<unsigned char>& img) {
	ImVec2 d(p1.x - p0.x, p1.y - p0.y);
	const float len = std::sqrt(d.x * d.x + d.y * d.y);
	if (len < 1e-4f)
		return;
	d.x /= len; d.y /= len;
	const ImVec2 n(-d.y, d.x);
	ImVec2 quad[4] = {
		{ p0.x + n.x * halfW, p0.y + n.y * halfW },
		{ p1.x + n.x * halfW, p1.y + n.y * halfW },
		{ p1.x - n.x * halfW, p1.y - n.y * halfW },
		{ p0.x - n.x * halfW, p0.y - n.y * halfW },
	};
	FillConvex(quad, 4, r, g, b, img);
}

// 512-icon-space -> the 64px fallback grid.
ImVec2 ToFallback(const ImVec2& p) {
	return { p.x * (float(kLogoSize) / 512.0f),
		p.y * (float(kLogoSize) / 512.0f) };
}

std::vector<unsigned char> MakeLogoRgba() {
	std::vector<unsigned char> img(size_t(kLogoSize) * kLogoSize * 4, 0);
	// Frame: the bracket as thick segments (round joins approximated by
	// the segment overlap; corners are covered by the shared endpoints).
	for (int i = 0; i + 1 < kFramePathCount; ++i)
		FillThickSegment(ToFallback(kFramePath[i]),
			ToFallback(kFramePath[i + 1]),
			kFrameThickness * 0.5f * (float(kLogoSize) / 512.0f),
			kTeal[0], kTeal[1], kTeal[2], img);
	// Stone facets (painter's order = the table above).
	for (int f = 0; f < kFacetCount; ++f) {
		ImVec2 pts[5];
		for (int i = 0; i < kFacets[f].count; ++i)
			pts[i] = ToFallback(kFacets[f].pts[i]);
		FillConvex(pts, kFacets[f].count, kFacets[f].r, kFacets[f].g,
			kFacets[f].b, img);
	}
	return img;
}

std::string LogoPath() {
	namespace fs = std::filesystem;
	// Prefer the exe dir (portable, next to Editor.exe + content; the
	// branding sync in editor/CMakeLists.txt stages the checked-in
	// raster there, so the generated fallback almost never fires).
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

	// Generate the fallback logo once (idempotent; only when the
	// synced branding raster is absent), then load it as the icon.
	const std::string path = LogoPath();
	{
		std::error_code ec;
		if (!std::filesystem::is_regular_file(path, ec)) {
			std::vector<unsigned char> img = MakeLogoRgba();
			stbi_write_png(path.c_str(), kLogoSize, kLogoSize, 4, img.data(),
				kLogoSize * 4);
			Log("Tourmaline fallback logo written: " + path);
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

float DrawTourmalineGem(ImDrawList* dl, const ImVec2& p, float h) {
	// Vector twin of the branding mark: the teal bracket + the faceted
	// stone, mapped from the 512-icon-space geometry into the (p, h)
	// box. Returns the drawn width (the mark is wider than it is tall —
	// the caller Dummy()s exactly that).
	const float s = h / kMarkH;
	const ImVec2 q(p.x - kMarkMinX * s, p.y - kMarkMinY * s);
	const auto Map = [s, q](const ImVec2& pt) {
		return ImVec2(q.x + pt.x * s, q.y + pt.y * s);
	};

	// Frame: one thick rounded-corner polyline (antialiased).
	ImVec2 frame[kFramePathCount];
	for (int i = 0; i < kFramePathCount; ++i)
		frame[i] = Map(kFramePath[i]);
	dl->AddPolyline(frame, kFramePathCount,
		IM_COL32(kTeal[0], kTeal[1], kTeal[2], 255),
		ImDrawListFlags_None, kFrameThickness * s);

	// Stone facets, painter's order (flat tones stand in for the SVG
	// gradients; the union is the hexagonal silhouette).
	for (int f = 0; f < kFacetCount; ++f) {
		ImVec2 pts[5];
		for (int i = 0; i < kFacets[f].count; ++i)
			pts[i] = Map(kFacets[f].pts[i]);
		dl->AddConvexPolyFilled(pts, kFacets[f].count,
			IM_COL32(kFacets[f].r, kFacets[f].g, kFacets[f].b, 255));
	}
	return kMarkW * s;
}

} // namespace editor
