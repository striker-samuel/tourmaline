# Editor content server and source hot reload

`EditorContentServer` (`editor/src/editor_content_server.h`) is the
editor-facing authoring source change boundary. It owns the project-scoped
native file watcher, its ignore policy, initial stat baseline, event routing
and overflow recovery. It is **not** the runtime `ContentRegistry`, which
resolves cooked paks and override providers.

When a project opens, the editor starts the server with the native
`ProjectDefault` policy plus editor-owned `artifacts/` and `gi/`
exclusions. Switching projects drops the old server and its pending events.
`TOURMALINE_EDITOR_WATCH=0` disables watching.

At the frame/update boundary, before command recording,
`EditorApp::ConsumeWatchedProjectChanges` drains settled, typed events.
The server never mutates the live world or imports content. The editor
dispatches to its existing selective refresh paths:

- **Mesh source (.gltf/.glb/.obj):** stale-gated import. If it is the
  currently opened scene source and a new artifact was derived,
  `ReloadReferencesViaTemp` preserves the working .tour commands,
  cameras, selection, save target and dirty state.
- **Image beside .texture:** force-import the texture under the existing
  URI and rewire live material slots without a scene restart.
- **.meta:** rescan stable source identities.
- **.tour/.tourrecipe:** report external edits but never silently replay
  over unsaved working document changes.
- **Removed source:** report removal for a later asset-db reconciliation.
- **Other source:** report only; no unsafe universal scene reload.

No full-project scanning occurs every frame. The service reconciles a
stat snapshot on startup (baseline) and after a native watcher overflow.
Generated outputs and editor temporary files are excluded from the
native event feed.

## Verification

`EditorContentServerTests` is a CPU-only test for typed routing,
de-duplication, removal semantics, lifecycle and a live source event.

Run after configuring a build with `BUILD_TESTING=ON`:

```sh
cmake --build <build-dir> --target EditorContentServerTests
ctest --test-dir <build-dir> -R editor_content_server_tests --output-on-failure
```

Editor smoke test: open a project with an active scene, modify the scene's
source mesh in an external DCC and confirm its geometry updates without
losing unsaved .tour edits; modify an image with a sibling .texture asset
and confirm the viewport updates without a relaunch; finally edit a .tour
externally and ensure the editor logs the change without overwriting the
working scene.
