// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Small, readable brush-preset assets shared by terrain sculpting and
// PlacementGraph scatter authoring. No editor state or ImGui dependency.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace editor::brush {
enum class Kind { Terrain, Scatter };

struct Preset {
	Kind kind = Kind::Terrain;
	std::string name = "Custom";
	float radius = 8.0f;      // world-space metres
	float strength = 0.5f;    // per-stamp sculpt delta / mask opacity
	float spacing = 0.5f;     // stroke spacing as fraction of radius
	int operation = 0;        // terrain: raise/lower/flatten/smooth
	float density = 0.5f;     // scatter graph Density.value
	float minScale = 0.8f;
	float maxScale = 1.2f;
	float minDistance = 0.0f; // scatter graph Spacing.minSpacing
};

inline bool Valid(const Preset& p) {
	return !p.name.empty() && p.name.size() < 128 &&
		std::isfinite(p.radius) && p.radius >= 0.5f && p.radius <= 256.0f &&
		std::isfinite(p.strength) && p.strength >= 0.0f && p.strength <= 64.0f &&
		std::isfinite(p.spacing) && p.spacing >= 0.05f && p.spacing <= 2.0f &&
		p.operation >= 0 && p.operation <= 3 &&
		std::isfinite(p.density) && p.density >= 0.0f && p.density <= 1.0f &&
		std::isfinite(p.minScale) && p.minScale > 0.0f && p.minScale <= 32.0f &&
		std::isfinite(p.maxScale) && p.maxScale >= p.minScale &&
		p.maxScale <= 32.0f &&
		std::isfinite(p.minDistance) && p.minDistance >= 0.0f &&
		p.minDistance <= 64.0f;
}

inline bool Read(std::istream& in, Preset& out) {
	Preset p;
	bool version = false, type = false;
	std::string line;
	try {
		while (std::getline(in, line)) {
			if (line.empty() || line[0] == '#') continue;
			const size_t eq = line.find('=');
			if (eq == std::string::npos) return false;
			const std::string key = line.substr(0, eq);
			const std::string val = line.substr(eq + 1);
			if (key == "version") { if (val != "1") return false; version = true; }
			else if (key == "kind") {
				if (val == "terrain") p.kind = Kind::Terrain;
				else if (val == "scatter") p.kind = Kind::Scatter;
				else return false;
				type = true;
			}
			else if (key == "name") p.name = val;
			else if (key == "radius") p.radius = std::stof(val);
			else if (key == "strength") p.strength = std::stof(val);
			else if (key == "spacing") p.spacing = std::stof(val);
			else if (key == "operation") p.operation = std::stoi(val);
			else if (key == "density") p.density = std::stof(val);
			else if (key == "minScale") p.minScale = std::stof(val);
			else if (key == "maxScale") p.maxScale = std::stof(val);
			else if (key == "minDistance") p.minDistance = std::stof(val);
			else return false; // schema typo, not a silent partial load
		}
	} catch (...) { return false; }
	if (!version || !type || !Valid(p)) return false;
	out = p; // transactional parse: never modify caller on failure
	return true;
}

inline void Write(std::ostream& out, const Preset& p) {
	out << "version=1\nkind=" << (p.kind == Kind::Terrain ? "terrain" : "scatter")
		<< "\nname=" << p.name
		<< "\nradius=" << p.radius
		<< "\nstrength=" << p.strength
		<< "\nspacing=" << p.spacing
		<< "\noperation=" << p.operation
		<< "\ndensity=" << p.density
		<< "\nminScale=" << p.minScale
		<< "\nmaxScale=" << p.maxScale
		<< "\nminDistance=" << p.minDistance << '\n';
}

// Prefer the source checkout's editor/brush_presets even when launched from
// a build directory. Outside a checkout, create a local asset folder.
inline std::filesystem::path Directory() {
	std::error_code ec;
	auto p = std::filesystem::current_path(ec);
	if (ec) return "editor/brush_presets";
	for (int i = 0; i < 7; ++i) {
		if (std::filesystem::exists(p / "editor" / "CMakeLists.txt", ec))
			return p / "editor" / "brush_presets";
		if (!p.has_parent_path() || p == p.parent_path()) break;
		p = p.parent_path();
	}
	return std::filesystem::path("editor") / "brush_presets";
}

inline std::vector<std::filesystem::path> Assets() {
	std::vector<std::filesystem::path> files;
	std::error_code ec;
	const auto dir = Directory();
	if (!std::filesystem::is_directory(dir, ec)) return files;
	for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
		if (ec) break;
		if (entry.is_regular_file(ec) && entry.path().extension() == ".brushpreset")
			files.push_back(entry.path());
	}
	std::sort(files.begin(), files.end());
	return files;
}

inline bool Load(const std::filesystem::path& path, Preset& p) {
	std::ifstream in(path);
	return in && Read(in, p);
}

inline bool Save(Preset p) {
	if (!Valid(p)) return false;
	std::string stem;
	for (char c : p.name) {
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '-' || c == '_')
			stem += c;
		else if (c == ' ') stem += '_';
	}
	if (stem.empty()) return false;
	const auto dir = Directory();
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	if (ec) return false;
	const auto path = dir / (stem + ".brushpreset");
	std::ofstream out(path, std::ios::trunc);
	if (!out) return false;
	Write(out, p);
	return bool(out);
}
} // namespace editor::brush
