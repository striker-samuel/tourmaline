# Experimental: Win32 editor host

**Status: EXPERIMENTAL — smoke prototype only; not production-ready.**

The authoring editor may become Windows-exclusive. The engine and game/App
runtime should remain cross-platform and continue using their current SDL3
host. This experiment introduces a native Win32 window/message-pump prototype,
with an optional Vulkan Win32 surface entry point, without changing Editor.exe.

## Build and run (Windows only)

Configure with the existing Windows Ninja toolchain, adding
`-DTOURMALINE_EXPERIMENTAL_WIN32_EDITOR_HOST=ON`, then run:

```powershell
cmake --build out/build/x64-Release --target EditorWin32HostSmoke
.\out\build\x64-Release\EditorWin32HostSmoke.exe
```

Use the appropriate output/build directory for your configuration. The smoke
binary opens a resizable native window with no renderer or editor UI. Close
the window to exit.

## Migration sequence

1. Keep this opt-in host separate while identifying the SDL dependencies in
   `tour::App` (window, event pump, input, audio, multi-window behavior).
2. Extract shared renderer/frame lifecycle without changing the element
   ordering, swapchain recreation boundaries, or engine host ABI inadvertently.
3. Drive Vulkan instance/surface creation through the Win32 host, including
   `VK_KHR_surface` and `VK_KHR_win32_surface`.
4. Wire ImGui Win32 platform backend + existing Vulkan renderer backend.
   Verify docked and detached platform windows before switching Editor.exe.
5. Port keyboard/mouse focus, pointer capture, raw input, DPI, IME, clipboard,
   drag/drop, file dialogs and window placement. Preserve editor shortcuts and
   the existing command/undo pipeline.
6. Switch only Editor.exe to the Win32 host after manual regression coverage.
   Do not modify the SDL3 game/App path as part of that switch.

## Not implemented in this PR

This prototype does **not** replace `EditorApp : tour::App`, create a Vulkan
instance or renderer, initialize ImGui, translate editor input, or provide
multi-viewport handling. Its `CreateVulkanSurface` helper needs an existing
Vulkan instance with the platform surface extensions enabled. The prototype
does not prove editor functionality, cross-platform build compatibility, or
successful Windows compilation (no Windows build has been run here).

The existing `Editor` target remains unchanged at runtime. This is an
incremental starting point for future migration PRs, not a claim that the
editor has already moved to Win32.

## Native UI, no Dear ImGui (additional experimental target)

`EditorWin32Native` is a standalone Win32/Common Controls + RichEdit source
authoring frontend. It uses neither SDL3 nor Dear ImGui; it is not wired into
the scene renderer, editor command stack, or engine asset database.

Configure with `-DTOURMALINE_EXPERIMENTAL_WIN32_EDITOR_HOST=ON` and build
`EditorWin32Native`. Launch it with `--project <directory>` or pick a project
directory from its **Project** menu.

Implemented in this frontend:
- Native project file tree showing source scenes and common asset formats.
- Open/new/save/save-as/reload for UTF-8 `.tour` files, including unsaved-edit prompts.
- Native RichEdit text editing and undo/redo, basic find-next, and keyboard shortcuts.
- Save via same-directory temporary file and replace-on-success.

Missing: Vulkan live viewport, visual outliner/inspector, gizmos, full scene
load/replay validation, cooking/baking integration, asset transforms, dockable
native panels, and integration with the existing editor's edit-command stack.
The existing SDL3/ImGui `Editor.exe` is **not** used as a fallback by this
new executable; it remains the separate legacy editor while this work
continues. Never describe `EditorWin32Native` as feature-complete.
