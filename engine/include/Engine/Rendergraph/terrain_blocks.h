// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// G-block geometry bake (CPU side, header-only for testability).
//
// A G-block is a 256 m square of terrain surface baked from the H-field at
// one of three LOD grids (L0 64 / L1 32 / L2 16 quads per edge) into the
// 32-byte TerrainVertex stream (world space, TerrainFullLayout — the
// article's Terrain I..III full-float layout). Every block shares one
// index buffer PER LOD (BuildBlockIndices); per-block vertex ranges ride
// vertexBase like the old tile field.
//
// Cracks: each block carries a skirt — the border ring duplicated and
// dropped by skirtDepth (3 m). Skirt triangles are wound at bake time to
// face outward (normal check, not convention), so back-face culling never
// eats them.
//
// Color: S-tile RGBA channels are retained as terrain vertex attributes.
// Alpha marks solid ground (0 = hole, 255 = solid); the vertex shaders
// generate NaN clip coordinates for holes and the pixel shader discards
// masked fragments. Normals come from SampleNormal (H-field central differences).

#include <cstdint>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/Renderer/mesh.h>
#include <Engine/Rendergraph/terrain_hfield.h>

namespace smvk::terrain {

// ---- LOD grids (quads per 256 m block edge) ------------------------------
inline constexpr uint32_t kBlockLods[3] = {64, 32, 16};
inline constexpr float kBlockSize = 256.0f;
inline constexpr float kSkirtDepth = 3.0f;

// ---- S-mask sampling (nearest, world-edge clamped) -----------------------
struct MaskSet {
	const std::vector<std::vector<uint8_t>>* tiles = nullptr;
	uint32_t tilesPerAxis = 16; // 4096/256
	uint32_t res = 256;         // texels per mask tile edge
};

[[nodiscard]] inline glm::vec4 SampleMaskRGBA(const MaskSet& m, const TerrainMeta& meta,
	float worldSize, float x, float z) noexcept {
	if (!m.tiles || m.tiles->empty())
		return glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
	const float lx = std::clamp(x - meta.originX, 0.0f, worldSize - 1e-4f);
	const float lz = std::clamp(z - meta.originZ, 0.0f, worldSize - 1e-4f);
	const float tile = worldSize / float(m.tilesPerAxis);
	const uint32_t tx = std::min(uint32_t(lx / tile), m.tilesPerAxis - 1);
	const uint32_t tz = std::min(uint32_t(lz / tile), m.tilesPerAxis - 1);
	const auto& px = (*m.tiles)[size_t(tz) * size_t(m.tilesPerAxis) + size_t(tx)];
	const float texel = tile / float(m.res);
	int32_t ix = int32_t((lx - float(tx) * tile) / texel);
	int32_t iy = int32_t((lz - float(tz) * tile) / texel);
	ix = std::clamp(ix, int32_t(0), int32_t(m.res) - 1);
	iy = std::clamp(iy, int32_t(0), int32_t(m.res) - 1);
	const size_t offset = (size_t(iy) * size_t(m.res) + size_t(ix)) * 4;
	return glm::vec4(px[offset], px[offset + 1], px[offset + 2], px[offset + 3])
		* (1.0f / 255.0f);
}

// Legacy checker/channel accessor for callers that only need R.
[[nodiscard]] inline float SampleMaskR(const MaskSet& m, const TerrainMeta& meta,
	float worldSize, float x, float z) noexcept {
	return SampleMaskRGBA(m, meta, worldSize, x, z).r;
}

struct GBlockMesh {
	std::vector<TerrainVertex> verts;
	glm::vec3 bmin{0.0f};
	glm::vec3 bmax{0.0f};
};

// Bakes one block (gx,gz in 0..15) at `quads` resolution. Appends nothing:
// out is fully rewritten (surface verts row-major + 4 skirt rings).
inline void BakeGBlockVerts(const TerrainHField& hf, const MaskSet& masks,
	uint32_t gx, uint32_t gz, uint32_t quads, float skirtDepth, GBlockMesh& out) {
	const TerrainMeta& meta = hf.meta;
	const float worldSize = meta.worldSize;
	const float x0 = meta.originX + float(gx) * kBlockSize;
	const float z0 = meta.originZ + float(gz) * kBlockSize;
	const float step = kBlockSize / float(quads);
	const float texel = meta.hTileSize / float(meta.hRes); // normal eps
	const uint32_t row = quads + 1;

	out.verts.clear();
	out.verts.reserve(size_t(row) * size_t(row) + size_t(4) * size_t(row));
	float minY = 1e30f, maxY = -1e30f;
	for (uint32_t j = 0; j <= quads; ++j) {
		for (uint32_t i = 0; i <= quads; ++i) {
			const float x = x0 + float(i) * step;
			const float z = z0 + float(j) * step;
			const float y = SampleHeight(hf, x, z);
			const glm::vec3 n = SampleNormal(hf, x, z);
			// Tangent along +X (legacy convention), flip -1.
			const float hx = SampleHeight(hf, x + texel, z)
				- SampleHeight(hf, x - texel, z);
			glm::vec3 t(2.0f * texel, hx, 0.0f);
			const float tl = glm::length(t);
			t = tl > 0.0f ? t * (1.0f / tl) : glm::vec3(1, 0, 0);
			// Preserve all four authored mask channels for terrain materials.
			// A=0 requests a terrain hole, independent of RGB attributes.
			const glm::vec4 attributes = SampleMaskRGBA(masks, meta, worldSize, x, z);
			TerrainVertexSrc src{};
			src.pos = glm::vec3(x, y, z);
			src.normal = n;
			src.tangent = t;
			src.tangentFlip = -1.0f;
			src.color = attributes;
			TerrainVertex v{};
			PackTerrainVertex(src, v);
			out.verts.push_back(v);
			if (y < minY)
				minY = y;
			if (y > maxY)
				maxY = y;
		}
	}
	// Skirt rings: bottom(j=0), top(j=quads), left(i=0), right(i=quads).
	// Winding is fixed in BuildBlockIndices; here only positions drop.
	auto dropRing = [&](uint32_t idx) {
		TerrainVertex v = out.verts[idx];
		v.pos.y -= skirtDepth;
		out.verts.push_back(v);
	};
	for (uint32_t i = 0; i <= quads; ++i)
		dropRing(i); // bottom
	const uint32_t topBase = quads * row;
	for (uint32_t i = 0; i <= quads; ++i)
		dropRing(topBase + i); // top
	for (uint32_t j = 0; j <= quads; ++j)
		dropRing(j * row); // left
	for (uint32_t j = 0; j <= quads; ++j)
		dropRing(j * row + quads); // right
	out.bmin = glm::vec3(x0, minY - skirtDepth, z0);
	out.bmax = glm::vec3(x0 + kBlockSize, maxY, z0 + kBlockSize);
}

// Shared per-LOD topology: surface quads (up-facing, legacy winding) +
// skirt quads wound to face outward (verified per quad from the given
// surface positions, so border height variation can't flip them; an empty
// span skips the check and emits the default pattern).
inline void BuildBlockIndices(uint32_t quads, std::vector<uint32_t>& out,
	std::span<const glm::vec3> positions = {}) {
	out.clear();
	const uint32_t row = quads + 1;
	out.reserve(size_t(quads) * size_t(quads) * 6 + size_t(4) * size_t(quads) * 6);
	for (uint32_t j = 0; j < quads; ++j) {
		for (uint32_t i = 0; i < quads; ++i) {
			const uint32_t a = j * row + i;
			const uint32_t b = a + 1u;
			const uint32_t c = a + row;
			const uint32_t e = c + 1u;
			out.push_back(a);
			out.push_back(c);
			out.push_back(b);
			out.push_back(b);
			out.push_back(c);
			out.push_back(e);
		}
	}
	// Skirt quads: ring r (0=bottom,1=top,2=left,3=right), segment i.
	// Ring base offsets in the appended region.
	const uint32_t ringSize = row;
	const uint32_t surfCount = row * row;
	auto edgeVert = [&](uint32_t ring, uint32_t i) -> uint32_t {
		switch (ring) {
		case 0: return i;
		case 1: return quads * row + i;
		case 2: return i * row;
		default: return i * row + quads;
		}
	};
	for (uint32_t ring = 0; ring < 4; ++ring) {
		const uint32_t ringBase = surfCount + ring * ringSize;
		for (uint32_t i = 0; i < quads; ++i) {
			const uint32_t a = edgeVert(ring, i);
			const uint32_t b = edgeVert(ring, i + 1);
			const uint32_t sa = ringBase + i;
			const uint32_t sb = ringBase + i + 1;
			// Candidate (a,b,sa)+(b,sb,sa); flip when facing inward.
			bool flip = false;
			if (positions.size() > sa) {
				const glm::vec3 pa = positions[a];
				const glm::vec3 pb = positions[b];
				const glm::vec3 ps = positions[sa];
				glm::vec3 fn = glm::cross(pb - pa, ps - pa);
				// Outward reference: edge midpoint pushed away from the
				// block center, purely horizontal.
				glm::vec3 mid = (pa + pb) * 0.5f;
				glm::vec3 ctr = positions[surfCount / 2];
				glm::vec3 away(mid.x - ctr.x, 0.0f, mid.z - ctr.z);
				if (glm::dot(fn, away) < 0.0f)
					flip = true;
			}
			if (!flip) {
				out.insert(out.end(), {a, b, sa, b, sb, sa});
			}
			else {
				out.insert(out.end(), {a, sa, b, b, sa, sb});
			}
		}
	}
}

// ---- Far-block bake: grid coords + full RGBA terrain attributes ---------
// quads=16 fixed (kFarQuads); world XZ reconstructs in the VS from the
// per-block push tail, height samples the R16 atlas.
inline constexpr uint32_t kFarQuads = 16;

inline void BakeFarBlockVerts(const TerrainHField& hf, const MaskSet& masks,
	uint32_t gx, uint32_t gz, std::vector<TerrainFarVertex>& out) {
	const TerrainMeta& meta = hf.meta;
	const float worldSize = meta.worldSize;
	const float x0 = meta.originX + float(gx) * kBlockSize;
	const float z0 = meta.originZ + float(gz) * kBlockSize;
	const float step = kBlockSize / float(kFarQuads);
	const uint32_t row = kFarQuads + 1;
	out.clear();
	out.reserve(size_t(row) * size_t(row) + size_t(4) * size_t(row));
	for (uint32_t j = 0; j <= kFarQuads; ++j) {
		for (uint32_t i = 0; i <= kFarQuads; ++i) {
			const float x = x0 + float(i) * step;
			const float z = z0 + float(j) * step;
			const glm::vec4 attributes =
				SampleMaskRGBA(masks, meta, worldSize, x, z);
			TerrainFarVertex v{};
			PackTerrainFarVertex(float(i), float(j), attributes, false, v);
			out.push_back(v);
		}
	}
	for (uint32_t i = 0; i <= kFarQuads; ++i) {
		TerrainFarVertex v{};
		const float x = x0 + float(i) * step; // bottom ring
		PackTerrainFarVertex(float(i), 0.0f,
			SampleMaskRGBA(masks, meta, worldSize, x, z0), true, v);
		out.push_back(v);
	}
	for (uint32_t i = 0; i <= kFarQuads; ++i) {
		TerrainFarVertex v{};
		const float x = x0 + float(i) * step; // top ring
		PackTerrainFarVertex(float(i), float(kFarQuads),
			SampleMaskRGBA(masks, meta, worldSize, x, z0 + kBlockSize), true, v);
		out.push_back(v);
	}
	for (uint32_t j = 0; j <= kFarQuads; ++j) {
		TerrainFarVertex v{};
		const float z = z0 + float(j) * step; // left ring
		PackTerrainFarVertex(0.0f, float(j),
			SampleMaskRGBA(masks, meta, worldSize, x0, z), true, v);
		out.push_back(v);
	}
	for (uint32_t j = 0; j <= kFarQuads; ++j) {
		TerrainFarVertex v{};
		const float z = z0 + float(j) * step; // right ring
		PackTerrainFarVertex(float(kFarQuads), float(j),
			SampleMaskRGBA(masks, meta, worldSize, x0 + kBlockSize, z), true, v);
		out.push_back(v);
	}
}

} // namespace smvk::terrain
