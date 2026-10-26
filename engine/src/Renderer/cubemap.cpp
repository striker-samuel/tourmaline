// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Renderer/cubemap.h>

#include <Engine/Utils/texture_loader.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace smvk {

std::vector<uint8_t> GenerateCubemapFacePixels(CubemapFace face, uint32_t size) {
	const uint32_t S = std::max(1u, size);
	std::vector<uint8_t> px(size_t(S) * S * 4);
	// Per-face tint (same hues the baker's test-pattern mode uses).
	float tint[3] = { 1.0f, 1.0f, 1.0f };
	switch (face) {
	case CubemapFace::Px: tint[0] = 1.0f; tint[1] = 0.35f; tint[2] = 0.30f; break;
	case CubemapFace::Nx: tint[0] = 0.30f; tint[1] = 0.65f; tint[2] = 1.00f; break;
	case CubemapFace::Py: tint[0] = 0.45f; tint[1] = 1.00f; tint[2] = 0.45f; break;
	case CubemapFace::Ny: tint[0] = 1.00f; tint[1] = 0.85f; tint[2] = 0.35f; break;
	case CubemapFace::Pz: tint[0] = 0.65f; tint[1] = 0.45f; tint[2] = 1.00f; break;
	default: tint[0] = 0.85f; tint[1] = 0.85f; tint[2] = 0.90f; break;
	}
	for (uint32_t y = 0; y < S; ++y) {
		for (uint32_t x = 0; x < S; ++x) {
			const float u = (float(x) + 0.5f) / float(S);
			const float v = (float(y) + 0.5f) / float(S);
			// Gradient across the face + center crosshair so rotated /
			// mirrored faces are obvious.
			float g = 0.55f + 0.45f * u * v * 4.0f * 0.25f + 0.20f * u;
			const bool cross = (std::abs(int(x) - int(S) / 2) < std::max(1, int(S) / 128))
				|| (std::abs(int(y) - int(S) / 2) < std::max(1, int(S) / 128));
			if (cross)
				g = 1.0f;
			const size_t i = (size_t(y) * S + x) * 4;
			px[i + 0] = (uint8_t)(std::clamp(tint[0] * g, 0.0f, 1.0f) * 255.0f);
			px[i + 1] = (uint8_t)(std::clamp(tint[1] * g, 0.0f, 1.0f) * 255.0f);
			px[i + 2] = (uint8_t)(std::clamp(tint[2] * g, 0.0f, 1.0f) * 255.0f);
			px[i + 3] = 255;
		}
	}
	return px;
}

std::string CubemapVfsPath(const std::string& name) {
	return "gi/cubemaps/" + name + ".ktx2";
}

std::string CubemapManifestVfsPath(const std::string& name) {
	return "gi/cubemaps/" + name + ".cubemanifest";
}

std::string CubemapManifestSerialize(const std::vector<CubemapManifestEntry>& entries) {
	std::string out;
	char buf[512];
	for (const auto& e : entries) {
		// %.9g: float round-trip EXACT (the invalidation model's
		// recomputes parse the def back and must reproduce the recorded
		// bake-input hash bit-for-bit — %.6g quantization broke the
		// def fold).
		std::snprintf(buf, sizeof(buf),
			"%s %.9g %.9g %.9g %.9g %.9g box %.9g %.9g %.9g %.9g %.9g %.9g%s%s%s%s%s\n",
			e.vfsPath.c_str(),
			(double)e.position.x, (double)e.position.y, (double)e.position.z,
			(double)e.influenceRadius, (double)e.fadeRadius,
			(double)e.boxMin.x, (double)e.boxMin.y, (double)e.boxMin.z,
			(double)e.boxMax.x, (double)e.boxMax.y, (double)e.boxMax.z,
			e.pinned ? " pin" : "",
			!e.guid.empty() ? " guid " : "",
			!e.guid.empty() ? e.guid.c_str() : "",
			!e.inputHash.empty() ? " hash " : "",
			!e.inputHash.empty() ? e.inputHash.c_str() : "");
		out += buf;
	}
	return out;
}

bool CubemapManifestParse(std::string_view text,
	std::vector<CubemapManifestEntry>& out, std::string& err) {
	out.clear();
	auto isSpace = [](char c) {
		return c == ' ' || c == '\t';
	};
	size_t pos = 0;
	size_t lineNo = 0;
	while (pos < text.size()) {
		size_t end = text.find('\n', pos);
		if (end == std::string_view::npos)
			end = text.size();
		std::string_view line = text.substr(pos, end - pos);
		pos = end + 1;
		++lineNo;
		// Trim trailing \r + whitespace-only lines.
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
			line.remove_suffix(1);
		size_t first = 0;
		while (first < line.size() && (line[first] == ' ' || line[first] == '\t'))
			++first;
		if (first >= line.size() || line[first] == '#')
			continue;
		line.remove_prefix(first);
		// Tokenize (the legacy grammar is positional: path + 5 floats +
		// optional flags).
		std::vector<std::string_view> toks;
		for (size_t i = 0; i < line.size();) {
			while (i < line.size() && isSpace(line[i]))
				++i;
			if (i >= line.size())
				break;
			size_t j = i;
			while (j < line.size() && !isSpace(line[j]))
				++j;
			toks.push_back(line.substr(i, j - i));
			i = j;
		}
		if (toks.size() < 6) {
			err = "line " + std::to_string(lineNo) + ": want '<vfsPath> x y z influence fade [pin] [guid <hex>] [hash <hex>]'";
			out.clear();
			return false;
		}
		CubemapManifestEntry e{};
		e.vfsPath = std::string(toks[0]);
		auto parseFloat = [](std::string_view t, float& v) {
			if (t.empty() || t.size() > 32)
				return false;
			char buf[40] = {};
			std::memcpy(buf, t.data(),
				t.size() < sizeof(buf) - 1 ? t.size() : sizeof(buf) - 1);
			char* endp = nullptr;
			v = std::strtof(buf, &endp);
			return endp != buf && *endp == '\0';
		};
		if (!parseFloat(toks[1], e.position.x)
			|| !parseFloat(toks[2], e.position.y)
			|| !parseFloat(toks[3], e.position.z)
			|| !parseFloat(toks[4], e.influenceRadius)
			|| !parseFloat(toks[5], e.fadeRadius)) {
			err = "line " + std::to_string(lineNo) + ": want '<vfsPath> x y z influence fade [pin] [guid <hex>] [hash <hex>]'";
			out.clear();
			return false;
		}
		size_t i = 6;
		// Optional "box <minx> <miny> <minz> <maxx> <maxy> <maxz>": the
		// depth-fitted proxy volume (see CubemapManifestEntry). Old rows
		// carry none and derive the legacy symmetric box below.
		bool hasBox = false;
		if (i < toks.size() && toks[i] == "box") {
			if (toks.size() < i + 7) {
				err = "line " + std::to_string(lineNo)
					+ ": 'box' wants 6 floats (min xyz + max xyz)";
				out.clear();
				return false;
			}
			if (!parseFloat(toks[i + 1], e.boxMin.x)
				|| !parseFloat(toks[i + 2], e.boxMin.y)
				|| !parseFloat(toks[i + 3], e.boxMin.z)
				|| !parseFloat(toks[i + 4], e.boxMax.x)
				|| !parseFloat(toks[i + 5], e.boxMax.y)
				|| !parseFloat(toks[i + 6], e.boxMax.z)) {
				err = "line " + std::to_string(lineNo)
					+ ": 'box' wants 6 numeric floats";
				out.clear();
				return false;
			}
			hasBox = true;
			i += 7;
		}
		if (!hasBox)
			e.DeriveSymmetricBox();
		if (i < toks.size() && toks[i] == "pin") {
			e.pinned = true;
			++i;
		}
		if (i < toks.size() && toks[i] == "guid") {
			if (i + 1 >= toks.size() || toks[i + 1].size() != 32) {
				err = "line " + std::to_string(lineNo) + ": 'guid' wants a 32-hex value";
				out.clear();
				return false;
			}
			e.guid = std::string(toks[i + 1]);
			bool hex = true;
			for (char c : e.guid)
				hex = hex && ((c >= '0' && c <= '9')
					|| (c >= 'a' && c <= 'f'));
			if (!hex) {
				err = "line " + std::to_string(lineNo) + ": 'guid' wants a 32-hex value";
				out.clear();
				return false;
			}
			i += 2;
		}
		// Optional 'hash <16-hex>': the bake-input hash at bake time
		// (the pak gate's freshness check; the runtime skips the token —
		// validation is bake/pak-side).
		if (i < toks.size() && toks[i] == "hash") {
			if (i + 1 >= toks.size() || toks[i + 1].size() != 16) {
				err = "line " + std::to_string(lineNo) + ": 'hash' wants a 16-hex value";
				out.clear();
				return false;
			}
			e.inputHash = std::string(toks[i + 1]);
			bool hex = true;
			for (char c : e.inputHash)
				hex = hex && ((c >= '0' && c <= '9')
					|| (c >= 'a' && c <= 'f'));
			if (!hex) {
				err = "line " + std::to_string(lineNo) + ": 'hash' wants a 16-hex value";
				out.clear();
				return false;
			}
			i += 2;
		}
		if (i < toks.size()) {
			err = "line " + std::to_string(lineNo) + ": unknown trailing token '" + std::string(toks[i]) + "' (want 'pin', 'guid <hex>' or 'hash <hex>')";
			out.clear();
			return false;
		}
		if (e.influenceRadius <= 0.0f || e.fadeRadius < 0.0f) {
			err = "line " + std::to_string(lineNo) + ": influence must be > 0, fade >= 0";
			out.clear();
			return false;
		}
		out.push_back(std::move(e));
	}
	if (out.empty()) {
		err = "manifest has no probe rows";
		return false;
	}
	// Row cap: the RESIDENT/upload budget (kForwardPlusMaxProbes; the
	// runtime streams the nearest rows within it - the manifest may
	// carry more than the budget, the streaming decides residency).
	// Per-PIXEL shading cost never scales with this: the tile lists
	// bucket at most MAX_FP_PROBE_TILE_SLOTS (4) per tile. Pinned rows
	// survive truncation (artist-placed probes must not lose to bulk
	// order).
	if (out.size() > kCubemapMaxManifestRows) {
		err = "manifest lists " + std::to_string(out.size())
			+ " probes (row cap " + std::to_string(kCubemapMaxManifestRows) + ")";
		std::stable_partition(out.begin(), out.end(),
			[](const CubemapManifestEntry& e) { return e.pinned; });
		out.resize(kCubemapMaxManifestRows);
	}
	return true;
}

SamplerCreateInfo CubemapSampler() noexcept {
	SamplerCreateInfo ci{};
	ci.magFilter = Filter::Linear;
	ci.minFilter = Filter::Linear;
	ci.mipmapMode = MipmapMode::Linear;
	ci.addressU = AddressMode::ClampToEdge;
	ci.addressV = AddressMode::ClampToEdge;
	ci.addressW = AddressMode::ClampToEdge;
	return ci;
}

Holder<TextureHandle> CreateCubemapTexture(ResourceManager& ctx,
	const void* const faces[kCubemapFaceCount],
	uint32_t size, bool generateMips, const char* debugName) {
	if (size == 0) {
		std::cerr << "[cubemap] CreateCubemapTexture: size is 0" << std::endl;
		return {};
	}
	for (uint32_t f = 0; f < kCubemapFaceCount; ++f) {
		if (!faces[f]) {
			std::cerr << "[cubemap] CreateCubemapTexture: face "
				<< f << " (" << CubemapFaceName(CubemapFace(f)) << ") is null" << std::endl;
			return {};
		}
	}
	uint32_t mipLevels = 1;
	if (generateMips) {
		uint32_t w = size;
		while (w > 1) {
			w = std::max(1u, w / 2u);
			++mipLevels;
		}
	}
	ImageCreateInfo ci = MakeCubemapCreateInfo(size, Format::RGBA8_UNorm,
		mipLevels, 1, ImageUsage::Sampled | ImageUsage::TransferDst, debugName);
	// Tight packing: mip m of face f follows mip m of face f-1. The
	// CreateTexture upload path walks mips outer / layers inner with the
	// same order, so a single contiguous blob covers all faces + mips.
	const size_t faceBytes = size_t(size) * size * 4;
	std::vector<uint8_t> upload;
	if (!generateMips) {
		upload.resize(faceBytes * kCubemapFaceCount);
		for (uint32_t f = 0; f < kCubemapFaceCount; ++f)
			std::memcpy(upload.data() + size_t(f) * faceBytes, faces[f], faceBytes);
	} else {
		// Per-face box-filtered mip chain.
		std::vector<std::vector<uint8_t>> chains[kCubemapFaceCount];
		for (uint32_t f = 0; f < kCubemapFaceCount; ++f) {
			const auto* src = static_cast<const uint8_t*>(faces[f]);
			chains[f].emplace_back(src, src + faceBytes);
			uint32_t pw = size, ph = size;
			while (pw > 1 || ph > 1) {
				const uint32_t cw = std::max(1u, pw / 2u);
				const uint32_t ch = std::max(1u, ph / 2u);
				std::vector<uint8_t> dst(size_t(cw) * ch * 4);
				for (uint32_t yy = 0; yy < ch; ++yy) {
					for (uint32_t xx = 0; xx < cw; ++xx) {
						uint32_t acc[4] = { 0, 0, 0, 0 };
						uint32_t n = 0;
						for (uint32_t dy = 0; dy < 2; ++dy) {
							for (uint32_t dx = 0; dx < 2; ++dx) {
								const uint32_t sx = std::min(xx * 2 + dx, pw - 1);
								const uint32_t sy = std::min(yy * 2 + dy, ph - 1);
								const uint8_t* s = &chains[f].back()[(size_t(sy) * pw + sx) * 4];
								for (int c = 0; c < 4; ++c)
									acc[c] += s[c];
								++n;
							}
						}
						uint8_t* d = &dst[(size_t(yy) * cw + xx) * 4];
						for (int c = 0; c < 4; ++c)
							d[c] = uint8_t((acc[c] + n / 2) / n);
					}
				}
				chains[f].push_back(std::move(dst));
				pw = cw;
				ph = ch;
			}
		}
		size_t total = 0;
		for (uint32_t m = 0; m < mipLevels; ++m)
			total += chains[0][m].size() * kCubemapFaceCount;
		upload.reserve(total);
		for (uint32_t m = 0; m < mipLevels; ++m)
			for (uint32_t f = 0; f < kCubemapFaceCount; ++f)
				upload.insert(upload.end(), chains[f][m].begin(), chains[f][m].end());
	}
	auto tex = ctx.CreateTexture(ci, upload.data());
	if (tex.empty())
		std::cerr << "[cubemap] CreateCubemapTexture: CreateTexture failed" << std::endl;
	return tex;
}

bool CubemapLibrary::Load(ResourceManager& ctx, const std::string& name,
	std::span<const uint8_t> ktxBytes, uint32_t fallbackSize) {
	auto it = entries.find(name);
	if (it != entries.end() && it->second.bindless >= 0)
		return true;
	const std::string debugName = "cubemap_" + name;
	if (!ktxBytes.empty()) {
		auto tex = LoadKtxTextureMemory(ctx, ktxBytes.data(), ktxBytes.size(), debugName);
		if (!tex.empty() && ctx.IsCubemapTexture(tex)) {
			CubemapEntry e{};
			e.size = ctx.GetTextureCubemapSize(tex);
			e.baked = true;
			e.bindless = static_cast<int32_t>(ctx.GetTextureBindlessIndex(tex));
			e.mipLevels = ctx.GetTextureMipLevels(tex);
			e.texture = std::move(tex);
			entries[name] = std::move(e);
			std::cout << "[cubemap] '" << name << "' loaded from baked payload: "
				<< "(" << entries[name].size << "px)" << std::endl;
			return true;
		}
		if (!tex.empty())
			std::cerr << "[cubemap] '" << name << "' decoded but is not a cubemap: "
				<< "(falling back to procedural)" << std::endl;
		else
			std::cerr << "[cubemap] baked payload failed to decode: "
				<< "(falling back to procedural)" << std::endl;
	}
	// Fallback: procedural per-face gobos (identical math to the baker's
	// test-pattern mode, single mip level).
	const uint32_t S = std::max(1u, fallbackSize);
	std::vector<std::vector<uint8_t>> facePx;
	facePx.reserve(kCubemapFaceCount);
	const void* faces[kCubemapFaceCount] = {};
	for (uint32_t f = 0; f < kCubemapFaceCount; ++f) {
		facePx.push_back(GenerateCubemapFacePixels(CubemapFace(f), S));
		faces[f] = facePx.back().data();
	}
	auto tex = CreateCubemapTexture(ctx, faces, S, false, debugName.c_str());
	if (tex.empty()) {
		std::cerr << "[cubemap] '" << name
			<< "' unavailable (no baked file, procedural upload failed)" << std::endl;
		return false;
	}
	CubemapEntry e{};
	e.size = S;
	e.baked = false;
	e.bindless = static_cast<int32_t>(ctx.GetTextureBindlessIndex(tex));
	e.mipLevels = 1;
	e.texture = std::move(tex);
	entries[name] = std::move(e);
	std::cout << "[cubemap] '" << name << "' procedural fallback (" << S << "px)" << std::endl;
	return true;
}

bool ReflectionProbeLibrary::Load(ResourceManager& ctx, const OverlayVfs& vfs,
	const std::string& name, const glm::vec3& position,
	float influenceRadius, float fadeRadius,
	const std::string& vfsPath, uint32_t fallbackSize) {
	auto it = probes.find(name);
	if (it != probes.end() && it->second.valid)
		return true;

	const std::string path = vfsPath.empty() ? CubemapVfsPath(name) : vfsPath;
	const std::string debugName = "refl_probe_" + name;

	ReflectionProbe probe{};
	probe.position = position;
	probe.influenceRadius = influenceRadius;
	probe.fadeRadius = fadeRadius;

	if (auto bytes = vfs.TryRead(path)) {
		auto tex = LoadKtxTextureMemory(ctx, bytes->data(), bytes->size(), debugName);
		if (!tex.empty() && ctx.IsCubemapTexture(tex)) {
			probe.entry.size = ctx.GetTextureCubemapSize(tex);
			probe.entry.baked = true;
			probe.entry.bindless = static_cast<int32_t>(ctx.GetTextureBindlessIndex(tex));
			probe.entry.mipLevels = ctx.GetTextureMipLevels(tex);
			probe.entry.texture = std::move(tex);
			probe.valid = true;
			std::cout << "[refl_probe] '" << name << "' loaded from baked file: "
				<< path << " (" << probe.entry.size << "px, pos=" << position.x << "," << position.y << "," << position.z
				<< ", radius=" << influenceRadius << "+" << fadeRadius << ")" << std::endl;
		} else if (!tex.empty()) {
			std::cerr << "[refl_probe] '" << name << "' decoded but is not a cubemap: " << path << std::endl;
		} else {
			std::cerr << "[refl_probe] baked file failed to decode: " << path << std::endl;
		}
	}

	if (!probe.valid) {
		// Procedural fallback
		const uint32_t S = std::max(1u, fallbackSize);
		std::vector<std::vector<uint8_t>> facePx;
		facePx.reserve(kCubemapFaceCount);
		const void* faces[kCubemapFaceCount] = {};
		for (uint32_t f = 0; f < kCubemapFaceCount; ++f) {
			facePx.push_back(GenerateCubemapFacePixels(CubemapFace(f), S));
			faces[f] = facePx.back().data();
		}
		auto tex = CreateCubemapTexture(ctx, faces, S, false, debugName.c_str());
		if (tex.empty()) {
			std::cerr << "[refl_probe] '" << name << "' unavailable (no baked file, procedural upload failed)" << std::endl;
			return false;
		}
		probe.entry.size = S;
		probe.entry.baked = false;
		probe.entry.bindless = static_cast<int32_t>(ctx.GetTextureBindlessIndex(tex));
		probe.entry.mipLevels = 1;
		probe.entry.texture = std::move(tex);
		probe.valid = true;
		std::cout << "[refl_probe] '" << name << "' procedural fallback (" << S << "px)" << std::endl;
	}

	probes[name] = std::move(probe);
	return true;
}

const ReflectionProbe* ReflectionProbeLibrary::Nearest(const glm::vec3& worldPos) const {
	const ReflectionProbe* best = nullptr;
	float bestDist2 = FLT_MAX;
	for (const auto& [_, probe] : probes) {
		if (!probe.valid) continue;
		glm::vec3 diff = worldPos - probe.position;
		float dist2 = glm::dot(diff, diff);
		if (dist2 < bestDist2) {
			bestDist2 = dist2;
			best = &probe;
		}
	}
	return best;
}

std::vector<const ReflectionProbe*> ReflectionProbeLibrary::SortedByDistance(const glm::vec3& worldPos) const {
	std::vector<const ReflectionProbe*> result;
	result.reserve(probes.size());
	for (const auto& [_, probe] : probes) {
		if (probe.valid) result.push_back(&probe);
	}
	std::sort(result.begin(), result.end(),
		[&](const ReflectionProbe* a, const ReflectionProbe* b) {
			glm::vec3 diffA = worldPos - a->position;
			glm::vec3 diffB = worldPos - b->position;
			return glm::dot(diffA, diffA) < glm::dot(diffB, diffB);
		});
	return result;
}

} // namespace smvk
