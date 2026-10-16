// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// TerrainHFieldTests: CPU height-field sampling + meta JSON contract.
// GPU-less, header-only (terrain_hfield.h, terrain_meta.h).

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <Engine/Asset/pak.h>
#include <Engine/Rendergraph/terrain_hfield.h>
#include <Engine/Rendergraph/terrain_meta.h>

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

smvk::terrain::TerrainHField MakeField(float fillH01) {
	using namespace smvk::terrain;
	TerrainHField f;
	f.meta.baseY = -40.0f;
	f.meta.range = 420.0f;
	f.meta.worldSize = 4096.0f;
	f.meta.originX = -2048.0f;
	f.meta.originZ = -2048.0f;
	f.meta.hTileSize = 1024.0f;
	f.meta.hRes = 32; // small for test speed (same code path)
	f.tiles.resize(16);
	uint8_t r = 0, g = 0;
	EncodeRG16(fillH01, r, g);
	for (auto& t : f.tiles) {
		t.resize(size_t(32) * 32 * 4);
		for (size_t i = 0; i < 32 * 32; ++i) {
			t[i * 4] = r;
			t[i * 4 + 1] = g;
			t[i * 4 + 2] = 0;
			t[i * 4 + 3] = 0;
		}
	}
	return f;
}

} // namespace

int main() {
	using namespace smvk::terrain;

	// valid() gates on 16 tiles x hRes^2 RGBA8.
	{
		TerrainHField f = MakeField(0.5f);
		Check(f.valid(), "hfield.valid-full");
		f.tiles.pop_back();
		Check(!f.valid(), "hfield.valid-rejects-short");
	}
	// Constant field samples exactly (1-LSB tolerance).
	{
		TerrainHField f = MakeField(0.25f);
		Check(Near(SampleH01(f, 0.0f, 0.0f), 0.25f, 1e-4f),
			"hfield.const-center");
		Check(Near(SampleH01(f, -2048.0f, -2048.0f), 0.25f, 1e-4f),
			"hfield.const-corner");
		Check(Near(SampleHeight(f, 100.0f, -300.0f), -40.0f + 0.25f * 420.0f, 0.05f),
			"hfield.const-world");
		const glm::vec3 n = SampleNormal(f, 0.0f, 0.0f);
		Check(Near(n.x, 0) && Near(n.y, 1) && Near(n.z, 0),
			"hfield.const-normal-up");
	}
	// Per-tile constants: tile select maps world -> right tile.
	{
		TerrainHField f = MakeField(0.0f);
		for (uint32_t tz = 0; tz < 4; ++tz)
			for (uint32_t tx = 0; tx < 4; ++tx) {
				uint8_t r = 0, g = 0;
				EncodeRG16(float(tz * 4 + tx) / 15.0f, r, g);
				auto& t = f.tiles[tz * 4 + tx];
				for (size_t i = 0; i < 32 * 32; ++i) {
					t[i * 4] = r;
					t[i * 4 + 1] = g;
				}
			}
		// Tile (2,1) center: x=-2048+2.5*1024=512, z=-2048+1.5*1024=-512.
		Check(Near(SampleH01(f, 512.0f, -512.0f), 6.0f / 15.0f, 1e-3f),
			"hfield.tile-select");
		// Outside the world clamps to the edge tile, not OOB.
		Check(Near(SampleH01(f, -99999.0f, 99999.0f),
				SampleH01(f, -2048.0f, 2047.0f), 1e-6f),
			"hfield.clamp-edge");
	}
	// Bilinear: horizontal ramp inside one tile, sampled at a texel center
	// (exact) and halfway between centers (average).
	{
		TerrainHField f = MakeField(0.0f);
		auto& t = f.tiles[0];
		for (uint32_t py = 0; py < 32; ++py)
			for (uint32_t px = 0; px < 32; ++px) {
				uint8_t r = 0, g = 0;
				EncodeRG16(float(px) / 31.0f, r, g);
				const size_t o = (size_t(py) * 32 + px) * 4;
				t[o] = r;
				t[o + 1] = g;
			}
		// Tile 0 spans x in [-2048,-1024); texel = 1024/32 = 32m.
		// Texel 7 center: x = -2048 + 7.5*32 = -1808.
		Check(Near(SampleH01(f, -1808.0f, -2000.0f), 7.0f / 31.0f, 2e-3f),
			"hfield.bilinear-center-exact");
		// Halfway between texel 7 and 8 centers: x = -2048 + 8*32 = -1792.
		Check(Near(SampleH01(f, -1792.0f, -2000.0f), 7.5f / 31.0f, 2e-3f),
			"hfield.bilinear-mid-average");
	}
	// TileMinMax on a constant tile.
	{
		TerrainHField f = MakeField(0.75f);
		float mn = 0, mx = 0;
		TileMinMax(f.tiles[5], 32, -40.0f, 420.0f, mn, mx);
		Check(Near(mn, -40.0f + 0.75f * 420.0f, 0.1f)
			&& Near(mx, mn, 0.01f),
			"hfield.minmax-const");
	}
	// Meta JSON: accept the generator schema, reject mutations.
	{
		TerrainMeta m;
		std::string err;
		const char* good = "{\"baseY\": -40.0, \"range\": 420.0, "
			"\"worldSize\": 4096.0, \"origin\": [-2048.0, -2048.0], "
			"\"hTileSize\": 1024.0, \"hRes\": 1024, \"gBlock\": 256.0, "
			"\"packing\": \"rg16\"}";
		Check(ParseTerrainJson(good, m, err), "hfield.json-good");
		Check(Near(m.baseY, -40.0f, 1e-5f) && Near(m.range, 420.0f, 1e-5f)
			&& Near(m.originX, -2048.0f, 1e-4f) && m.hRes == 1024u
			&& m.packingRg16,
			"hfield.json-values");
		const char* unknown = "{\"baseY\": -40.0, \"range\": 420.0, "
			"\"worldSize\": 4096.0, \"origin\": [-2048.0, -2048.0], "
			"\"hTileSize\": 1024.0, \"hRes\": 1024, \"gBlock\": 256.0, "
			"\"packing\": \"rg16\", \"oops\": 1}";
		Check(!ParseTerrainJson(unknown, m, err), "hfield.json-reject-unknown");
		const char* badRes = "{\"baseY\": -40.0, \"range\": 420.0, "
			"\"worldSize\": 4096.0, \"origin\": [-2048.0, -2048.0], "
			"\"hTileSize\": 1024.0, \"hRes\": 512, \"gBlock\": 256.0, "
			"\"packing\": \"rg16\"}";
		Check(!ParseTerrainJson(badRes, m, err), "hfield.json-reject-hres");
		const char* badPack = "{\"baseY\": -40.0, \"range\": 420.0, "
			"\"worldSize\": 4096.0, \"origin\": [-2048.0, -2048.0], "
			"\"hTileSize\": 1024.0, \"hRes\": 1024, \"gBlock\": 256.0, "
			"\"packing\": \"ra16\"}";
		Check(!ParseTerrainJson(badPack, m, err), "hfield.json-reject-packing");
		const char* missing = "{\"baseY\": -40.0, \"range\": 420.0}";
		Check(!ParseTerrainJson(missing, m, err), "hfield.json-reject-missing");
		std::string trailing = std::string(good) + " ";
		Check(ParseTerrainJson(trailing, m, err), "hfield.json-trailing-ws-ok");
		Check(!ParseTerrainJson(std::string(good) + "x", m, err),
			"hfield.json-reject-trailing");
	}
	// Naming helpers match the generator.
	Check(HTileName(0, 0) == "h_00_00", "hfield.name-h");
	Check(STileName(15, 3) == "m_15_03", "hfield.name-m");
	Check(std::string(kTerrainMetaBlobPath) == "terrain/meta",
		"hfield.name-meta");

	// Meta blob round-trip (the runtime form).
	{
		TerrainMeta m;
		m.baseY = -40.0f;
		m.range = 420.0f;
		m.worldSize = 4096.0f;
		m.originX = -2048.0f;
		m.originZ = -2048.0f;
		m.hTileSize = 1024.0f;
		m.hRes = 1024;
		m.gBlock = 256.0f;
		float mm[16][2];
		for (int i = 0; i < 16; ++i) {
			mm[i][0] = -40.0f + float(i);
			mm[i][1] = -40.0f + float(i) + 10.0f;
		}
		std::vector<uint8_t> blob;
		SerializeTerrainMeta(m, mm, blob);
		Check(blob.size() == kTerrainMetaBlobSize, "hfield.metablob-size");
		TerrainMeta back;
		float mmBack[16][2] = {};
		Check(ParseTerrainMetaBlob(blob, back, mmBack), "hfield.metablob-parse");
		Check(Near(back.baseY, -40.0f, 1e-6f) && Near(back.range, 420.0f, 1e-5f)
			&& back.hRes == 1024u, "hfield.metablob-values");
		Check(Near(mmBack[7][0], -33.0f, 1e-5f)
			&& Near(mmBack[7][1], -23.0f, 1e-5f), "hfield.metablob-minmax");
		blob[0] ^= 0xFF; // corrupt magic
		Check(!ParseTerrainMetaBlob(blob, back, mmBack),
			"hfield.metablob-reject-magic");
	}

	// Pak layer: SMP6 Terrain round-trip + the SMP5 legacy-empty hack.
	{
		using namespace smvk;
		// SMP6 writer with one Terrain blob re-opens with the blob visible.
		PakWriter w;
		std::vector<uint8_t> payload(64, 0xAB);
		Check(w.AddBlob(PakAssetKind::Terrain, "terrain/h_00_00", payload),
			"hfield.pak-add-terrain");
		std::vector<uint8_t> image = w.Finalize();
		std::string perr;
		auto reader = PakReader::Open(image, perr);
		Check(reader.has_value(), "hfield.pak-smp6-open");
		if (reader) {
			Check(reader->BlobsOfKind(PakAssetKind::Terrain).size() == 1,
				"hfield.pak-smp6-terrain-visible");
		}
		// Forge an SMP5-epoch image (same bytes, old magics, fixed CRC):
		// with a Terrain blob the legacy hack must fail closed.
		auto forgeSmp5 = [](const std::vector<uint8_t>& src) {
			std::vector<uint8_t> img = src;
			memcpy(img.data() + 0, &kPakMagicSmp5, 4);
			const size_t fOff = img.size() - 24;
			memcpy(img.data() + fOff + 20, &kPakFooterMagicKpm5, 4);
			const uint32_t crc = PakCrc32(img.data(), fOff + 16);
			memcpy(img.data() + fOff + 16, &crc, 4);
			return img;
		};
		{
			std::vector<uint8_t> legacy = forgeSmp5(image);
			std::string lerr;
			Check(!PakReader::Open(legacy, lerr),
				"hfield.pak-smp5-terrain-rejected");
		}
		// Same forge without Terrain blobs: legacy-empty hack accepts.
		{
			PakWriter w2;
			std::vector<uint8_t> tex(32, 0x11);
			Check(w2.AddBlob(PakAssetKind::Texture, "textures/x.ktx2", tex),
				"hfield.pak-add-texture");
			std::vector<uint8_t> img2 = w2.Finalize();
			std::vector<uint8_t> legacy2 = forgeSmp5(img2);
			std::string lerr;
			auto r2 = PakReader::Open(legacy2, lerr);
			Check(r2.has_value(), "hfield.pak-smp5-empty-accepted");
			if (r2) {
				Check(r2->BlobsOfKind(PakAssetKind::Terrain).empty(),
					"hfield.pak-smp5-no-terrain");
			}
		}
	}

	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
