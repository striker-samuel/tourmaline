// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Pixel-coordinate, GPU-independent layout for in-game HUDs and menus.
#include <glm/glm.hpp>

#include <cstddef>
#include <vector>

namespace tour {

struct UiRect2D {
	glm::vec2 origin{0.0f};
	glm::vec2 size{0.0f};

	[[nodiscard]] float Right() const { return origin.x + size.x; }
	[[nodiscard]] float Bottom() const { return origin.y + size.y; }
	[[nodiscard]] bool Contains(glm::vec2 p) const {
		return p.x >= origin.x && p.y >= origin.y &&
			p.x < Right() && p.y < Bottom();
	}
};

[[nodiscard]] UiRect2D IntersectUiRects(UiRect2D a, UiRect2D b);

struct UiLayoutItem2D {
	UiRect2D bounds{};
	UiRect2D clip{}; // inherited clip; renderers intersect it with bounds
};

enum class UiLayoutDirection2D { Row, Column };
enum class UiLayoutAlign2D { Start, Center, End, Stretch };
enum class UiAnchor2D {
	TopLeft, TopRight, BottomLeft, BottomRight, Center
};

struct UiLayoutStyle2D {
	UiLayoutDirection2D direction = UiLayoutDirection2D::Column;
	UiLayoutAlign2D align = UiLayoutAlign2D::Stretch;
	// left, top, right, bottom in pixels
	glm::vec4 padding{0.0f};
	float gap = 0.0f;
	bool clipChildren = true;
};

// Lightweight immediate-mode layout. No retained widget tree. The caller
// chooses its own state/input, and draws using each item's bounds + clip.
//
// Next({0, h}) fills the cross-axis in a column; Next({w, 0}) does so
// in a row. A zero main-axis request takes remaining space. Layout is
// sequential and does not redistribute space to later siblings.
class UiLayout2D {
public:
	explicit UiLayout2D(UiRect2D viewport, UiLayoutStyle2D root = {});

	[[nodiscard]] UiLayoutItem2D Next(glm::vec2 requestedSize);
	void Push(UiLayoutItem2D container, UiLayoutStyle2D style = {});
	// Never pops the root viewport.
	[[nodiscard]] bool Pop();
	[[nodiscard]] size_t Depth() const { return m_frames.size(); }

	[[nodiscard]] static UiLayoutItem2D Anchor(
		UiLayoutItem2D parent, glm::vec2 size, UiAnchor2D anchor,
		glm::vec2 offset = glm::vec2(0.0f));

private:
	struct Frame {
		UiRect2D content{};
		UiRect2D clip{};
		UiLayoutStyle2D style{};
		float cursor = 0.0f;
	};
	std::vector<Frame> m_frames;
};

} // namespace tour
