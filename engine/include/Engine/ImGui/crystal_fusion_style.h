// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <imgui.h>

namespace tour::branding {

// Crystal Fusion: neutral dark stone surfaces, with accents taken directly
// from branding/tourmaline-icon.svg (teal #00A99B, magenta #EA306A,
// orange #FF940D). Keep all panel backgrounds opaque over the 3D viewport.
inline void ApplyCrystalFusionImGuiStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s); // Reset unlisted colors instead of inheriting an old theme.
    s.WindowRounding = 3.0f;
    s.ChildRounding = 3.0f;
    s.FrameRounding = 3.0f;
    s.PopupRounding = 3.0f;
    s.ScrollbarRounding = 4.0f;
    s.GrabRounding = 3.0f;
    s.TabRounding = 3.0f;
    s.WindowPadding = ImVec2(8, 8);
    s.FramePadding = ImVec2(7, 4);
    s.ItemSpacing = ImVec2(7, 5);
    s.ItemInnerSpacing = ImVec2(5, 4);
    s.ScrollbarSize = 11.0f;
    s.GrabMinSize = 10.0f;

    // Hex values are the source of truth: avoid hand-rounded floats that
    // drift away from the logo or the documented stone-gray palette.
    const auto rgb = [](unsigned int hex, float alpha = 1.0f) {
        return ImVec4(float((hex >> 16) & 0xFF) / 255.0f,
                      float((hex >> 8) & 0xFF) / 255.0f,
                      float(hex & 0xFF) / 255.0f,
                      alpha);
    };
    const ImVec4 teal = rgb(0x00A99B);
    const ImVec4 magenta = rgb(0xEA306A);
    const ImVec4 orange = rgb(0xFF940D);
    ImVec4* c = s.Colors;

    // Stone-gray base (no blue cast).
    c[ImGuiCol_Text] = rgb(0xE7E8EA);
    c[ImGuiCol_TextDisabled] = rgb(0x989BA1);
    c[ImGuiCol_WindowBg] = rgb(0x18191C);
    c[ImGuiCol_ChildBg] = rgb(0x222428);
    c[ImGuiCol_PopupBg] = rgb(0x222428);
    c[ImGuiCol_MenuBarBg] = rgb(0x222428);
    c[ImGuiCol_Border] = rgb(0x41454A);
    c[ImGuiCol_BorderShadow] = rgb(0x000000, 0.0f);
    c[ImGuiCol_FrameBg] = rgb(0x2D3034);
    c[ImGuiCol_FrameBgHovered] = rgb(0x3A3E42);
    c[ImGuiCol_FrameBgActive] = rgb(0x44484D);
    c[ImGuiCol_TitleBg] = rgb(0x202226);
    c[ImGuiCol_TitleBgActive] = rgb(0x2B2E32);
    c[ImGuiCol_TitleBgCollapsed] = rgb(0x202226);
    c[ImGuiCol_ScrollbarBg] = rgb(0x18191C);
    c[ImGuiCol_ScrollbarGrab] = rgb(0x41454A);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(0x555A60);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(0x686D73);
    c[ImGuiCol_Separator] = rgb(0x41454A);
    c[ImGuiCol_Tab] = rgb(0x292C30);
    c[ImGuiCol_TabUnfocused] = rgb(0x222428);
    c[ImGuiCol_TabUnfocusedActive] = rgb(0x383B40);
    c[ImGuiCol_TableHeaderBg] = rgb(0x2B2E32);
    c[ImGuiCol_TableBorderStrong] = rgb(0x41454A);
    c[ImGuiCol_TableBorderLight] = rgb(0x34373B);
    c[ImGuiCol_TableRowBg] = rgb(0x222428, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = rgb(0xFFFFFF, 0.035f);

    // Teal: primary interactions, selection, keyboard focus and drag targets.
    c[ImGuiCol_Button] = rgb(0x176B64);
    c[ImGuiCol_ButtonHovered] = teal;
    c[ImGuiCol_ButtonActive] = rgb(0x007E74);
    c[ImGuiCol_CheckMark] = teal;
    c[ImGuiCol_Header] = rgb(0x176B64, 0.60f);
    c[ImGuiCol_HeaderHovered] = rgb(0x008D82, 0.85f);
    c[ImGuiCol_HeaderActive] = rgb(0x007E74);
    c[ImGuiCol_NavHighlight] = teal;
    c[ImGuiCol_DragDropTarget] = teal;
    c[ImGuiCol_TextSelectedBg] = rgb(0x00A99B, 0.33f);
    c[ImGuiCol_ResizeGrip] = rgb(0x00A99B, 0.20f);
    c[ImGuiCol_ResizeGripHovered] = rgb(0x00A99B, 0.65f);
    c[ImGuiCol_ResizeGripActive] = teal;
    c[ImGuiCol_DockingPreview] = rgb(0x00A99B, 0.65f);

    // Magenta: active tab/slider selection. Orange: action highlights.
    c[ImGuiCol_TabHovered] = rgb(0xA92357);
    c[ImGuiCol_TabSelected] = magenta;
    c[ImGuiCol_TabSelectedOverline] = magenta;
    c[ImGuiCol_SliderGrab] = magenta;
    c[ImGuiCol_SliderGrabActive] = rgb(0xFF5687);
    c[ImGuiCol_SeparatorHovered] = rgb(0xFF940D, 0.75f);
    c[ImGuiCol_SeparatorActive] = orange;
    c[ImGuiCol_PlotLines] = teal;
    c[ImGuiCol_PlotLinesHovered] = orange;
    c[ImGuiCol_PlotHistogram] = magenta;
    c[ImGuiCol_PlotHistogramHovered] = orange;
    c[ImGuiCol_NavWindowingHighlight] = rgb(0x00A99B, 0.70f);
    c[ImGuiCol_NavWindowingDimBg] = rgb(0x18191C, 0.40f);
    c[ImGuiCol_ModalWindowDimBg] = rgb(0x18191C, 0.65f);
}

} // namespace tour::branding
