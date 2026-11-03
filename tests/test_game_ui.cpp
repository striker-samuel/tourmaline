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

	std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
