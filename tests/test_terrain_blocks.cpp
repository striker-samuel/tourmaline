// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// TerrainBlocksTests: G-block bake (counts, winding, skirts, AABB, color).
// GPU-less, header-only (terrain_blocks.h).

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include <Engine/Rendergraph/terrain_blocks.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	if (++g_checks, cond)
		std::cout << "PASS " << name << "\n";
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

bool Near(float a, float b, float eps = 1e-3f) {
	return std::fabs(a - b) <= eps;
}

// 16 H-tiles of res*res constant height + 16x16 S-tiles of res*res.
smvk::terrain::TerrainHField FlatField(uint32_t hRes, float h01) {
	using namespace smvk::terrain;
	TerrainHField f;
	f.meta.baseY = -40.0f;
	f.meta.range = 420.0f;
	f.meta.worldSize = 4096.0f;
	f.meta.originX = -2048.0f;
	f.meta.originZ = -2048.0f;
	f.meta.hTileSize = 1024.0f;
	f.meta.hRes = hRes;
	f.tiles.resize(16);
	uint8_t r = 0, g = 0;
	EncodeRG16(h01, r, g);
	for (auto& t : f.tiles) {
		t.assign(size_t(hRes) * hRes * 4, 0);
		for (size_t i = 0; i < size_t(hRes) * hRes; ++i) {
			t[i * 4] = r;
			t[i * 4 + 1] = g;
		}
	}
	return f;
}

smvk::terrain::MaskSet CheckerMasks() {
	using namespace smvk::terrain;
	static std::vector<std::vector<uint8_t>> tiles;
	if (tiles.empty()) {
		tiles.resize(256);
		for (uint32_t tz = 0; tz < 16; ++tz)
			for (uint32_t tx = 0; tx < 16; ++tx) {
				auto& t = tiles[tz * 16 + tx];
				t.resize(8 * 8 * 4, 0);
				for (uint32_t py = 0; py < 8; ++py)
					for (uint32_t px = 0; px < 8; ++px) {
						// World-continuous 32m checker in R (8px per
						// 256m mask tile -> 32m texels).
						const float wxm = float(tx * 256 + px * 32);
						const float wym = float(tz * 256 + py * 32);
						const uint8_t v =
							((uint32_t(wxm / 32) + uint32_t(wym / 32)) % 2 == 0)
							? 255 : 0;
						const size_t o = (size_t(py) * 8 + px) * 4;
						t[o] = v;
						t[o + 3] = 255;
					}
			}
	}
	MaskSet m;
	m.tiles = &tiles;
	m.tilesPerAxis = 16;
	m.res = 8;
	return m;
}

} // namespace

int main() {
	using namespace smvk::terrain;

	// Bake counts at quads=4: 25 surface + 20 skirt verts,
	// 96 surface + 96 skirt indices.
	{
		TerrainHField f = FlatField(8, 0.5f);
		MaskSet noMasks{};
		GBlockMesh b;
		BakeGBlockVerts(f, noMasks, 0, 0, 4, kSkirtDepth, b);
		Check(b.verts.size() == 25 + 20, "blocks.bake-counts-verts");
		std::vector<glm::vec3> bPos;
		for (const auto& v : b.verts)
			bPos.push_back(v.pos);
		std::vector<uint32_t> idx;
		BuildBlockIndices(4, idx, bPos);
		Check(idx.size() == 96 + 96, "blocks.bake-counts-indices");
		// Flat heights -> surface y all equal, AABB exact.
		const float wantY = -40.0f + 0.5f * 420.0f;
		Check(Near(b.verts[0].pos.y, wantY, 0.1f), "blocks.flat-height");
		Check(Near(b.bmin.x, -2048.0f, 1e-3f)
			&& Near(b.bmax.x, -1792.0f, 1e-3f), "blocks.aabb-xz");
		Check(Near(b.bmin.y, wantY - kSkirtDepth, 0.1f)
			&& Near(b.bmax.y, wantY, 0.1f), "blocks.aabb-y-skirt");
		// Skirt ring drops exactly skirtDepth below its edge vert.
		bool skirtsOk = true;
		for (uint32_t k = 0; k < 20; ++k) {
			const uint32_t edge = (k < 5) ? k : (k < 10) ? (4 * 5 + (k - 5))
				: (k < 15) ? ((k - 10) * 5) : ((k - 15) * 5 + 4);
			const float dy = b.verts[edge].pos.y - b.verts[25 + k].pos.y;
			if (!Near(dy, kSkirtDepth, 1e-4f))
				skirtsOk = false;
		}
		Check(skirtsOk, "blocks.skirt-drop");
		// Surface winding: every triangle faces +Y on the flat field.
		bool upOk = true;
		for (size_t k = 0; k < 96; k += 3) {
			const glm::vec3 a = b.verts[idx[k]].pos;
			const glm::vec3 bb = b.verts[idx[k + 1]].pos;
			const glm::vec3 c = b.verts[idx[k + 2]].pos;
			const glm::vec3 n = glm::cross(bb - a, c - a);
			if (!(n.y > 0.0f))
				upOk = false;
		}
		Check(upOk, "blocks.surface-faces-up");
		// Skirt winding: faces away from the block center (horizontal).
		bool outOk = true;
		const glm::vec3 ctr = (b.bmin + b.bmax) * 0.5f;
		for (size_t k = 96; k < idx.size(); k += 3) {
			const glm::vec3 a = b.verts[idx[k]].pos;
			const glm::vec3 bb = b.verts[idx[k + 1]].pos;
			const glm::vec3 c = b.verts[idx[k + 2]].pos;
			const glm::vec3 n = glm::cross(bb - a, c - a);
			const glm::vec3 mid = (a + bb + c) * (1.0f / 3.0f);
			const glm::vec3 away(mid.x - ctr.x, 0.0f, mid.z - ctr.z);
			if (!(glm::dot(glm::vec3(n.x, 0, n.z), away) > 0.0f))
				outOk = false;
		}
		Check(outOk, "blocks.skirts-face-outward");
	}
	// Sloped field: normals tilt, AABB spans the slope.
	{
		TerrainHField f = FlatField(8, 0.0f);
		// East ramp across tile 0: h01 = px/(res-1).
		auto& t = f.tiles[0];
		for (uint32_t py = 0; py < 8; ++py)
			for (uint32_t px = 0; px < 8; ++px) {
				uint8_t r = 0, g = 0;
				EncodeRG16(float(px) / 7.0f, r, g);
				const size_t o = (size_t(py) * 8 + px) * 4;
				t[o] = r;
				t[o + 1] = g;
			}
		MaskSet noMasks{};
		GBlockMesh b;
		BakeGBlockVerts(f, noMasks, 0, 0, 4, kSkirtDepth, b);
		Check(b.bmax.y > b.bmin.y + 1.0f, "blocks.slope-spans-y");
		const glm::vec3 n = SampleNormal(f, -1920.0f, -2000.0f);
		Check(n.x < -0.05f && n.y > 0.9f, "blocks.slope-normal-tilts");
	}
	// Far bake (Terrain IV): counts, skirt flags, shade range, index
	// topology shared with the near pattern.
	{
		TerrainHField f = FlatField(8, 0.5f);
		MaskSet chk = CheckerMasks();
		std::vector<smvk::TerrainFarVertex> far;
		BakeFarBlockVerts(f, chk, 3, 5, far);
		const size_t surf = size_t(kFarQuads + 1) * size_t(kFarQuads + 1);
		Check(far.size() == surf + 4 * (kFarQuads + 1), "blocks.far-counts");
		// Grid coords exact in half (integers to 2048); skirt flags set
		// only on the rings.
		bool gridOk = true;
		for (uint32_t j = 0; j <= kFarQuads; ++j)
			for (uint32_t i = 0; i <= kFarQuads; ++i) {
				const auto& v = far[j * (kFarQuads + 1) + i];
				const float ix = glm::unpackHalf1x16(v.p[0]);
				const float iz = glm::unpackHalf1x16(v.p[1]);
				const float fl = glm::unpackHalf1x16(v.p[3]);
				if (ix != float(i) || iz != float(j) || fl != 0.0f)
					gridOk = false;
			}
		for (size_t k = surf; k < far.size(); ++k) {
			if (glm::unpackHalf1x16(far[k].p[3]) != 1.0f)
				gridOk = false;
		}
		Check(gridOk, "blocks.far-grid-skirt-flags");
		// Shade varies with the checker, stays in [0,1].
		float mnS = 1e30f, mxS = -1e30f;
		for (size_t k = 0; k < surf; ++k) {
			const float s = glm::unpackHalf1x16(far[k].p[2]);
			mnS = std::min(mnS, s);
			mxS = std::max(mxS, s);
			if (!(s >= 0.0f && s <= 1.0f))
				gridOk = false;
		}
		Check(mxS - mnS > 0.2f, "blocks.far-shade-varies");
		// Far topology faces outward on real positions (winding check
		// path with position span, like the runtime far bake).
		std::vector<uint32_t> farIdx;
		{
			std::vector<glm::vec3> pos;
			for (uint32_t j = 0; j <= kFarQuads; ++j)
				for (uint32_t i = 0; i <= kFarQuads; ++i)
					pos.emplace_back(float(i), 0.0f, float(j));
			for (uint32_t k = 0; k < 4 * (kFarQuads + 1); ++k)
				pos.emplace_back(0.0f, -1000.0f, 0.0f);
			BuildBlockIndices(kFarQuads, farIdx, pos);
		}
		Check(farIdx.size()
				== size_t(kFarQuads) * kFarQuads * 6 + 4 * kFarQuads * 6,
			"blocks.far-index-counts");
	}
	// Mask color: checker produces both dark and light verts in a block;
	// absent masks produce uniform dark.
	{
		TerrainHField f = FlatField(8, 0.5f);
		MaskSet chk = CheckerMasks();
		GBlockMesh b;
		// quads=8 (32m step): the step matches the checker square size,
		// so consecutive verts alternate dark/light.
		BakeGBlockVerts(f, chk, 8, 8, 8, kSkirtDepth, b);
		float mnR = 1e30f, mxR = -1e30f;
		for (uint32_t j = 0; j < 9; ++j)
			for (uint32_t i = 0; i < 9; ++i) {
				const float r = b.verts[j * 9 + i].color[0] / 255.0f;
				mnR = std::min(mnR, r);
				mxR = std::max(mxR, r);
			}
		Check(mxR - mnR > 0.2f, "blocks.checker-varies-color");
		GBlockMesh b2;
		MaskSet noMasks{};
		BakeGBlockVerts(f, noMasks, 8, 8, 4, kSkirtDepth, b2);
		float mn2 = 1e30f, mx2 = -1e30f;
		for (uint32_t k = 0; k < 25; ++k) {
			const float r = b2.verts[k].color[0] / 255.0f;
			mn2 = std::min(mn2, r);
			mx2 = std::max(mx2, r);
		}
		Check(mx2 - mn2 < 1e-5f, "blocks.nomask-uniform");
	}

	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
