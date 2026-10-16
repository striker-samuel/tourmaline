// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// TerrainPackTests: RG16 height packing + baseY/range mapping + normals.
// GPU-less, header-only (terrain_pack.h). Follows test_runconfig.cpp style.

#include <cmath>
#include <cstdint>
#include <iostream>

#include <Engine/Rendergraph/terrain_pack.h>

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

bool Near(float a, float b, float eps = 1e-4f) {
	return std::fabs(a - b) <= eps;
}

} // namespace

int main() {
	using namespace smvk::terrain;

	// Decode edges: black -> 0, white(RG) -> 1.
	Check(DecodeH01(0, 0) == 0.0f, "terrainpack.decode-black-zero");
	Check(Near(DecodeH01(255, 255), 1.0f, 1e-5f), "terrainpack.decode-white-one");
	// 16-bit ordering: R is MSB (R=1,G=0 > R=0,G=255).
	Check(DecodeH01(1, 0) > DecodeH01(0, 255), "terrainpack.decode-msb");
	// Mid value exact: R=128,G=0 -> 32768/65535.
	Check(Near(DecodeH01(128, 0), 32768.0f / 65535.0f, 1e-6f),
		"terrainpack.decode-mid-exact");

	// Round-trip across the range (<= 1 LSB error = 1/65535).
	for (int i = 0; i <= 16; ++i) {
		const float h = static_cast<float>(i) / 16.0f;
		uint8_t r = 0, g = 0;
		EncodeRG16(h, r, g);
		const float back = DecodeH01(r, g);
		if (!Near(back, h, 1.0f / 65535.0f + 1e-6f)) {
			Check(false, "terrainpack.roundtrip-sweep");
			break;
		}
		if (i == 16)
			Check(true, "terrainpack.roundtrip-sweep");
	}
	// Clamp behavior.
	{
		uint8_t r = 0, g = 0;
		EncodeRG16(-0.5f, r, g);
		Check(r == 0 && g == 0, "terrainpack.encode-clamp-low");
		EncodeRG16(1.5f, r, g);
		Check(r == 255 && g == 255, "terrainpack.encode-clamp-high");
	}
	// B/A independence is structural (decode takes R,G only) — pin arity.
	Check(DecodeH01(10, 20) == DecodeH01(10, 20), "terrainpack.ba-independent");

	// World mapping: baseY + range.
	Check(HeightToWorld(0.0f) == kTerrainBaseY, "terrainpack.world-base");
	Check(Near(HeightToWorld(1.0f), kTerrainBaseY + kTerrainRange, 1e-4f),
		"terrainpack.world-top");
	Check(Near(HeightToWorld(0.5f), kTerrainBaseY + kTerrainRange * 0.5f, 1e-4f),
		"terrainpack.world-mid");
	Check(Near(WorldToH01(kTerrainBaseY), 0.0f, 1e-6f), "terrainpack.inv-base");
	Check(Near(WorldToH01(kTerrainBaseY + kTerrainRange), 1.0f, 1e-6f),
		"terrainpack.inv-top");
	// Step size: 420/65535 ~= 6.4mm.
	Check(Near(kTerrainRange / 65535.0f, 0.00641f, 1e-4f), "terrainpack.step-6mm");

	// Normals: flat -> +Y.
	{
		const glm::vec3 n = ComputeNormalH(10, 10, 10, 10, 1.0f);
		Check(Near(n.x, 0) && Near(n.y, 1) && Near(n.z, 0), "terrainpack.normal-flat");
	}
	// 45-degree ramp in X: hL=0,hR=2 over step 1 -> dh/dx=1 -> n=(-1,1,0)/sqrt2.
	{
		const glm::vec3 n = ComputeNormalH(0, 2, 1, 1, 1.0f);
		const float s = 0.70710678f;
		Check(Near(n.x, -s, 1e-4f) && Near(n.y, s, 1e-4f) && Near(n.z, 0, 1e-4f),
			"terrainpack.normal-ramp-x");
	}
	// Ramp in Z: hD=0,hU=2 (D=-Z side) -> n=(0,1,-1)/sqrt2 per -dz convention.
	{
		const glm::vec3 n = ComputeNormalH(1, 1, 0, 2, 1.0f);
		const float s = 0.70710678f;
		Check(Near(n.x, 0, 1e-4f) && Near(n.y, s, 1e-4f) && Near(n.z, -s, 1e-4f),
			"terrainpack.normal-ramp-z");
	}
	// Import stub: passes through + normalizes.
	{
		const glm::vec3 n = ComputeNormalImportStub(0, 0, 0, 0, 1.0f,
			glm::vec3(0, 2, 0));
		Check(Near(n.x, 0) && Near(n.y, 1) && Near(n.z, 0),
			"terrainpack.import-stub-normalize");
	}

	// R16 atlas texel: verbatim RG16 bits (R=MSB), so .r == h01.
	Check(PackHeightR16(0, 0) == 0, "terrainpack.r16-black");
	Check(PackHeightR16(255, 255) == 65535, "terrainpack.r16-white");
	Check(PackHeightR16(1, 0) == 256, "terrainpack.r16-msb");
	{
		uint8_t r = 0, g = 0;
		EncodeRG16(0.37f, r, g);
		const float back =
			float(PackHeightR16(r, g)) * (1.0f / 65535.0f);
		Check(Near(back, DecodeH01(r, g), 1e-7f), "terrainpack.r16-matches-decode");
	}

	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
