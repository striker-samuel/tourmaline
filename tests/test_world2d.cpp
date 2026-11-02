// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Renderer/World2DRenderer.h>
#include <cassert>
#include <array>
#include <cmath>
#include <iostream>
#include <vector>

using namespace tour;

int main() {
	// A 6x6 atlas page fits exactly one padded 4x4 tile.
	SpriteAtlas2D atlas(6);
	const std::vector<uint8_t> red(4 * 4 * 4, 17);
	const uint32_t a = atlas.Insert(4, 4, red);
	const uint32_t b = atlas.Insert(4, 4, red);
	assert(a == 0 && b == 1);
	assert(atlas.PageCount() == 2);
	assert(atlas.Region(a)->page == 0);
	assert(atlas.Region(b)->page == 1);
	assert(atlas.Region(a)->uvMin.x == 1.0f / 6.0f);
	assert(atlas.Region(a)->uvMax.x == 5.0f / 6.0f);
	assert(atlas.PagePixels(0)[0] == 17); // extruded top-left corner
	assert(atlas.PagePixels(0)[(5 * 6 + 5) * 4] == 17);
	assert(atlas.Insert(5, 4, red) == kInvalidSprite2D);
	assert(atlas.Insert(4, 4, std::span<const uint8_t>(red.data(), 1)) ==
		kInvalidSprite2D);
	assert(atlas.PageCount() == 2);

	std::array<Sprite2D, 5> sprites{};
	sprites[0].region = a; sprites[0].z = 3.5f;
	sprites[1].region = b; sprites[1].z = -1.0f;
	sprites[2].region = a; sprites[2].z = 3.5f;
	sprites[3].region = b; sprites[3].z = 3.5f;
	sprites[4].region = kInvalidSprite2D;
	auto batch = BuildSpriteBatch2D(atlas, sprites);
	assert(batch.instances.size() == 4);
	assert(batch.minZ == -1.0f && batch.maxZ == 3.5f);
	assert(batch.runs.size() == 3);
	assert(batch.runs[0].page == 1 && batch.runs[0].firstInstance == 0);
	assert(batch.runs[1].page == 0 && batch.runs[1].count == 2);
	assert(batch.runs[2].page == 1 && batch.runs[2].firstInstance == 3);
	assert(batch.instances[1].z == 3.5f);
	// Stable sort keeps equal-Z insertion order. No texture sorting that
	// would put b between the two a sprites (or overdraw incorrectly).
	sprites[2].z = std::numeric_limits<float>::quiet_NaN();
	assert(BuildSpriteBatch2D(atlas, sprites).instances.size() == 3);
	const auto revision = atlas.Revision();
	atlas.Clear();
	assert(atlas.Revision() != revision && atlas.Region(a) == nullptr);
	std::cout << "world2d atlas/batch tests passed\n";
}
