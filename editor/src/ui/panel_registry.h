// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

#include "ui/editor_context.h"
#include <imgui.h>
#include <functional>
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

    void Register(const char* title, bool& visible, DrawFn draw) {
        IM_ASSERT(title && *title);
        const bool inserted = m_names.emplace(title).second;
        IM_ASSERT(inserted && "Duplicate editor panel title");
        if (inserted)
            m_panels.push_back({title, &visible, std::move(draw)});
    }

    void DrawAll(EditorContext& context) {
        for (auto& panel : m_panels) {
            if (!*panel.visible) continue;
            if (ImGui::Begin(panel.title.c_str(), panel.visible))
                panel.draw(context);
            ImGui::End();
        }
    }

private:
    struct Panel {
        std::string title;
        bool* visible;
        DrawFn draw;
    };
    std::vector<Panel> m_panels;
    std::unordered_set<std::string> m_names;
};
} // namespace editor
