# Implementation status — source cross-check (2026-11-11)

This is a **source-level** comparison of documentation with public interfaces, implementation files, and CMake targets on `master`. It is **not** a full compilation, GPU runtime validation, or claim that every design note in `docs/` is implemented.

| Area | Verified implementation references | Scope / important limitation |
| --- | --- | --- |
| Content-as-code | [GameProject build](../samples/04_project_game/CMakeLists.txt), [recipe](../samples/04_project_game/project/game.tourrecipe), [generated project.h](../samples/04_project_game/project/project.h) | `GameProject_paks` cooks content explicitly; compilation alone is not a cook. Generated C++ keys and `MatchAssets` exist. |
| Runtime mesh entities | [AssetManager](../app/src/AssetManager.h), [World](../engine/include/Engine/App/world.h), [tests](../tests/test_world_entities.cpp) | Typed catalog keys identify shared assets; generational handles identify World-owned spawned instances. |
| Animation | [animator](../engine/src/App/animator.cpp), [guide](entities_and_animators.md) | Per-entity playback state exists, but the rendering path flushes only the first animator pose per target cluster. |
| Physics authoring | [scene payloads](../engine/include/Engine/Scene/scene_payloads.h), [scene tables](../engine/src/Scene/SceneTables.cpp), [editor commands](../editor/src/editor_commands.cpp) | Authoring, editing, storing, and serializing physics bodies do **not** imply runtime Jolt creation or simulation. |
| Jolt | [CMake dependency](../CMakeLists.txt), [migration record](scene_tables_migration.md) | Vendored CPU physics and isolated tests; full authored collider-to-runtime integration is incomplete. |
| Editor | [CLI](../editor/src/main.cpp), [launcher](../editor/launcher/main.cpp), [editor CMake](../editor/CMakeLists.txt) | `Editor --project <dir>` is mandatory when launched directly; the launcher owns project selection. |
| Linked editor game | [public entry](../editor/include/TourmalineEditorEntry.h), [editor callback](../editor/src/editor_game.cpp), [guide](editor_game_entry.md) | `TourmalineEditorEntry` and `GameSession` are optional, statically linked callbacks; they do not clone the world or provide a complete game-in-editor viewport. |
| Native game embedding | [root CMake](../CMakeLists.txt), [template](../templates/native_game/CMakeLists.txt) | `TOURMALINE_ENABLE_GAME_HOST` opts a subproject into host/tools without editor/sample targets. |
| C ABI | [RuntimeC.h](../app/src/RuntimeC.h), [RuntimeC.cpp](../app/src/RuntimeC.cpp), [C tests](../tests/test_runtime_c.c) | Prototype content opening/catalog validation/byte reads; no C gameplay/rendering/input/audio interface or stable ABI guarantee. |
| Package inspection | [tourinfo](../tools/tourinfo.py), [CLI manual](agent_cli_tools.md) | `tourinfo` is the newer headless query/validation CLI; the older `pak_browser.py` CLI is still available. Deep payload CRC checking is explicit. |
| Demo download | [CMake target](../CMakeLists.txt), [Linux fetch](../tools/fetch_demo_content.sh), [Windows fetch](../tools/fetch_demo_content.ps1) | The Sponza release is **content**, not prebuilt binaries. |
| Benchmark export | [viewer UI](../app/src/RenderSettingsUi.cpp), [guide](../benchmarks/README.md) | UI uses **Benchmarks** > **Export for benchmark**; Bistro scenarios need separately cooked content. |

## Revalidation requirements

Before making stronger claims, run CPU tests and CTest, exercise CLI `--help` and sample workflows, validate clean Windows/Linux builds, and smoke-test relevant GPU functionality. A design document or an implemented helper alone does not prove an integrated end-user system. This table deliberately avoids claims about all renderer subsystems, all proposals, and visual/runtime correctness.
