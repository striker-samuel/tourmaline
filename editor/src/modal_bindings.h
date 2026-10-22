// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Central modal key registry: EVERY editor key binding lives in
// kModalBindings exactly once, with the modes it belongs to. Dispatch
// (UpdateModalInput), the viewport legend and the conflict validator
// all read this list, so a key can never be silently double-assigned
// again — LIGHTING 'V' once shadowed the global viewport toggle
// because bindings lived in five scattered per-mode tables.
//
// Adding a binding: write a tiny EditorApp handler, declare it in
// editor_app.h next to the other Modal* handlers, append one row to
// kModalBindings (mode-specific rows first, globals last — the first
// matching row wins on a shared key).

#include <cstddef>
#include <cstdint>

namespace editor {

class EditorApp;
enum class EditorMode : int;

// Scope bits (Global applies in every mode).
inline constexpr uint8_t kScopeGlobal = 1u << 0;
inline constexpr uint8_t kScopeLayout = 1u << 1;
inline constexpr uint8_t kScopeLighting = 1u << 2;
inline constexpr uint8_t kScopeTest = 1u << 3;
inline constexpr uint8_t kScopeProfile = 1u << 4;

struct ModalBindingDef {
	int key = 0;                  // TOUR_KEY_* / input::Key (see input.h)
	uint8_t mods = 0;             // kModal{Ctrl,Shift,Alt} bits
	uint8_t scopes = 0;           // kScope* bits
	void (EditorApp::*run)() = nullptr;
	const char* label = "";       // action name (legend/help)
	const char* keycap = "";      // legend key label ("V", "F8", "Shift+F")
};

size_t ModalBindingCount();
const ModalBindingDef& ModalBindingAt(size_t i);
bool ModalBindingApplies(const ModalBindingDef& b, EditorMode mode);

} // namespace editor
