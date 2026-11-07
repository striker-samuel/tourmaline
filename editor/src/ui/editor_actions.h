// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

#include "editor_commands.h"
#include <cstdint>
#include <functional>
#include <glm/mat4x4.hpp>
#include <utility>

namespace editor {
// Thin UI-facing adapter around the existing Do/TrackDrag/FinishDrag
// command pipeline. Widgets must not directly mutate scene state.
struct EditorActions {
    using ExecuteFn = std::function<void(const EditCmd&)>;
    using TrackFn = std::function<bool(EditVerb, uint32_t, const glm::mat4&, bool)>;
    using FinishFn = std::function<void()>;

    ExecuteFn execute;
    TrackFn trackDrag;
    FinishFn finishDrag;

    void Execute(const EditCmd& cmd) const {
        execute(cmd);
    }
    bool TrackDrag(EditVerb verb, uint32_t node, const glm::mat4& before, bool activated) const {
        return trackDrag(verb, node, before, activated);
    }
    void FinishDrag() const {
        finishDrag();
    }
};
} // namespace editor
