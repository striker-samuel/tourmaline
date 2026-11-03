# Optional static game entry in the editor

The game implements a **statically named C++ function**. The editor does
not mandate an ECS, gameplay reflection, scripting language, or
GameBehaviour subclass. It also does not call the standalone RunGame
startup path (which can load a different, cooked scene).

## Game-defined entry

```cpp
#include <TourmalineEditorEntry.h>
#include <Engine/App/world.h>
#include <memory>

editor::GameSession TourmalineEditorEntry(
    const editor::GameEntryContext& ctx) {
    // Unsaved editor changes are visible here, read-only.
    const tour::World& source = ctx.authoredWorld;
    (void)source;
    // The game chooses how to build its own mutable simulation.
    struct State { float time = 0; };
    auto game = std::make_shared<State>();
    return {
        .onUpdate = [game](float dt) { game->time += dt; },
        .onStop = [game] { /* optional shutdown */ }
    };
}
```

## Explicit static linking

In a **top-level Tourmaline build**, after the Editor target exists:

```cmake
add_library(MyEditorGame STATIC game/editor_entry.cpp game/game.cpp)
target_link_libraries(MyEditorGame PUBLIC tourmaline::editor_entry engine)
tourmaline_editor_link_game(MyEditorGame)
```

Exactly one `TourmalineEditorEntry(const editor::GameEntryContext&)`
definition is needed; missing entries fail at link time. A generic editor
without `tourmaline_editor_link_game` builds without referencing any game
symbol. The current subproject build intentionally skips the Editor;
external game repos need a top-level build including Editor for this helper.

Use **Mode > Run linked game code** and **Stop linked game code**. Entry
runs at the synchronized update boundary; updates run every frame. Stop,
scene replacement, project changes, and shutdown invoke `onStop`.

**Scope:** This starts and ticks game-owned C++ state while letting the
game inspect the editor's current `const World&`. It is NOT a complete
in-editor game viewport: the editor does not clone the authored world,
automatically convert it to runtime entities, or render the game's
simulation. Native in-process code is not crash-isolated. The game's
scene conversion, state ownership and gameplay architecture remain
entirely up to the game. Rebuilding game C++ code relinks the editor.
