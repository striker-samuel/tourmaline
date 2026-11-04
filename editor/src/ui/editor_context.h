// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
#include "ui/editor_actions.h"
#include <cstdint>

namespace editor {
// Small UI-facing context. Add narrowly scoped services here as panels
// migrate; do NOT expose EditorApp or grant arbitrary scene mutation.
struct EditorContext {
    uint32_t selectedNode;
    EditorActions& actions;
};
} // namespace editor
