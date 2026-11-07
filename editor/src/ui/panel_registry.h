// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

#include "ui/editor_context.h"
#include <cmath>
#include <functional>
#include <imgui.h>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace editor {
// Per-frame registration of dockable panels. Owns ImGui Begin/End so
// individual panels only draw their contents and can share a context.
class PanelRegistry {
  public:
    using DrawFn = std::function<void(EditorContext&)>;
    using SettingsFn = std::function<void()>;

    void Register(const char* title, bool& visible, DrawFn draw, SettingsFn settings = {}) {
        IM_ASSERT(title && *title);
        const bool inserted = m_names.emplace(title).second;
        IM_ASSERT(inserted && "Duplicate editor panel title");
        if (inserted)
            m_panels.push_back({title, &visible, std::move(draw), std::move(settings)});
    }

    void DrawAll(EditorContext& context) {
        for (auto& panel : m_panels) {
            if (!*panel.visible)
                continue;
            if (ImGui::Begin(panel.title.c_str(), panel.visible)) {
                // Every dockable panel gets the same unobtrusive settings
                // affordance. Draw the glyph ourselves (independent of the
                // current ImGui font's Unicode icon coverage).
                ImGui::PushID(panel.title.c_str());
                const float side = ImGui::GetFrameHeight();
                const float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - side;
                ImGui::SetCursorPosX(x);
                ImGui::InvisibleButton("##panel_gear", ImVec2(side, side));
                const ImVec2 lo = ImGui::GetItemRectMin();
                const ImVec2 center(lo.x + side * 0.5f, lo.y + side * 0.5f);
                const float r = side * 0.23f;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
                dl->AddCircle(center, r, color, 12, 1.5f);
                dl->AddCircleFilled(center, r * 0.28f, color, 8);
                for (int k = 0; k < 8; ++k) {
                    const float a = float(k) * 0.78539816339f;
                    const ImVec2 u(cosf(a), sinf(a));
                    dl->AddLine(ImVec2(center.x + u.x * r, center.y + u.y * r),
                                ImVec2(center.x + u.x * (r + side * 0.12f),
                                       center.y + u.y * (r + side * 0.12f)),
                                color,
                                1.4f);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s settings", panel.title.c_str());
                if (ImGui::IsItemClicked())
                    ImGui::OpenPopup("##panel_settings");
                if (ImGui::BeginPopup("##panel_settings")) {
                    if (panel.settings) {
                        panel.settings();
                        ImGui::Separator();
                    }
                    if (ImGui::MenuItem("Close panel"))
                        *panel.visible = false;
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                if (*panel.visible) {
                    // Dense property inspectors use a slightly smaller
                    // text scale than the rest of the editor chrome.
                    // Restore the window scale before ending the panel.
                    const bool compact = panel.title == "Inspector";
                    if (compact)
                        ImGui::SetWindowFontScale(0.875f);
                    panel.draw(context);
                    if (compact)
                        ImGui::SetWindowFontScale(1.0f);
                }
            }
            ImGui::End();
        }
    }

  private:
    struct Panel {
        std::string title;
        bool* visible;
        DrawFn draw;
        SettingsFn settings;
    };
    std::vector<Panel> m_panels;
    std::unordered_set<std::string> m_names;
};
} // namespace editor
