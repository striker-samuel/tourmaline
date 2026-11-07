// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

#include <imgui.h>

namespace editor::ui {

// Modal popups keep ImGui's blocking/popup stack semantics, but own an
// independent platform viewport. NoAutoMerge is essential: otherwise
// Dear ImGui merges a centered modal into the main editor swapchain.
// Keep OS decorations so the result is a proper system window. The
// platform backend creates, focuses, renders and destroys the SDL OS
// window during RenderPlatformWindows().
inline void PrepareSystemModal(const ImVec2& size = ImVec2(0, 0),
                               ImGuiCond sizeCondition = ImGuiCond_Appearing) {
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGuiWindowClass modalClass{};
        modalClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
        modalClass.ViewportFlagsOverrideClear = ImGuiViewportFlags_NoDecoration;
        modalClass.ParentViewportId = ImGui::GetMainViewport()->ID;
        ImGui::SetNextWindowClass(&modalClass);
    }
    const ImGuiViewport* main = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(main->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (size.x > 0 && size.y > 0)
        ImGui::SetNextWindowSize(size, sizeCondition);
}

// The operating system draws the title bar for these decorated
// platform windows. Suppress ImGui's own one to avoid two title bars.
// ImGui keeps popup visibility itself, so native viewport close events
// do not automatically dismiss a BeginPopupModal(nullptr). Consumers
// must treat the platform close button like their Cancel/Close action.
inline bool SystemModalCloseRequested() {
    const ImGuiViewport* viewport = ImGui::GetWindowViewport();
    return viewport && viewport != ImGui::GetMainViewport() && viewport->PlatformRequestClose;
}

inline constexpr ImGuiWindowFlags kSystemModalFlags =
    ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
    ImGuiWindowFlags_NoSavedSettings;

} // namespace editor::ui
