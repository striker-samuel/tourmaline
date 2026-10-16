// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Placement/placement_terrain.h>
#include <Engine/Placement/placement_eval.h>

#include <algorithm>
#include <cmath>

namespace smvk::placement {

HeightfieldData BuildTerrainHeights(const TerrainParams& params) {
	HeightfieldData hf;
	const uint32_t res = std::clamp(params.res, 2u, 512u);
	const float size = std::max(params.size, 1.0f);
	const float height = std::max(params.height, 0.0f);
	const float freq = std::max(params.freq, 1e-4f);
	hf.width = res;
	hf.height = res;
	hf.origin = glm::vec3(params.origin.x, 0.0f, params.origin.y);
	const float sp = size / float(res - 1);
	hf.spacing = glm::vec2(sp, sp);
	hf.heights.resize((size_t)res * res);
	// Rolling hills: two value-noise octaves. Output range [0, height].
	for (uint32_t z = 0; z < res; ++z) {
		for (uint32_t x = 0; x < res; ++x) {
			const float wx = params.origin.x + float(x) * sp;
			const float wz = params.origin.y + float(z) * sp;
			const float n1 = ValueNoise2D(wx * freq, wz * freq, params.seed);
			const float n2 = ValueNoise2D(wx * freq * 3.7f + 11.3f,
				wz * freq * 3.7f + 7.1f, params.seed ^ 0x9e3779b9ULL);
			hf.heights[(size_t)z * res + x] = height * (0.7f * n1 + 0.3f * n2);
		}
	}
	return hf;
}

bool BuildTerrainUpload(const TerrainParams& params, StaticMeshUpload& out) {
	// Single-node terrain == a 1x1 group tile (shared code path).
	TerrainGroupData g;
	g.params = params;
	g.tilesX = 1;
	g.tilesZ = 1;
	g.heights = BuildTerrainHeights(params);
	return BuildTerrainTileUpload(g, 0, 0, out);
}

// ---------------------------------------------------------------------------
// Terrain groups: tiles + painting
// ---------------------------------------------------------------------------

void TerrainTileRange(const TerrainGroupData& g, uint32_t tx, uint32_t tz,
	uint32_t& x0, uint32_t& x1, uint32_t& z0, uint32_t& z1) {
	const uint32_t res = g.heights.width; // global samples per side
	const uint32_t tX = std::max(1u, g.tilesX);
	const uint32_t tZ = std::max(1u, g.tilesZ);
	// Inclusive sample split of the res-1 intervals; neighbours share the
	// edge sample so heights are continuous across tile borders.
	x0 = tx * (res - 1) / tX;
	x1 = (tx + 1 == tX) ? res - 1 : (tx + 1) * (res - 1) / tX;
	z0 = tz * (res - 1) / tZ;
	z1 = (tz + 1 == tZ) ? res - 1 : (tz + 1) * (res - 1) / tZ;
}

bool BuildTerrainTileUpload(const TerrainGroupData& g, uint32_t tx, uint32_t tz,
	StaticMeshUpload& out) {
	out = StaticMeshUpload{};
	const HeightfieldData& hf = g.heights;
	const uint32_t res = hf.width;
	if (res < 2 || !hf.valid())
		return false;
	const uint32_t tX = std::max(1u, g.tilesX);
	const uint32_t tZ = std::max(1u, g.tilesZ);
	if (tx >= tX || tz >= tZ)
		return false;
	uint32_t x0, x1, z0, z1;
	TerrainTileRange(g, tx, tz, x0, x1, z0, z1);
	const uint32_t w = x1 - x0 + 1;
	const uint32_t h = z1 - z0 + 1;
	const float sp = hf.spacing.x; // global sample spacing
	const float invW = 1.0f / float(w - 1);
	const float invH = 1.0f / float(h - 1);
	out.vertices.reserve((size_t)w * h);
	out.indices.reserve((size_t)(w - 1) * (h - 1) * 6);
	glm::vec3 mn(1e30f), mx(-1e30f);
	for (uint32_t z = 0; z < h; ++z) {
		for (uint32_t x = 0; x < w; ++x) {
			const uint32_t gx = x0 + x, gz = z0 + z;
			const float lx = float(x) * sp;
			const float lz = float(z) * sp;
			// Normals sample the GLOBAL field so edge verts match across
			// neighbouring tiles (same absolute XZ -> same central diff).
			const float wx = hf.origin.x + float(gx) * sp;
			const float wz = hf.origin.z + float(gz) * sp;
			const float hgt = hf.heights[(size_t)gz * res + gx];
			const glm::vec3 n = hf.sampleNormal(wx, wz);
			glm::vec3 t = glm::cross(glm::vec3(0, 1, 0), n);
			if (glm::dot(t, t) < 1e-8f)
				t = glm::vec3(1, 0, 0);
			t = glm::normalize(t);
			StaticVertex v{};
			v.pos = glm::vec3(lx, hgt, lz);
			v.color = glm::vec4(1.0f);
			v.texCoord = glm::vec2(float(x) * invW, float(z) * invH);
			v.normal = n;
			v.tangent = glm::vec4(t, 1.0f);
			out.vertices.push_back(v);
			mn.x = std::min(mn.x, v.pos.x);
			mn.y = std::min(mn.y, v.pos.y);
			mn.z = std::min(mn.z, v.pos.z);
			mx.x = std::max(mx.x, v.pos.x);
			mx.y = std::max(mx.y, v.pos.y);
			mx.z = std::max(mx.z, v.pos.z);
		}
	}
	for (uint32_t z = 0; z + 1 < h; ++z) {
		for (uint32_t x = 0; x + 1 < w; ++x) {
			const uint32_t a = z * w + x;
			const uint32_t b = a + 1;
			const uint32_t c = a + w;
			const uint32_t d = c + 1;
			// Up-facing winding for +Y-up counter-clockwise front faces.
			out.indices.insert(out.indices.end(), { a, c, b, b, c, d });
		}
	}
	Surface s{};
	s.firstIndex = 0;
	s.indexCount = (uint32_t)out.indices.size();
	s.baseVertex = 0;
	out.surfaces.push_back(s);
	out.aabbMin = mn;
	out.aabbMax = mx;
	return true;
}

void TerrainBrushBox(const TerrainGroupData& g, float wx, float wz,
	float radius, uint32_t& x0, uint32_t& x1, uint32_t& z0, uint32_t& z1) {
	const HeightfieldData& hf = g.heights;
	const uint32_t res = hf.width;
	const float sp = hf.spacing.x;
	// Grid coords of the stamp center, rounded outward by the radius.
	const float cx = (wx - hf.origin.x) / sp;
	const float cz = (wz - hf.origin.z) / sp;
	const float r = std::max(radius, 0.0f) / sp;
	auto clampIdx = [&](int64_t v) { return uint32_t(std::clamp(v,
		int64_t(0), int64_t(res - 1))); };
	x0 = clampIdx(int64_t(std::floor(cx - r)));
	x1 = clampIdx(int64_t(std::ceil(cx + r)));
	z0 = clampIdx(int64_t(std::floor(cz - r)));
	z1 = clampIdx(int64_t(std::ceil(cz + r)));
}

void TerrainBrushStamp(TerrainGroupData& g, TerrainBrushOp op,
	float wx, float wz, float radius, float strength, float target) {
	HeightfieldData& hf = g.heights;
	const uint32_t res = hf.width;
	const float sp = hf.spacing.x;
	uint32_t x0, x1, z0, z1;
	TerrainBrushBox(g, wx, wz, radius, x0, x1, z0, z1);
	const float cx = (wx - hf.origin.x) / sp;
	const float cz = (wz - hf.origin.z) / sp;
	const float r = std::max(radius / sp, 1e-4f);
	const float amount = std::clamp(strength, -1e6f, 1e6f);
	// Smooth reads a pre-stamp snapshot so one stamp's result never
	// depends on its own writes (deterministic replay, safe undo boxes).
	std::vector<float> snapshot;
	if (op == TerrainBrushOp::Smooth)
		snapshot = hf.heights;
	for (uint32_t z = z0; z <= z1; ++z) {
		for (uint32_t x = x0; x <= x1; ++x) {
			const float dx = float(x) - cx;
			const float dz = float(z) - cz;
			const float dist = std::sqrt(dx * dx + dz * dz);
			if (dist >= r)
				continue;
			// Smoothstep falloff 1 -> 0 across the brush.
			const float t = 1.0f - dist / r;
			const float f = t * t * (3.0f - 2.0f * t);
			float& h = hf.heights[(size_t)z * res + x];
			switch (op) {
			case TerrainBrushOp::Raise:
				h += amount * f;
				break;
			case TerrainBrushOp::Lower:
				h -= amount * f;
				break;
			case TerrainBrushOp::Flatten: {
				const float k = f * std::clamp(amount, 0.0f, 1.0f);
				h += (target - h) * k;
				break;
			}
			case TerrainBrushOp::Smooth: {
				// 3x3 box average over the pre-stamp snapshot.
				float acc = 0.0f;
				int n = 0;
				for (int dz2 = -1; dz2 <= 1; ++dz2)
					for (int dx2 = -1; dx2 <= 1; ++dx2) {
						const int64_t sx = int64_t(x) + dx2;
						const int64_t sz = int64_t(z) + dz2;
						if (sx < 0 || sz < 0 || sx >= int64_t(res)
							|| sz >= int64_t(res))
							continue;
						acc += snapshot[(size_t)sz * res + (size_t)sx];
						++n;
					}
				if (n > 0) {
					const float k = f * std::clamp(amount, 0.0f, 1.0f);
					h += (acc / float(n) - h) * k;
				}
				break;
			}
			}
			// Terrains have no overhangs: clamp to a sane height range.
			h = std::clamp(h, -4096.0f, 4096.0f);
		}
	}
}

void TerrainBrushTouchedTiles(uint32_t x0, uint32_t x1, uint32_t z0,
	uint32_t z1, uint32_t res, uint32_t tilesX, uint32_t tilesZ,
	std::vector<uint32_t>& outTiles) {
	outTiles.clear();
	const uint32_t tX = std::max(1u, tilesX);
	const uint32_t tZ = std::max(1u, tilesZ);
	if (res < 2 || x1 < x0 || z1 < z0)
		return;
	// A tile is touched when the box overlaps its inclusive sample range.
	for (uint32_t tz = 0; tz < tZ; ++tz) {
		for (uint32_t tx = 0; tx < tX; ++tx) {
			const uint32_t tx0 = tx * (res - 1) / tX;
			const uint32_t tx1 = (tx + 1 == tX) ? res - 1
				: (tx + 1) * (res - 1) / tX;
			const uint32_t tz0 = tz * (res - 1) / tZ;
			const uint32_t tz1 = (tz + 1 == tZ) ? res - 1
				: (tz + 1) * (res - 1) / tZ;
			if (x1 >= tx0 && x0 <= tx1 && z1 >= tz0 && z0 <= tz1)
				outTiles.push_back(tx | (tz << 16));
		}
	}
}

bool TerrainRaycast(const HeightfieldData& hf,
	const glm::vec3& o, const glm::vec3& d, float maxDist, float& t) {
	if (!hf.valid())
		return false;
	const float sp = hf.spacing.x;
	// March in ~half-sample steps (terrain detail scale), then refine by
	// bisection. Misses cleanly when the ray never dips under the field.
	const float step = std::max(sp * 0.5f, 0.1f);
	const uint32_t steps = uint32_t(std::max(maxDist / step, 1.0f));
	auto heightAt = [&](float x, float z) {
		return hf.sampleHeight(x, z);
	};
	float tPrev = 0.0f;
	float dPrev = o.y - heightAt(o.x, o.z);
	if (dPrev <= 0.0f) {
		t = 0.0f;
		return true; // starts at/below the surface
	}
	for (uint32_t i = 1; i <= steps; ++i) {
		const float ti = float(i) * step;
		if (ti > maxDist)
			break;
		const glm::vec3 p = o + d * ti;
		const float di = p.y - heightAt(p.x, p.z);
		if (di <= 0.0f) {
			// Bisection refine between tPrev (above) and ti (below).
			float lo = tPrev, hi = ti;
			for (int k = 0; k < 12; ++k) {
				const float mid = 0.5f * (lo + hi);
				const glm::vec3 pm = o + d * mid;
				if (pm.y - heightAt(pm.x, pm.z) <= 0.0f)
					hi = mid;
				else
					lo = mid;
			}
			t = hi;
			return true;
		}
		tPrev = ti;
		dPrev = di;
	}
	(void)dPrev;
	return false;
}

void TerrainCopyBox(const TerrainGroupData& g,
	uint32_t x0, uint32_t x1, uint32_t z0, uint32_t z1,
	std::vector<float>& outBox, uint32_t& boxW, uint32_t& boxH) {
	const uint32_t res = g.heights.width;
	boxW = x1 - x0 + 1;
	boxH = z1 - z0 + 1;
	outBox.resize(size_t(boxW) * boxH);
	for (uint32_t z = 0; z < boxH; ++z)
		for (uint32_t x = 0; x < boxW; ++x)
			outBox[(size_t)z * boxW + x] =
				g.heights.heights[(size_t)(z0 + z) * res + (x0 + x)];
}

void TerrainWriteBox(TerrainGroupData& g,
	uint32_t x0, uint32_t x1, uint32_t z0, uint32_t z1,
	const std::vector<float>& box, uint32_t boxW) {
	const uint32_t res = g.heights.width;
	const uint32_t boxH = z1 - z0 + 1;
	for (uint32_t z = 0; z < boxH; ++z)
		for (uint32_t x = 0; x < boxW && x <= x1 - x0; ++x)
			g.heights.heights[(size_t)(z0 + z) * res + (x0 + x)]
				= box[(size_t)z * boxW + x];
}

} // namespace smvk::placement
