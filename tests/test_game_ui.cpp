// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Renderer/GameUiRenderer.h>

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>
#include <cstdio>
// House test convention (AGENTS.md): the CHECK macro ALWAYS evaluates its
// condition, in every build flavor. This file originally used CHECK(),
// whose NDEBUG no-op makes Release runs vacuous - and any side effect
// inside an assert (see test_gpu_scene_candidates.cpp:
// assert(leaves.Flush(db))) was silently skipped, crashing the run.
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } \
    while (0)
namespace { int g_checks = 0; int g_failures = 0; }

using namespace tour;

int main() {
	const UiRect2D canvas{{0, 0}, {800, 600}};
	UiLayoutStyle2D root{};
	root.direction = UiLayoutDirection2D::Column;
	root.padding = {20, 10, 20, 10};
	root.gap = 10;
	UiLayout2D layout(canvas, root);
	CHECK(layout.Depth() == 1);
	const auto header = layout.Next({0, 40});
	CHECK(header.bounds.origin == glm::vec2(20, 10));
	CHECK(header.bounds.size == glm::vec2(760, 40));
	const auto content = layout.Next({0, 60});
	CHECK(content.bounds.origin == glm::vec2(20, 60));
	CHECK(content.bounds.size == glm::vec2(760, 60));
	const auto fill = layout.Next({0, 0});
	CHECK(fill.bounds.size == glm::vec2(760, 460));

	UiLayoutStyle2D row{};
	row.direction = UiLayoutDirection2D::Row;
	row.align = UiLayoutAlign2D::Center;
	row.padding = {8, 4, 8, 4};
	row.gap = 6;
	layout.Push(header, row);
	const auto icon = layout.Next({20, 20});
	CHECK(icon.bounds.origin == glm::vec2(28, 20));
	CHECK(icon.bounds.size == glm::vec2(20, 20));
	const auto title = layout.Next({0, 16});
	CHECK(title.bounds.origin == glm::vec2(54, 22));
	CHECK(title.bounds.size == glm::vec2(718, 16));
	CHECK(layout.Pop());
	CHECK(!layout.Pop());

	const UiLayoutItem2D rootItem{canvas, canvas};
	const auto bottomRight = UiLayout2D::Anchor(
		rootItem, {128, 64}, UiAnchor2D::BottomRight, {-16, -16});
	CHECK(bottomRight.bounds.origin == glm::vec2(656, 520));
	CHECK(bottomRight.bounds.size == glm::vec2(128, 64));
	CHECK(bottomRight.bounds.Contains({700, 540}));
	CHECK(!bottomRight.bounds.Contains({800, 540}));

	UiLayout2D nested(canvas);
	UiLayoutStyle2D nestedStyle{};
	nestedStyle.padding = {4, 4, 4, 4};
	nested.Push({{{780, 570}, {100, 100}}, canvas}, nestedStyle);
	const auto clippedChild = nested.Next({0, 40});
	CHECK(clippedChild.clip.origin == glm::vec2(780, 570));
	CHECK(clippedChild.clip.size == glm::vec2(20, 30));

	std::vector<UiDrawRect2D> draws;
	UiDrawRect2D panel{};
	panel.bounds = header.bounds;
	panel.clip = header.clip;
	panel.tint = {0.1f, 0.2f, 0.3f, 0.8f};
	draws.push_back(panel);
	UiDrawRect2D image{};
	image.bounds = icon.bounds;
	image.clip = icon.clip;
	image.texture = smvk::TextureHandle::Mint(4, 2);
	image.uvMin = {0.25f, 0.5f};
	image.uvMax = {0.5f, 0.75f};
	draws.push_back(image);
	draws.push_back(panel); // repeated solid background retains painter order
	UiDrawRect2D outside{};
	outside.bounds = {{900, 10}, {80, 30}};
	draws.push_back(outside);
	UiDrawRect2D bad{};
	bad.bounds = {{0, 0}, {std::numeric_limits<float>::quiet_NaN(), 4}};
	draws.push_back(bad);
	auto prepared = PrepareGameUiRects(draws, {800, 600});
	CHECK(prepared.size() == 3);
	CHECK(prepared[0].instance.textureIndex == UINT32_MAX);
	CHECK(prepared[1].texture == image.texture);
	CHECK(prepared[1].instance.uvMin[0] == 0.25f);
	CHECK(prepared[2].instance.tint[3] == 0.8f);

	UiDrawRect2D crop{};
	crop.bounds = {{770, 580}, {40, 50}};
	crop.clip = UiRect2D{{780, 590}, {40, 40}};
	const auto trimmed = PrepareGameUiRects(
		std::span<const UiDrawRect2D>(&crop, 1), {800, 600});
	CHECK(trimmed.size() == 1);
	CHECK(trimmed[0].instance.origin[0] == 770); // preserve UV geometry
	CHECK(trimmed[0].instance.clipMin[0] == 780);
	CHECK(trimmed[0].instance.clipMax[0] == 800);
	CHECK(trimmed[0].instance.clipMin[1] == 590);
	CHECK(trimmed[0].instance.clipMax[1] == 600);


	// Authored SpriteSets reuse their host-owned atlas; named UVs are
	// converted without packing pixels or allocating a second atlas.
	asset::SpriteSet atlas;
	std::string err;
	CHECK(asset::ParseSpriteSetText(R"(spriteset 1
texture content/ui/panels.png
size 128 64
sprite panel 16 8 40 30 border 6 5 10 7
sprite icon 64 0 16 16
sprite cropped 80 0 8 8 trim 16 16 4 2
)", atlas, err));
	GameUiRenderer ui;
	const auto handle = smvk::TextureHandle::Mint(7, 1);
	CHECK(!ui.DrawSprite(atlas, "absent", {{0, 0}, {10, 10}}, handle));
	CHECK(ui.DrawSprite(atlas, "icon", {{2, 4}, {32, 32}}, handle));
	CHECK(ui.DrawSprite(atlas, "cropped", {{0, 0}, {32, 32}}, handle));
	CHECK(ui.DrawNineSlice(atlas, "panel", {{10, 20}, {100, 60}}, handle));
	CHECK(!ui.DrawNineSlice(atlas, "absent", {{0, 0}, {10, 10}}, handle));
	CHECK(ui.PendingRects().size() == 11); // two sprites + nine slices
	const auto pending = ui.PendingRects();
	CHECK(pending[0].uvMin == glm::vec2(0.5f, 0.0f));
	CHECK(pending[1].bounds.origin == glm::vec2(8, 4));
	CHECK(pending[1].bounds.size == glm::vec2(16, 16));
	CHECK(pending[2].bounds.origin == glm::vec2(10, 20));
	CHECK(pending[2].bounds.size == glm::vec2(6, 5));
	CHECK(pending[3].bounds.origin == glm::vec2(16, 20));
	CHECK(pending[3].bounds.size == glm::vec2(84, 5));
	CHECK(pending[4].bounds.size == glm::vec2(10, 5));
	CHECK(pending[6].bounds.size == glm::vec2(84, 48));
	CHECK(std::abs(pending[2].uvMin.x - 0.125f) < 0.0001f);
	CHECK(std::abs(pending[2].uvMax.x - 0.171875f) < 0.0001f);
	CHECK(pending[6].clip == std::nullopt);

	// The expansion is represented as ordinary painter-ordered rectangles
	// and can be inspected without access to a GPU device.
	const auto* region = atlas.Find("panel");
	CHECK(region != nullptr);
	CHECK(region->border[0] == 6 && region->border[2] == 10);
	const auto uv = atlas.UV(*region);
	CHECK(std::abs(uv[0] - 0.125f) < 0.0001f);
	CHECK(std::abs(uv[1] - 0.125f) < 0.0001f);
	CHECK(ui.DrawNineSlice(atlas, "panel", {{0, 0}, {8, 7}}, handle));
	CHECK(ui.PendingRects().size() == 15); // collapsed center and edges
	const auto small = ui.PendingRects();
	CHECK(std::abs(small[11].bounds.size.x - 3.0f) < 0.0001f);
	CHECK(std::abs(small[11].bounds.size.y - (35.0f / 12.0f)) < 0.0001f);
	CHECK(small[12].bounds.origin.x == 3.0f);
	CHECK(small[13].bounds.origin.y > 0.0f;

	CHECK(ui.DrawNineSlice(atlas, "icon", {{0, 0}, {16, 16}}, handle));
	CHECK(!ui.DrawNineSlice(atlas, "panel", {{0, 0}, {0, 16}}, handle));
	CHECK(!ui.DrawNineSlice(atlas, "panel",
		{{0, 0}, {std::numeric_limits<float>::quiet_NaN(), 16}}, handle));

	std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
