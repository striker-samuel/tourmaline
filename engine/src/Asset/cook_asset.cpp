// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Cook-asset manifest IO (see Engine/Asset/cook_asset.h).

#include <Engine/Asset/cook_asset.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace smvk::cook {

namespace {

bool IsHexLower(char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// FNV-1a 64-bit over the canonical settings rendering (deterministic
// across runs/platforms for the same inputs; NOT cryptographic — a
// staleness token, not a security hash).
uint64_t HashStr(const std::string& s) {
	uint64_t h = 1469598103934665603ull;
	for (unsigned char c : s) {
		h ^= c;
		h *= 1099511628211ull;
	}
	return h;
}

std::string Hex16(uint64_t v) {
	char buf[17];
	std::snprintf(buf, sizeof(buf), "%016llx",
		(unsigned long long)v);
	return std::string(buf);
}

std::string Trim(const std::string& s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
		return {};
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

float ParseFloat(const std::string& s, float fallback) {
	if (s.empty())
		return fallback;
	char* end = nullptr;
	const float v = std::strtof(s.c_str(), &end);
	return (end == s.c_str()) ? fallback : v;
}

int ParseInt(const std::string& s, int fallback) {
	if (s.empty())
		return fallback;
	char* end = nullptr;
	const long v = std::strtol(s.c_str(), &end, 10);
	return (end == s.c_str()) ? fallback : int(v);
}

uint32_t ParseU32(const std::string& s, uint32_t fallback) {
	if (s.empty())
		return fallback;
	char* end = nullptr;
	const unsigned long v = std::strtoul(s.c_str(), &end, 10);
	return (end == s.c_str()) ? fallback : uint32_t(v);
}

bool ParseBool(const std::string& s, bool fallback) {
	if (s.empty())
		return fallback;
	if (s == "1" || s == "true" || s == "yes")
		return true;
	if (s == "0" || s == "false" || s == "no")
		return false;
	return fallback;
}

} // namespace

bool IsCookAssetGuid(std::string_view s) {
	if (s.size() != 32)
		return false;
	for (char c : s)
		if (!IsHexLower(c))
			return false;
	return true;
}

GiCookAsset DefaultGiCookAsset(const std::string& name,
	const std::string& scene) {
	GiCookAsset a;
	a.name = name;
	a.scene = scene;
	a.giVolumes = true;
	a.world = true;
	return a;
}

LightmapCookAsset DefaultLightmapCookAsset(const std::string& name,
	const std::string& scene) {
	LightmapCookAsset a;
	a.name = name;
	a.scene = scene;
	return a;
}

std::string GiCookSettingsHash(const GiCookAsset& a) {
	std::ostringstream o;
	o << "scene=" << a.scene << "\n";
	o << "giVolumes=" << (a.giVolumes ? 1 : 0) << "\n";
	o << "world=" << (a.world ? 1 : 0) << "\n";
	return Hex16(HashStr(o.str()));
}

std::string LightmapCookSettingsHash(const LightmapCookAsset& a) {
	std::ostringstream o;
	o << "scene=" << a.scene << "\n";
	o << "lmMetersPerTexel=" << a.lmMetersPerTexel << "\n";
	o << "lmLobes=" << a.lmLobes << "\n";
	o << "lmAtlasWidth=" << a.lmAtlasWidth << "\n";
	o << "lmRays=" << a.lmRays << "\n";
	o << "lmProxyRes=" << a.lmProxyRes << "\n";
	o << "lmExcludeSmall=" << a.lmExcludeSmall << "\n";
	o << "lmExcludeKw=" << a.lmExcludeKw << "\n";
	o << "sunAzimuth=" << a.sunAzimuth << "\n";
	o << "sunElevation=" << a.sunElevation << "\n";
	o << "sunIntensity=" << a.sunIntensity << "\n";
	o << "skyIntensity=" << a.skyIntensity << "\n";
	o << "sunColor=" << a.sunColor.x << " " << a.sunColor.y << " "
		<< a.sunColor.z << "\n";
	return Hex16(HashStr(o.str()));
}

bool SaveGiCookAssetText(std::string& out, const GiCookAsset& a) {
	std::ostringstream o;
	o << "# Tourmaline GI cook asset v1 (settings + baked blob result)\n";
	o << "guid=" << a.guid << "\n";
	o << "name=" << a.name << "\n";
	o << "scene=" << a.scene << "\n";
	o << "giVolumes=" << (a.giVolumes ? 1 : 0) << "\n";
	o << "world=" << (a.world ? 1 : 0) << "\n";
	// The binary blob RESULT (the bake payload at its guid-derived path;
	// empty = never baked — creation writes exactly this).
	o << "blobGuid=" << a.blobGuid << "\n";
	o << "blobPath=" << a.blobPath << "\n";
	o << "bakedHash=" << a.bakedHash << "\n";
	out = o.str();
	return true;
}

bool LoadGiCookAssetText(const std::string& text, GiCookAsset& out,
	std::string& err) {
	out = GiCookAsset{};
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = Trim(line.substr(0, eq));
		const std::string val = Trim(line.substr(eq + 1));
		if (key == "guid") out.guid = val;
		else if (key == "name") out.name = val;
		else if (key == "scene") out.scene = val;
		else if (key == "giVolumes") out.giVolumes = ParseBool(val, out.giVolumes);
		else if (key == "world") out.world = ParseBool(val, out.world);
		else if (key == "blobGuid") out.blobGuid = val;
		else if (key == "blobPath") out.blobPath = val;
		else if (key == "bakedHash") out.bakedHash = val;
		// Unknown keys skip (forward tolerance).
	}
	if (!IsCookAssetGuid(out.guid)) {
		err = "gi cook asset carries no/invalid guid";
		out = GiCookAsset{};
		return false;
	}
	// A claimed blob must be a real GUID (a hand-edited typo fails loudly
	// instead of binding a phantom payload).
	if (!out.blobGuid.empty() && !IsCookAssetGuid(out.blobGuid)) {
		err = "gi cook asset carries invalid blobGuid";
		out = GiCookAsset{};
		return false;
	}
	return true;
}

bool SaveGiCookAsset(const std::string& path, const GiCookAsset& a,
	std::string& err) {
	std::string text;
	SaveGiCookAssetText(text, a);
	std::ofstream f(fs::path(path), std::ios::binary | std::ios::trunc);
	if (!f) {
		err = "cannot write '" + path + "'";
		return false;
	}
	f << text;
	if (!f) {
		err = "cannot write '" + path + "'";
		return false;
	}
	return true;
}

bool LoadGiCookAsset(const std::string& path, GiCookAsset& out,
	std::string& err) {
	std::ifstream in(fs::path(path), std::ios::binary);
	if (!in) {
		err = "cannot open '" + path + "'";
		return false;
	}
	const std::string text((std::istreambuf_iterator<char>(in)),
		std::istreambuf_iterator<char>());
	return LoadGiCookAssetText(text, out, err);
}

bool SaveLightmapCookAssetText(std::string& out,
	const LightmapCookAsset& a) {
	std::ostringstream o;
	o << "# Tourmaline lightmap cook asset v1 (settings + baked blob result)\n";
	o << "guid=" << a.guid << "\n";
	o << "name=" << a.name << "\n";
	o << "scene=" << a.scene << "\n";
	o << "lmMetersPerTexel=" << a.lmMetersPerTexel << "\n";
	o << "lmLobes=" << a.lmLobes << "\n";
	o << "lmAtlasWidth=" << a.lmAtlasWidth << "\n";
	o << "lmRays=" << a.lmRays << "\n";
	o << "lmProxyRes=" << a.lmProxyRes << "\n";
	o << "lmExcludeSmall=" << a.lmExcludeSmall << "\n";
	o << "lmExcludeKw=" << a.lmExcludeKw << "\n";
	o << "sunAzimuth=" << a.sunAzimuth << "\n";
	o << "sunElevation=" << a.sunElevation << "\n";
	o << "sunIntensity=" << a.sunIntensity << "\n";
	o << "skyIntensity=" << a.skyIntensity << "\n";
	o << "sunColor=" << a.sunColor.x << " " << a.sunColor.y << " "
		<< a.sunColor.z << "\n";
	// The binary blob RESULT (the atlas at its guid-derived path; empty
	// = never baked — creation writes exactly this).
	o << "blobGuid=" << a.blobGuid << "\n";
	o << "blobPath=" << a.blobPath << "\n";
	o << "bakedHash=" << a.bakedHash << "\n";
	out = o.str();
	return true;
}

bool LoadLightmapCookAssetText(const std::string& text,
	LightmapCookAsset& out, std::string& err) {
	out = LightmapCookAsset{};
	const LightmapCookAsset defaults;
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = Trim(line.substr(0, eq));
		const std::string val = Trim(line.substr(eq + 1));
		if (key == "guid") out.guid = val;
		else if (key == "name") out.name = val;
		else if (key == "scene") out.scene = val;
		else if (key == "lmMetersPerTexel")
			out.lmMetersPerTexel = ParseFloat(val,
				defaults.lmMetersPerTexel);
		else if (key == "lmLobes")
			out.lmLobes = ParseU32(val, defaults.lmLobes);
		else if (key == "lmAtlasWidth")
			out.lmAtlasWidth = ParseU32(val, defaults.lmAtlasWidth);
		else if (key == "lmRays")
			out.lmRays = ParseU32(val, defaults.lmRays);
		else if (key == "lmProxyRes")
			out.lmProxyRes = ParseInt(val, defaults.lmProxyRes);
		else if (key == "lmExcludeSmall")
			out.lmExcludeSmall = ParseFloat(val,
				defaults.lmExcludeSmall);
		else if (key == "lmExcludeKw") out.lmExcludeKw = val;
		else if (key == "sunAzimuth")
			out.sunAzimuth = ParseFloat(val, defaults.sunAzimuth);
		else if (key == "sunElevation")
			out.sunElevation = ParseFloat(val, defaults.sunElevation);
		else if (key == "sunIntensity")
			out.sunIntensity = ParseFloat(val, defaults.sunIntensity);
		else if (key == "skyIntensity")
			out.skyIntensity = ParseFloat(val, defaults.skyIntensity);
		else if (key == "sunColor") {
			float x = defaults.sunColor.x, y = defaults.sunColor.y,
				z = defaults.sunColor.z;
			if (std::sscanf(val.c_str(), "%f %f %f", &x, &y, &z) == 3)
				out.sunColor = glm::vec3(x, y, z);
		}
		else if (key == "blobGuid") out.blobGuid = val;
		else if (key == "blobPath") out.blobPath = val;
		else if (key == "bakedHash") out.bakedHash = val;
		// Unknown keys skip (forward tolerance).
	}
	if (!IsCookAssetGuid(out.guid)) {
		err = "lightmap cook asset carries no/invalid guid";
		out = LightmapCookAsset{};
		return false;
	}
	if (!out.blobGuid.empty() && !IsCookAssetGuid(out.blobGuid)) {
		err = "lightmap cook asset carries invalid blobGuid";
		out = LightmapCookAsset{};
		return false;
	}
	return true;
}

bool SaveLightmapCookAsset(const std::string& path,
	const LightmapCookAsset& a, std::string& err) {
	std::string text;
	SaveLightmapCookAssetText(text, a);
	std::ofstream f(fs::path(path), std::ios::binary | std::ios::trunc);
	if (!f) {
		err = "cannot write '" + path + "'";
		return false;
	}
	f << text;
	if (!f) {
		err = "cannot write '" + path + "'";
		return false;
	}
	return true;
}

bool LoadLightmapCookAsset(const std::string& path, LightmapCookAsset& out,
	std::string& err) {
	std::ifstream in(fs::path(path), std::ios::binary);
	if (!in) {
		err = "cannot open '" + path + "'";
		return false;
	}
	const std::string text((std::istreambuf_iterator<char>(in)),
		std::istreambuf_iterator<char>());
	return LoadLightmapCookAssetText(text, out, err);
}

CubemapAsset DefaultCubemapAsset(const std::string& name,
	const std::string& scene) {
	CubemapAsset a;
	a.name = name;
	a.scene = scene;
	a.capture = scene;
	return a;
}

GivolumeAsset DefaultGivolumeAsset(const std::string& name,
	const std::string& scene, uint32_t node) {
	GivolumeAsset a;
	a.name = name;
	a.scene = scene;
	a.node = node;
	return a;
}

std::string CubemapSettingsHash(const CubemapAsset& a) {
	std::ostringstream o;
	o << "scene=" << a.scene << "\n";
	o << "size=" << a.size << "\n";
	o << "rays=" << a.rays << "\n";
	o << "capture=" << a.capture << "\n";
	return Hex16(HashStr(o.str()));
}

std::string GivolumeSettingsHash(const GivolumeAsset& a) {
	std::ostringstream o;
	o << "scene=" << a.scene << "\n";
	o << "node=" << a.node << "\n";
	o << "priority=" << a.priority << "\n";
	return Hex16(HashStr(o.str()));
}

bool SaveCubemapAssetText(std::string& out, const CubemapAsset& a) {
	std::ostringstream o;
	o << "# Tourmaline cubemap capture asset v1 (settings + baked result)\n";
	o << "guid=" << a.guid << "\n";
	o << "name=" << a.name << "\n";
	o << "scene=" << a.scene << "\n";
	o << "size=" << a.size << "\n";
	o << "rays=" << a.rays << "\n";
	o << "capture=" << a.capture << "\n";
	o << "blobGuid=" << a.blobGuid << "\n";
	o << "blobPath=" << a.blobPath << "\n";
	o << "bakedHash=" << a.bakedHash << "\n";
	out = o.str();
	return true;
}

bool LoadCubemapAssetText(const std::string& text, CubemapAsset& out,
	std::string& err) {
	out = CubemapAsset{};
	const CubemapAsset defaults;
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = Trim(line.substr(0, eq));
		const std::string val = Trim(line.substr(eq + 1));
		if (key == "guid") out.guid = val;
		else if (key == "name") out.name = val;
		else if (key == "scene") out.scene = val;
		else if (key == "size") out.size = ParseInt(val, defaults.size);
		else if (key == "rays") out.rays = ParseInt(val, defaults.rays);
		else if (key == "capture") out.capture = val;
		else if (key == "blobGuid") out.blobGuid = val;
		else if (key == "blobPath") out.blobPath = val;
		else if (key == "bakedHash") out.bakedHash = val;
	}
	if (!IsCookAssetGuid(out.guid)) {
		err = "cubemap asset carries no/invalid guid";
		out = CubemapAsset{};
		return false;
	}
	if (!out.blobGuid.empty() && !IsCookAssetGuid(out.blobGuid)) {
		err = "cubemap asset carries invalid blobGuid";
		out = CubemapAsset{};
		return false;
	}
	return true;
}

bool SaveCubemapAsset(const std::string& path, const CubemapAsset& a,
	std::string& err) {
	std::string text;
	SaveCubemapAssetText(text, a);
	std::ofstream f(fs::path(path), std::ios::binary | std::ios::trunc);
	if (!f) {
		err = "cannot write '" + path + "'";
		return false;
	}
	f << text;
	if (!f) {
		err = "cannot write '" + path + "'";
		return false;
	}
	return true;
}

bool LoadCubemapAsset(const std::string& path, CubemapAsset& out,
	std::string& err) {
	std::ifstream in(fs::path(path), std::ios::binary);
	if (!in) {
		err = "cannot open '" + path + "'";
		return false;
	}
	const std::string text((std::istreambuf_iterator<char>(in)),
		std::istreambuf_iterator<char>());
	return LoadCubemapAssetText(text, out, err);
}

bool SaveGivolumeAssetText(std::string& out, const GivolumeAsset& a) {
	std::ostringstream o;
	o << "# Tourmaline convex-volume asset v1 (settings + baked result)\n";
	o << "guid=" << a.guid << "\n";
	o << "name=" << a.name << "\n";
	o << "scene=" << a.scene << "\n";
	o << "node=" << a.node << "\n";
	o << "priority=" << a.priority << "\n";
	o << "blobGuid=" << a.blobGuid << "\n";
	o << "blobPath=" << a.blobPath << "\n";
	o << "bakedHash=" << a.bakedHash << "\n";
	out = o.str();
	return true;
}

bool LoadGivolumeAssetText(const std::string& text, GivolumeAsset& out,
	std::string& err) {
	out = GivolumeAsset{};
	const GivolumeAsset defaults;
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == '#')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = Trim(line.substr(0, eq));
		const std::string val = Trim(line.substr(eq + 1));
		if (key == "guid") out.guid = val;
		else if (key == "name") out.name = val;
		else if (key == "scene") out.scene = val;
		else if (key == "node")
			out.node = ParseU32(val, defaults.node);
		else if (key == "priority")
			out.priority = ParseU32(val, defaults.priority);
		else if (key == "blobGuid") out.blobGuid = val;
		else if (key == "blobPath") out.blobPath = val;
		else if (key == "bakedHash") out.bakedHash = val;
	}
	if (!IsCookAssetGuid(out.guid)) {
		err = "givolume asset carries no/invalid guid";
		out = GivolumeAsset{};
		return false;
	}
	if (!out.blobGuid.empty() && !IsCookAssetGuid(out.blobGuid)) {
		err = "givolume asset carries invalid blobGuid";
		out = GivolumeAsset{};
		return false;
	}
	return true;
}

bool SaveGivolumeAsset(const std::string& path, const GivolumeAsset& a,
	std::string& err) {
	std::string text;
	SaveGivolumeAssetText(text, a);
	std::ofstream f(fs::path(path), std::ios::binary | std::ios::trunc);
	if (!f) {
		err = "cannot write '" + path + "'";
		return false;
	}
	f << text;
	if (!f) {
		err = "cannot write '" + path + "'";
		return false;
	}
	return true;
}

bool LoadGivolumeAsset(const std::string& path, GivolumeAsset& out,
	std::string& err) {
	std::ifstream in(fs::path(path), std::ios::binary);
	if (!in) {
		err = "cannot open '" + path + "'";
		return false;
	}
	const std::string text((std::istreambuf_iterator<char>(in)),
		std::istreambuf_iterator<char>());
	return LoadGivolumeAssetText(text, out, err);
}

} // namespace smvk::cook
