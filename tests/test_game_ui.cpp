// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Renderer/GameUiRenderer.h>

#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

using namespace tour;

int main() {
	const UiRect2D canvas{{0, 0}, {800, 600}};
	UiLayoutStyle2D root{};
	root.direction = UiLayoutDirection2D::Column;
	root.padding = {20, 10, 20, 10};
	root.gap = 10;
	UiLayout2D layout(canvas, root);
	assert(layout.Depth() == 1);
	const auto header = layout.Next({0, 40});
	assert(header.bounds.origin == glm::vec2(20, 10));
	assert(header.bounds.size == glm::vec2(760, 40));
	const auto content = layout.Next({0, 60});
	assert(content.bounds.origin == glm::vec2(20, 60));
	assert(content.bounds.size == glm::vec2(760, 60));
	const auto fill = layout.Next({0, 0});
	assert(fill.bounds.size == glm::vec2(760, 450));

	UiLayoutStyle2D row{};
	row.direction = UiLayoutDirection2D::Row;
	row.align = UiLayoutAlign2D::Center;
	row.padding = {8, 4, 8, 4};
	row.gap = 6;
	layout.Push(header, row);
	const auto icon = layout.Next({20, 20});
	assert(icon.bounds.origin == glm::vec2(28, 20));
	assert(icon.bounds.size == glm::vec2(20, 20));
	const auto title = layout.Next({0, 16});
	assert(title.bounds.origin == glm::vec2(54, 22));
	assert(title.bounds.size == glm::vec2(718, 16));
	assert(layout.Pop());
	assert(!layout.Pop());

	const UiLayoutItem2D rootItem{canvas, canvas};
	const auto bottomRight = UiLayout2D::Anchor(
		rootItem, {128, 64}, UiAnchor2D::BottomRight, {-16, -16});
	assert(bottomRight.bounds.origin == glm::vec2(656, 520));
	assert(bottomRight.bounds.size == glm::vec2(128, 64));
	assert(bottomRight.bounds.Contains({700, 540}));
	assert(!bottomRight.bounds.Contains({800, 540}));

	UiLayout2D nested(canvas);
	UiLayoutStyle2D nestedStyle{};
	nestedStyle.padding = {4, 4, 4, 4};
	nested.Push({{{780, 570}, {100, 100}}, canvas}, nestedStyle);
	const auto clippedChild = nested.Next({0, 40});
	assert(clippedChild.clip.origin == glm::vec2(780, 570));
	assert(clippedChild.clip.size == glm::vec2(20, 30));

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
	assert(prepared.size() == 3);
	assert(prepared[0].instance.textureIndex == UINT32_MAX);
	assert(prepared[1].texture == image.texture);
	assert(prepared[1].instance.uvMin[0] == 0.25f);
	assert(prepared[2].instance.tint[3] == 0.8f);

	UiDrawRect2D crop{};
	crop.bounds = {{770, 580}, {40, 50}};
	crop.clip = UiRect2D{{780, 590}, {40, 40}};
	const auto trimmed = PrepareGameUiRects(
		std::span<const UiDrawRect2D>(&crop, 1), {800, 600});
	assert(trimmed.size() == 1);
	assert(trimmed[0].instance.origin[0] == 770); // preserve UV geometry
	assert(trimmed[0].instance.clipMin[0] == 780);
	assert(trimmed[0].instance.clipMax[0] == 800);
	assert(trimmed[0].instance.clipMin[1] == 590);
	assert(trimmed[0].instance.clipMax[1] == 600);

	std::cout << "game UI layout/rect tests passed\n";
}
