// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Placement/placement_terrain.h>
#include <Engine/Placement/placement_eval.h>

#include <algorithm>
#include <cmath>

namespace tour::placement {
using namespace ::smvk;

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

glm::vec2 TerrainTileOrigin(const TerrainGroupData& g, uint32_t tx, uint32_t tz) {
	uint32_t x0, x1, z0, z1;
	TerrainTileRange(g, tx, tz, x0, x1, z0, z1);
	// The tile's group-local XZ offset: its actual first sample (NOT the
	// even metric split — see the header contract). Uses the same
	// per-axis spacing as BuildTerrainTileUpload so tile tx's last column
	// lands exactly on tile tx+1's first column.
	return glm::vec2(float(x0) * g.heights.spacing.x,
		float(z0) * g.heights.spacing.y);
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
	// A tile needs >= TWO samples per axis (one interval): a finer split
	// divides by w-1 == 0 for the UVs below. The interval split
	// guarantees every tile spans >= 1 interval exactly when
	// tiles <= intervals per axis.
	if (tX > res - 1 || tZ > res - 1)
		return false;
	uint32_t x0, x1, z0, z1;
	TerrainTileRange(g, tx, tz, x0, x1, z0, z1);
	const uint32_t w = x1 - x0 + 1;
	const uint32_t h = z1 - z0 + 1;
	// Belt-and-braces: never emit a one-sample tile even if the split
	// math above ever drifts.
	if (w < 2 || h < 2)
		return false;
	// Per-axis spacing (equal for every producer today; kept per-axis so
	// non-square spacing would still seam correctly).
	const float spx = hf.spacing.x;
	const float spz = hf.spacing.y;
	const float invW = 1.0f / float(w - 1);
	const float invH = 1.0f / float(h - 1);
	out.vertices.reserve((size_t)w * h);
	out.indices.reserve((size_t)(w - 1) * (h - 1) * 6);
	glm::vec3 mn(1e30f), mx(-1e30f);
	for (uint32_t z = 0; z < h; ++z) {
		for (uint32_t x = 0; x < w; ++x) {
			const uint32_t gx = x0 + x, gz = z0 + z;
			const float lx = float(x) * spx;
			const float lz = float(z) * spz;
			// Normals sample the GLOBAL field so edge verts match across
			// neighbouring tiles (same absolute XZ -> same central diff).
			const float wx = hf.origin.x + float(gx) * spx;
			const float wz = hf.origin.z + float(gz) * spz;
			const float hgt = hf.heights[(size_t)gz * res + gx];
			const glm::vec3 n = hf.sampleNormal(wx, wz);
			glm::vec3 t = glm::cross(glm::vec3(0, 1, 0), n);
			if (glm::dot(t, t) < 1e-8f)
				t = glm::vec3(1, 0, 0);
			t = glm::normalize(t);
			ImportStaticVertex v{};
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
	if (!hf.valid() || !(maxDist > 0.0f))
		return false;
	// Clip the segment [0, maxDist] to the heightfield's XZ domain FIRST.
	// sampleHeight() edge-clamps OUTSIDE reads, so an unclipped march
	// would hit the boundary wall extruded to infinity — a ray starting
	// outside and below the boundary height would report a false t = 0.
	// Hits only exist inside the domain.
	const float minX = hf.origin.x;
	const float maxX = hf.origin.x + float(hf.width - 1) * hf.spacing.x;
	const float minZ = hf.origin.z;
	const float maxZ = hf.origin.z + float(hf.height - 1) * hf.spacing.y;
	float sLo = 0.0f;
	float sHi = maxDist;
	const float pos[2] = { o.x, o.z };
	const float dir[2] = { d.x, d.z };
	const float lo[2] = { minX, minZ };
	const float hi[2] = { maxX, maxZ };
	for (int a = 0; a < 2; ++a) {
		if (std::abs(dir[a]) < 1e-9f) {
			if (pos[a] < lo[a] || pos[a] > hi[a])
				return false; // parallel to this wall + outside: never over
			continue;
		}
		float ta0 = (lo[a] - pos[a]) / dir[a];
		float ta1 = (hi[a] - pos[a]) / dir[a];
		if (ta0 > ta1)
			std::swap(ta0, ta1);
		sLo = std::max(sLo, ta0);
		sHi = std::min(sHi, ta1);
		if (sLo > sHi)
			return false; // the segment never enters the domain
	}
	// March in ~half-sample steps (terrain detail scale), then refine by
	// bisection. The LAST sample lands exactly on the clipped segment end
	// (the domain exit or maxDist, whichever comes first) so the endpoint
	// is ALWAYS evaluated — a ray shorter than one step, or a maxDist
	// that is not a step multiple, still tests its full length.
	const float step = std::max(hf.spacing.x * 0.5f, 0.1f);
	const glm::vec3 p0 = o + d * sLo;
	float tPrev = sLo;
	if (p0.y - hf.sampleHeight(p0.x, p0.z) <= 0.0f) {
		t = sLo;
		return true; // enters the domain at/below the surface
	}
	const uint32_t steps = uint32_t(std::max((sHi - sLo) / step, 1.0f));
	for (uint32_t i = 1; i <= steps; ++i) {
		const float ti = (i == steps) ? sHi : sLo + float(i) * step;
		const glm::vec3 p = o + d * ti;
		if (p.y - hf.sampleHeight(p.x, p.z) <= 0.0f) {
			// Bisection refine between tPrev (above) and ti (below).
			float lo2 = tPrev, hi2 = ti;
			for (int k = 0; k < 12; ++k) {
				const float mid = 0.5f * (lo2 + hi2);
				const glm::vec3 pm = o + d * mid;
				if (pm.y - hf.sampleHeight(pm.x, pm.z) <= 0.0f)
					hi2 = mid;
				else
					lo2 = mid;
			}
			t = hi2;
			return true;
		}
		tPrev = ti;
	}
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

// ---------------------------------------------------------------------------
// WORLD-TILE BRIDGE (see placement_terrain.h's contract block)
// ---------------------------------------------------------------------------
namespace {

// The inclusive world-texel window covering [rectMin, rectMax) (+/- halo),
// clamped to the world's texel grid. Returns false when the rect lies
// entirely outside the world.
bool WorldTexelWindow(const smvk::terrain::TerrainMeta& meta,
	float texel, const glm::vec2& rectMin, const glm::vec2& rectMax,
	int32_t halo, uint32_t worldTexels,
	int32_t& ix0, int32_t& ix1, int32_t& iz0, int32_t& iz1) {
	ix0 = (int32_t)std::floor((rectMin.x - meta.originX) / texel) - halo;
	ix1 = (int32_t)std::ceil((rectMax.x - meta.originX) / texel) - 1 + halo;
	iz0 = (int32_t)std::floor((rectMin.y - meta.originZ) / texel) - halo;
	iz1 = (int32_t)std::ceil((rectMax.y - meta.originZ) / texel) - 1 + halo;
	ix0 = std::max(ix0, 0);
	iz0 = std::max(iz0, 0);
	ix1 = std::min(ix1, (int32_t)worldTexels - 1);
	iz1 = std::min(iz1, (int32_t)worldTexels - 1);
	return ix1 >= ix0 && iz1 >= iz0;
}

} // namespace

bool HarvestWorldHeightfield(const smvk::terrain::TerrainHField& field,
	const glm::vec2& rectMin, const glm::vec2& rectMax, uint32_t halo,
	HeightfieldData& out) {
	out = HeightfieldData{};
	if (!field.valid()) return false;
	const auto& meta = field.meta;
	const uint32_t n = field.tilesPerAxis();
	if (n == 0 || meta.hRes == 0) return false;
	const float texel = meta.hTileSize / float(meta.hRes);
	const uint32_t worldTexels = uint32_t(
		std::lround(meta.worldSize / texel));
	int32_t ix0, ix1, iz0, iz1;
	if (!WorldTexelWindow(meta, texel, rectMin, rectMax, int32_t(halo),
			worldTexels, ix0, ix1, iz0, iz1))
		return false;
	const uint32_t w = uint32_t(ix1 - ix0 + 1);
	const uint32_t h = uint32_t(iz1 - iz0 + 1);
	out.width = w;
	out.height = h;
	// +half a texel: placement texel i == world texel (base+i)'s CENTER
	// (the documented convention bridge).
	out.origin = glm::vec3(
		meta.originX + (float(ix0) + 0.5f) * texel, 0.0f,
		meta.originZ + (float(iz0) + 0.5f) * texel);
	out.spacing = glm::vec2(texel, texel);
	out.heights.resize((size_t)w * h);
	for (int32_t iz = iz0; iz <= iz1; ++iz) {
		const uint32_t tz = uint32_t(iz) / meta.hRes;
		const uint32_t py = uint32_t(iz) % meta.hRes;
		for (int32_t ix = ix0; ix <= ix1; ++ix) {
			const uint32_t tx = uint32_t(ix) / meta.hRes;
			const uint32_t pxx = uint32_t(ix) % meta.hRes;
			const auto& tile = field.tiles[size_t(tz) * size_t(n) + size_t(tx)];
			const size_t o = (size_t(py) * size_t(meta.hRes) + size_t(pxx)) * 4;
			out.heights[(size_t(iz - iz0)) * w + size_t(ix - ix0)]
				= smvk::terrain::HeightToWorld(
					smvk::terrain::DecodeH01(tile[o], tile[o + 1]));
		}
	}
	return out.valid();
}

bool HarvestWorldEcotope(const smvk::terrain::TerrainMeta& meta,
	const WorldMaskGrid& masks,
	const glm::vec2& rectMin, const glm::vec2& rectMax,
	EcotopeMaskData& out) {
	out = EcotopeMaskData{};	if (!masks.Valid()) return false;
	const float texel = meta.gBlock / float(masks.tileRes);
	const uint32_t worldTexels = masks.tilesPerAxis * masks.tileRes;
	int32_t ix0, ix1, iz0, iz1;
	if (!WorldTexelWindow(meta, texel, rectMin, rectMax, 0,
			worldTexels, ix0, ix1, iz0, iz1))
		return false;
	const uint32_t w = uint32_t(ix1 - ix0 + 1);
	const uint32_t h = uint32_t(iz1 - iz0 + 1);
	out.width = w;
	out.height = h;
	out.channels = 4; // RGBA = the paint layers (the FieldOp channel index)
	out.origin = glm::vec2(
		meta.originX + (float(ix0) + 0.5f) * texel,
		meta.originZ + (float(iz0) + 0.5f) * texel);
	out.spacing = glm::vec2(texel, texel);
	out.weights.assign((size_t)4 * h * w, 0.0f);
	for (int32_t iz = iz0; iz <= iz1; ++iz) {
		const uint32_t tz = uint32_t(iz) / masks.tileRes;
		const uint32_t py = uint32_t(iz) % masks.tileRes;
		for (int32_t ix = ix0; ix <= ix1; ++ix) {
			const uint32_t tx = uint32_t(ix) / masks.tileRes;
			const uint32_t pxx = uint32_t(ix) % masks.tileRes;
			const auto& tile =
				(*masks.tiles)[size_t(tz) * size_t(masks.tilesPerAxis) + size_t(tx)];
			const size_t o = (size_t(py) * size_t(masks.tileRes) + size_t(pxx)) * 4;
			for (uint32_t c = 0; c < 4; ++c)
				out.weights[((size_t)c * h + size_t(iz - iz0)) * w + size_t(ix - ix0)]
					= float(tile[o + c]) * (1.0f / 255.0f);
		}
	}
	return out.valid();
}

bool BuildWorldTileDomain(const smvk::terrain::TerrainHField& field,
	const WorldMaskGrid& masks, float tileSize,
	int32_t tileX, int32_t tileZ, float haloMeters,
	WorldTileData& out) {
	out = WorldTileData{};
	if (tileSize <= 0.0f || !field.valid() || !masks.Valid()) return false;
	const auto& meta = field.meta;
	const glm::vec2 rectMin(float(tileX) * tileSize, float(tileZ) * tileSize);
	const glm::vec2 rectMax = rectMin + glm::vec2(tileSize);
	// The world-rect bounds check (the harvest clamps interiors, but a
	// fully-outside tile refuses honestly).
	const glm::vec2 worldMin(meta.originX, meta.originZ);
	const glm::vec2 worldMax = worldMin + glm::vec2(meta.worldSize);
	if (rectMax.x <= worldMin.x || rectMax.y <= worldMin.y
		|| rectMin.x >= worldMax.x || rectMin.y >= worldMax.y)
		return false;
	// Heights: the tile rect + the halo (in texels — the window grows
	// so the normals' central differences stay interior).
	const float texel = meta.hTileSize / float(meta.hRes);
	const uint32_t halo = haloMeters > 0.0f
		? uint32_t(std::ceil(haloMeters / texel)) : 0;
	if (!HarvestWorldHeightfield(field, rectMin, rectMax, halo, out.heights))
		return false;
	// The domain extent = the HALF-OPEN tile rect (candidates live in
	// [min, max); the window around it is read-only halo).
	out.domain.field = &out.heights;
	out.domain.extentMin = rectMin;
	out.domain.extentMax = rectMax;
	// The paint window: the same rect (the eco sampler edge-clamps into
	// the halo naturally; a window halo keeps the mask's edge reads off
	// the clamped border when the halo was earned).
	if (!HarvestWorldEcotope(meta, masks, rectMin, rectMax, out.eco))
		return false;
	return true;
}

} // namespace tour::placement
