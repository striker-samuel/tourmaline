// SPDX-License-Identifier: MIT
#pragma once
// GPU-independent HUD specification; Tourmaline's GameUiRenderer records the
// rectangles into the scene's native graphics rendergraph.
#include <Engine/Renderer/GameUiLayout.h>
#include <Engine/Renderer/GameUiRenderer.h>
#include <array>
#include <cmath>
#include <glm/glm.hpp>
#include <string>
#include <string_view>

namespace starter {
using Color = glm::vec4;
inline constexpr Color Gold{0.98f, 0.76f, 0.33f, 1.0f};
inline constexpr Color Pale{0.96f, 0.94f, 0.83f, 1.0f};
inline constexpr Color Aqua{0.24f, 0.84f, 0.86f, 1.0f};
inline constexpr Color Rose{0.97f, 0.38f, 0.34f, 1.0f};

inline void rectangle(tour::GameUiRenderer& ui, tour::UiRect2D b, Color c) {
    ui.DrawRect({.bounds = b, .tint = c});
}
inline std::array<unsigned char, 7> glyph(char in) {
    // Original, font-file-free diagnostic capitals. Replace with an
    // imported font atlas for production UI.
    switch (in) {
    case 'A':
        return {14, 17, 17, 31, 17, 17, 17};
    case 'B':
        return {30, 17, 17, 30, 17, 17, 30};
    case 'C':
        return {14, 17, 16, 16, 16, 17, 14};
    case 'D':
        return {30, 17, 17, 17, 17, 17, 30};
    case 'E':
        return {31, 16, 16, 30, 16, 16, 31};
    case 'F':
        return {31, 16, 16, 30, 16, 16, 16};
    case 'G':
        return {14, 17, 16, 23, 17, 17, 15};
    case 'H':
        return {17, 17, 17, 31, 17, 17, 17};
    case 'I':
        return {14, 4, 4, 4, 4, 4, 14};
    case 'J':
        return {7, 2, 2, 2, 18, 18, 12};
    case 'K':
        return {17, 18, 20, 24, 20, 18, 17};
    case 'L':
        return {16, 16, 16, 16, 16, 16, 31};
    case 'M':
        return {17, 27, 21, 21, 17, 17, 17};
    case 'N':
        return {17, 25, 21, 19, 17, 17, 17};
    case 'O':
        return {14, 17, 17, 17, 17, 17, 14};
    case 'P':
        return {30, 17, 17, 30, 16, 16, 16};
    case 'Q':
        return {14, 17, 17, 17, 21, 18, 13};
    case 'R':
        return {30, 17, 17, 30, 20, 18, 17};
    case 'S':
        return {15, 16, 16, 14, 1, 1, 30};
    case 'T':
        return {31, 4, 4, 4, 4, 4, 4};
    case 'U':
        return {17, 17, 17, 17, 17, 17, 14};
    case 'V':
        return {17, 17, 17, 17, 17, 10, 4};
    case 'W':
        return {17, 17, 17, 21, 21, 21, 10};
    case 'X':
        return {17, 17, 10, 4, 10, 17, 17};
    case 'Y':
        return {17, 17, 10, 4, 4, 4, 4};
    case 'Z':
        return {31, 1, 2, 4, 8, 16, 31};
    case '0':
        return {14, 17, 19, 21, 25, 17, 14};
    case '1':
        return {4, 12, 4, 4, 4, 4, 14};
    case '2':
        return {14, 17, 1, 2, 4, 8, 31};
    case '3':
        return {30, 1, 1, 14, 1, 1, 30};
    case '4':
        return {2, 6, 10, 18, 31, 2, 2};
    case '5':
        return {31, 16, 16, 30, 1, 1, 30};
    case '6':
        return {14, 16, 16, 30, 17, 17, 14};
    case '7':
        return {31, 1, 2, 4, 8, 8, 8};
    case '8':
        return {14, 17, 17, 14, 17, 17, 14};
    case '9':
        return {14, 17, 17, 15, 1, 1, 14};
    case ':':
        return {0, 4, 4, 0, 4, 4, 0};
    case '+':
        return {0, 4, 4, 31, 4, 4, 0};
    case '/':
        return {1, 1, 2, 4, 8, 16, 16};
    case '.':
        return {0, 0, 0, 0, 0, 12, 12};
    case '-':
        return {0, 0, 0, 31, 0, 0, 0};
    case ' ':
        return {};
    default:
        return {31, 17, 1, 6, 4, 0, 4};
    }
}
inline void text(tour::GameUiRenderer& ui,
                 std::string_view s,
                 glm::vec2 origin,
                 float pixel = 2.0f,
                 Color c = Pale) {
    pixel = std::max(pixel, 1.0f);
    for (size_t k = 0; k < s.size(); ++k) {
        const auto bits = glyph(s[k] >= 'a' && s[k] <= 'z' ? char(s[k] - 32) : s[k]);
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 5; ++x)
                if (bits[y] & (1u << (4 - x)))
                    rectangle(ui,
                              {{origin.x + float(k) * pixel * 6 + x * pixel, origin.y + y * pixel},
                               {pixel * .89f, pixel * .89f}},
                              c);
    }
}
// Normalized layout: three clickable boxes in separate screen regions.
struct Buttons {
    tour::UiRect2D offer{}, upgrade{}, defend{};
};
inline Buttons layout(smvk::Extent2D extent) {
    const tour::UiLayoutItem2D root{{{0, 0}, {float(extent.width), float(extent.height)}},
                                    {{0, 0}, {float(extent.width), float(extent.height)}}};
    constexpr glm::vec2 size{108, 108};
    return {tour::UiLayout2D::Anchor(root, size, tour::UiAnchor2D::BottomLeft, {32, -30}).bounds,
            tour::UiLayout2D::Anchor(root, size, tour::UiAnchor2D::TopRight, {-40, 120}).bounds,
            tour::UiLayout2D::Anchor(root, size, tour::UiAnchor2D::BottomRight, {-50, -42}).bounds};
}
inline void button(tour::GameUiRenderer& ui,
                   tour::UiRect2D b,
                   Color tint,
                   std::string_view title,
                   std::string_view hotkey) {
    rectangle(ui, b, {0.05f, 0.09f, 0.13f, .95f});
    rectangle(ui, {{b.origin.x, b.origin.y}, {b.size.x, 4}}, tint);
    rectangle(ui, {{b.origin.x, b.Bottom() - 4}, {b.size.x, 4}}, tint);
    rectangle(ui, {{b.origin.x, b.origin.y}, {4, b.size.y}}, tint);
    rectangle(ui, {{b.Right() - 4, b.origin.y}, {4, b.size.y}}, tint);
    text(ui, hotkey, {b.origin.x + 44, b.origin.y + 23}, 4.3f, tint);
    text(ui, title, {b.origin.x + 10, b.origin.y + 82}, 1.85f);
}
template <typename State>
inline void draw(tour::GameUiRenderer& ui, smvk::Extent2D viewport, const State& game) {
    const auto b = layout(viewport);
    rectangle(ui, {{20, 18}, {430, 75}}, {.045f, .065f, .1f, .93f});
    text(ui, "NATIVE TOURMALINE", {40, 31}, 2.6f, Gold);
    text(ui,
         "FAVOR " + std::to_string(int(game.favor)) + "   SCORE " + std::to_string(game.score),
         {40, 68},
         1.7f,
         Aqua);
    button(ui, b.offer, Aqua, "OFFER", "1");
    button(ui, b.upgrade, Gold, "UPGRADE", "2");
    button(ui, b.defend, Rose, "DEFEND", "3");
    text(ui,
         "ONE ENGINE - ONE RENDERER",
         {float(viewport.width) * .5f - 180, float(viewport.height) - 43},
         2.0f,
         Pale);
}
} // namespace starter
