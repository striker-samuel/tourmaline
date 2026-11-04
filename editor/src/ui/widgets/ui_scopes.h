// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
#include <imgui.h>

namespace editor::ui {
// Lightweight RAII guards for reusable ImGui controls.
class ScopedId {
public:
    explicit ScopedId(const char* id) { ImGui::PushID(id); }
    explicit ScopedId(int id) { ImGui::PushID(id); }
    ~ScopedId() { ImGui::PopID(); }
    ScopedId(const ScopedId&) = delete;
    ScopedId& operator=(const ScopedId&) = delete;
};

class DisabledScope {
public:
    explicit DisabledScope(bool disabled) { ImGui::BeginDisabled(disabled); }
    ~DisabledScope() { ImGui::EndDisabled(); }
    DisabledScope(const DisabledScope&) = delete;
    DisabledScope& operator=(const DisabledScope&) = delete;
};
} // namespace editor::ui
