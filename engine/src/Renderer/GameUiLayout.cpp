// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Renderer/GameUiLayout.h>

#include <algorithm>
#include <cmath>

namespace tour {
namespace {

float NonNegative(float value) {
    return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
}

} // namespace

UiRect2D IntersectUiRects(UiRect2D a, UiRect2D b) {
    const glm::vec2 min{std::max(a.origin.x, b.origin.x), std::max(a.origin.y, b.origin.y)};
    const glm::vec2 max{std::min(a.Right(), b.Right()), std::min(a.Bottom(), b.Bottom())};
    return {min, {std::max(0.0f, max.x - min.x), std::max(0.0f, max.y - min.y)}};
}

UiLayout2D::UiLayout2D(UiRect2D viewport, UiLayoutStyle2D root) {
    Push({viewport, viewport}, root);
}

void UiLayout2D::Push(UiLayoutItem2D item, UiLayoutStyle2D style) {
    if (!m_frames.empty())
        item.clip = IntersectUiRects(m_frames.back().clip, item.clip);
    if (style.clipChildren)
        item.clip = IntersectUiRects(item.clip, item.bounds);

    const float l = NonNegative(style.padding.x);
    const float t = NonNegative(style.padding.y);
    const float r = NonNegative(style.padding.z);
    const float b = NonNegative(style.padding.w);
    Frame frame{};
    frame.style = style;
    frame.style.gap = NonNegative(style.gap);
    frame.clip = item.clip;
    frame.content.origin = item.bounds.origin + glm::vec2(l, t);
    frame.content.size = {std::max(0.0f, item.bounds.size.x - l - r),
                          std::max(0.0f, item.bounds.size.y - t - b)};
    m_frames.push_back(frame);
}

bool UiLayout2D::Pop() {
    if (m_frames.size() <= 1)
        return false;
    m_frames.pop_back();
    return true;
}

UiLayoutItem2D UiLayout2D::Next(glm::vec2 requestedSize) {
    Frame& f = m_frames.back();
    const bool row = f.style.direction == UiLayoutDirection2D::Row;
    const float mainSpan = row ? f.content.size.x : f.content.size.y;
    const float crossSpan = row ? f.content.size.y : f.content.size.x;
    const float requestedMain = row ? requestedSize.x : requestedSize.y;
    const float requestedCross = row ? requestedSize.y : requestedSize.x;
    const float remaining = std::max(0.0f, mainSpan - f.cursor);
    const float mainSize =
        requestedMain > 0.0f ? std::min(remaining, NonNegative(requestedMain)) : remaining;
    const float crossSize = f.style.align == UiLayoutAlign2D::Stretch || requestedCross <= 0.0f
                                ? crossSpan
                                : std::min(crossSpan, NonNegative(requestedCross));
    float crossOffset = 0.0f;
    if (f.style.align == UiLayoutAlign2D::Center)
        crossOffset = (crossSpan - crossSize) * 0.5f;
    else if (f.style.align == UiLayoutAlign2D::End)
        crossOffset = crossSpan - crossSize;

    UiRect2D rect{};
    if (row) {
        rect.origin = f.content.origin + glm::vec2(f.cursor, crossOffset);
        rect.size = {mainSize, crossSize};
    } else {
        rect.origin = f.content.origin + glm::vec2(crossOffset, f.cursor);
        rect.size = {crossSize, mainSize};
    }
    f.cursor += mainSize + f.style.gap;
    return {rect, f.clip};
}

UiLayoutItem2D UiLayout2D::Anchor(UiLayoutItem2D parent,
                                  glm::vec2 size,
                                  UiAnchor2D anchor,
                                  glm::vec2 offset) {
    size = {NonNegative(size.x), NonNegative(size.y)};
    glm::vec2 origin = parent.bounds.origin;
    switch (anchor) {
    case UiAnchor2D::TopLeft:
        break;
    case UiAnchor2D::TopRight:
        origin.x += parent.bounds.size.x - size.x;
        break;
    case UiAnchor2D::BottomLeft:
        origin.y += parent.bounds.size.y - size.y;
        break;
    case UiAnchor2D::BottomRight:
        origin += parent.bounds.size - size;
        break;
    case UiAnchor2D::Center:
        origin += (parent.bounds.size - size) * 0.5f;
        break;
    }
    return {{origin + offset, size}, parent.clip};
}

} // namespace tour
