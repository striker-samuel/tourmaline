// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include "editor_app.h"
#include <exception>
#include <utility>

namespace editor {
void EditorApp::ApplyGamePlayRequest() {
    if (!m_gamePlayRequest)
        return;
    const bool start = *m_gamePlayRequest;
    m_gamePlayRequest.reset();
    if (!start) {
        StopGamePlay();
        return;
    }
    if (!m_world || m_gameSession.Active())
        return;
#if defined(TOURMALINE_EDITOR_GAME_LINKED)
    // Don't call standalone RunGame/onStart: that path may load cooked
    // scenes instead of using this live authored scene.
    try {
        const GameEntryContext ctx{*m_world, *this, m_projectRoot};
        if (m_gameSession.Start(::TourmalineEditorEntry(ctx)))
            Log("Linked game code started (authoring world is read-only).");
    } catch (const std::exception& e) {
        Log(std::string("Game entry failed: ") + e.what());
    }
#else
    Log("No linked game entry; see docs/editor_game_entry.md");
#endif
}
void EditorApp::StopGamePlay() {
    m_gamePlayRequest.reset();
    if (!m_gameSession.Active())
        return;
    try {
        m_gameSession.Stop();
        Log("Linked game code stopped.");
    } catch (const std::exception& e) {
        Log(std::string("Game shutdown failed: ") + e.what());
    }
}
} // namespace editor
