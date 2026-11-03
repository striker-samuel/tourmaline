// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Optional game-authored entry, invoked by the statically linked Editor.
// No required ECS, scripting runtime, or GameBehaviour inheritance.
#include <functional>
#include <string_view>
#include <utility>

namespace tour {
class App;
struct World;
}
namespace editor {

// Authored state, including unsaved edits, is borrowed READ-ONLY.
// The game owns any mutable gameplay state it chooses to construct.
struct GameEntryContext {
    const tour::World& authoredWorld;
    tour::App& host;
    std::string_view projectRoot; // copy if retained
};

struct GameSession {
    std::function<void(float)> onUpdate;
    std::function<void()> onStop;
};

// Lifecycle is CPU-only and independently testable.
class GameSessionRunner {
public:
    bool Start(GameSession session) {
        if (m_active) return false;
        m_session = std::move(session);
        m_active = true;
        return true;
    }
    void Tick(float dt) {
        if (m_active && m_session.onUpdate) m_session.onUpdate(dt);
    }
    void Stop() {
        if (!m_active) return;
        m_active = false;
        GameSession previous = std::move(m_session);
        m_session = {};
        if (previous.onStop) previous.onStop();
    }
    [[nodiscard]] bool Active() const { return m_active; }

private:
    bool m_active = false;
    GameSession m_session;
};
} // namespace editor

// The game supplies EXACTLY this named C++ symbol when opted in.
//
// Example (MyGame and its methods are defined by the game, not Tourmaline):
//
//   editor::GameSession TourmalineEditorEntry(
//       const editor::GameEntryContext& ctx)
//   {
//       auto game = std::make_shared<MyGame>();
//
//       // The GAME decides how to interpret editor content.
//       game->initializeFromEditor(
//           ctx.authoredWorld,
//           std::string(ctx.projectRoot)
//       );
//
//       return {
//           .onUpdate = [game](float dt) {
//               game->update(dt);
//           },
//           .onStop = [game] {
//               game->shutdown();
//           }
//       };
//   }
//
// The implementation file needs <memory> and <string> for this example.
editor::GameSession TourmalineEditorEntry(
    const editor::GameEntryContext& context);
