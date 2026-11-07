// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

#include "editor_graph_canvas.h"
#include <Engine/Asset/state_machine.h>
#include <array>
#include <string>
#include <vector>

namespace editor {

// Independent document editor: no dependency on a loaded .tour, World,
// renderer, or the placement graph. The window can edit an external
// .statemachine source selected in the content browser.
class StateMachineGraphEditor {
  public:
    bool show = false;
    bool Open(const std::string& file, bool discard = false);
    void New(bool discard = false);
    bool Save();
    void Draw();
    [[nodiscard]] bool Dirty() const {
        return m_dirty;
    }
    [[nodiscard]] bool Focused() const {
        return m_focused;
    }
    [[nodiscard]] const std::string& Path() const {
        return m_path;
    }
    [[nodiscard]] const std::string& Error() const {
        return m_error;
    }

  private:
    struct Snapshot {
        tour::asset::StateMachineAsset doc;
        graphui::CanvasState canvas;
    };
    void PushUndo();
    void Undo();
    void Redo();
    void ResetHistory();
    void RemoveState(uint32_t index);
    void RemoveParameter(uint32_t index);
    void DrawParams();
    void DrawSelection();
    tour::asset::StateMachineAsset m_doc;
    graphui::CanvasState m_canvas;
    std::string m_path, m_error;
    std::array<char, 1024> m_pathField{};
    std::array<char, 96> m_newState{'i', 'd', 'l', 'e', '\0'};
    std::array<char, 96> m_newParam{'s', 'p', 'e', 'e', 'd', '\0'};
    std::array<char, 96> m_renameField{};
    int m_renameTarget = -1;
    int m_newParamKind = 2; // float
    bool m_dirty = false;
    bool m_focused = false;
    bool m_hasDoc = false;
    std::vector<Snapshot> m_undo, m_redo;
};

} // namespace editor
