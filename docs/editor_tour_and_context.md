# Editor `.tour` Format + Editor Context — Technical Spec

Source of truth: `engine/include/Engine/Asset/tour.h`, `engine/src/Asset/tour.cpp`,
`editor/src/editor_app.h`, `editor/src/editor_commands.h`,
`editor/src/editor_app.cpp`, `editor/src/editor_cook.cpp`,
`editor/src/editor_world.cpp`.

Status: documents behavior as implemented (tour version `kTourVersion = 1`).
Forward-compat rule everywhere: **unknown verbs / attrs warn-and-skip, never abort.**

---

## 1. What a `.tour` is

* An ASCII, Maya-`.ma`-flavored **CRUD command log applied ON TOP of a source scene**.
* Never a blob store: meshes, textures stay in the source glTF + loose files;
  the tour references them by **cluster-space id / pool index / path** only.
* The interchange between two consumers that must agree:
  * **Editor** — live edits + undo (`editor/src/editor_commands.*`, `TourToCmd`/`CmdToTour`).
  * **PakCooker `--tour`** — deterministic replay onto a fresh import
    (`ApplyTourToCluster`), before meshopt/LOD/bake so pool indices match.
* WYSIWYG contract: `deterministic source import + tour replay = cooked pak`.

File order (writer-enforced, `SaveTour`, `engine/src/Asset/tour.cpp:461`):

```
1. //Name: <file> / //Last modified: <stamp>   (comments, Maya header flavor)
2. source ...                                   (optional, DEPRECATED)
3. reference ...                                (0..N, root first by convention)
4. fileInfo ...                                 (constants block)
5. fileNode ...                                 (0..N, cooked file/image decls)
6. <direct commands> + tabulated -ref children (indented with \t)
7. override ...                                 (0..N, ordered — later wins)
8. editorSet ...                                (0..N, editor-only, never cooked)
9. cook ...                                     (0..1)
```

Every statement ends with `;`. `//` starts a comment outside quotes.
`create` property lines for `-ref` children are **tab-indented** under their
create (human tabulation only — parse order is file order).

---

## 2. Lexing / syntax

* Tokenizer (`Tokenize`, `engine/src/Asset/tour.cpp:46`):
  whitespace-split, `"` grouping with `\` escapes, `//` comment outside quotes.
  Explicitly quoted `""` yields one empty token (cook empty strings round-trip).
* Trailing `;` token is stripped; a bare `;` (space before terminator) carries nothing.
* Flags: `-n -m -p -at -t -v -s -pattern -enabled -source -name -ref -uuid -type -root -fingerprint`.
  `-t`/`-s` consume values until the next flag or `;`.
* `-v` word values accept `1/on/true/yes` and `0/off/false/no` (`ParseFlagWord`).
* Numbers: `ParseU32` (from_chars), `ParseDouble` (stod). Bad ids/triples are hard
  errors; unknown verbs/attrs are warnings.

Minimal grammar (informal):

```
tour      := header? source? reference* fileInfo* fileNode* command* override* editorSet* cook?
header    := "//Name:" TEXT NL "//Last modified:" TEXT NL
command   := verb flags ";" ("//" TEXT)?
flags     := ("-n" UINT | "-m" UINT | "-p" (UINT|QUOTED) | "-at" WORD
             | "-t" VAL* | "-s" VAL* | "-v" VAL | "-ref" UINT
             | "-uuid" HEX | "-name" QUOTED | "-type" WORD
             | "-pattern" QUOTED | "-enabled" BOOL | "-source" QUOTED)*
```

---

## 3. Header blocks

### 3.1 `source` (DEPRECATED)

```
source -p "content/models/Bistro/bistro.gltf" -fingerprint "9f3a...";
```

* Written only when the session has a source/fingerprint.
* `Fingerprint(path)` = `"<size-hex>:<mtime-hex>"`, empty when missing.
* Load: warn on mismatch, apply anyway. May be empty — geometry may come
  entirely from `reference` lines (loader builds empty world, binds refs, replays).

### 3.2 `reference` — Clarisse-style nested scene refs

```
reference -root -n 0 -source "content/models/base.gltf";
reference -n 0 -source "props/chair.gltf" -name "chair";
reference -n 0 -source "shot.tour" -name "shotA" -t 1 0 3 -enabled off;
```

| Flag | Meaning |
|---|---|
| `-root` | The scene's OWN source (ref #0). Inlined under scene root, **no placeholder, no namespace**, so cluster ids match a direct import exactly. Never disabled/removed/replaced. |
| `-n` | Parent cluster id of the placeholder |
| `-source` | `.gltf` (raw geometry) or `.tour` (import its source, replay its commands with id offsets) |
| `-name` | Placeholder node name (`REF <name>`) |
| `-t x y z` | Optional placement offset |
| `-enabled off` | Unloaded (kept in list, not bound; Maya load/unload) |

Bound at import by `ApplySceneReferences` (recursive, cycle-guarded).
Referenced nodes are ordinary nodes once inlined — overrides apply per occurrence.
`strict=false` replay: a failed command inside a ref is reported + skipped,
never aborts the whole scene.

### 3.3 `fileInfo` — format constants (Maya `fileInfo` block)

```
fileInfo -k "application" -v "vulkantoy-tour";
fileInfo -k "product" -v "VulkanToy";
fileInfo -k "version" -v "1";
```

Writer injects missing `application/product/version` defaults, then emits carried pairs.

### 3.4 `fileNode` — Maya file/image declarations (COOKED)

```
fileNode -name "brick" -path "tex/brick.ktx2" -colorspace srgb -wrap repeat -filter linear;
```

* Editor-authored image definitions, promoted to cooked records.
* Cooker consumes for KTX conversion: colorspace picks transfer (sRGB vs linear),
  wrap/filter override the glTF sampler on match.
* Match order: path basename, then stem; unmatched → usage inference
  (baseColor/emissive = sRGB, else linear).
* Words and `0/1` numerics accepted. Lines without `-path` are skipped with warning.

### 3.5 `editorSet` — editor-only state (NEVER cooked)

```
editorSet -k "env.sunAzimuth" -v "30";
editorSet -k "place.graph" -v "...";
editorSet -k "path.remap.tex/a.ktx2" -v "tex/b.ktx2";
```

`ApplyTourToCluster` ignores this section. Carries: IBL env, placement graphs,
path remaps, viewport prefs. Editor IBL must never leak into paks.

### 3.6 `cook` — cook-settings interchange (0..1 line)

```
cook -outPak "" -meshopt on -lods off -lodRatios "0.6,0.3,0.12" -lodError 0.02
  -lightmap off -lmMetersPerTexel 0.25 -lmLobes 12 -lmAtlasWidth 1024 -lmRays 256
  -lmProxyRes 256 -lmExcludeSmall 0 -lmExcludeKw "" -sunAzimuth 30 -sunElevation 55
  -sunIntensity 10000 -skyIntensity 600 -cubemap off -cubemapName "" -cubemapSize 128
  -cubemapRays 128 -meshlets off -meshletVerts 64 -meshletTris 124 -meshletConeWeight 0
  -giCvol on -giWorld on -autoKtx on;
```

* Mirrors `TourCook` (`engine/include/Engine/Asset/tour.h:68`) / `CookSettings`
  (`editor/src/editor_app.h:235`).
* PakCooker consumes as defaults under `--tour`; **CLI wins on conflict**.
* Destination (`outPak`) is conventionally empty in-file; the editor fills it at cook time.
* Legacy inputs tolerated: `-lmDensity` (texels/m → m/texel), `-lmBudget` (ignored).
* Struct fields: see `TourCook` — meshopt/lods/lodRatios/lodError, lightmap +
  `lmMetersPerTexel/lmLobes/lmAtlasWidth/lmRays/lmProxyRes/lmExcludeSmall/lmExcludeKw`,
  sun/sky, cubemap stage, meshlets, `giCvol/giWorld` (GiBaker sidecars), `autoKtx`.

---

## 4. Id spaces and addressing

### 4.1 Cluster-space vs world-space

* **File ids are CLUSTER-space**: the importer builds clusters deterministically,
  so the same source yields the same ids in editor and cooker.
* **Live ids are WORLD-space** (`EditCmd.node`, `editor/src/editor_commands.h:97`).
* Translation happens **only at the save/load boundary**
  (`CmdToTour` / `TourToCmd`, `editor/src/editor_app.h:1091`):
  `WorldToCluster` / `ClusterToWorld`, `WorldMatToCluster` / `ClusterMatToWorld`
  (single-cluster scenes; `-1` = unmapped).
* Recorded node names are **verify-only**: warn on mismatch, apply by id anyway.

### 4.2 Created-node `-ref` addressing

Nodes created by the tour own no cluster id until replayed. Later verbs address
them by creation-order file index within their family:

| Base | Family | Addressing verb |
|---|---|---|
| `0x80000000` `kCreateLightRefBase` | light-family (`createLight`, `createNode pointLight/spotLight`) | `setLightAttr -ref k` |
| `0x40000000` `kCreateTerrainRefBase` | terrain-family (`createTerrain`, `createNode terrain`) | `terrainPaint -ref k` + generic verbs (`Rename/SetTranslate/.../Parent`) |
| `0x20000000` `kCreateCvolRefBase` | cvolume-family (`createCvolume`, `createNode cvolume`) | `setCvolAttr -ref k` |
| `0x10000000` `kCreateCubemapRefBase` | cubemap-family (`createCubemap`, `createNode cubemap`) | `setCubemapAttr -ref k` |

Bases are disjoint; a node carries at most one. Families span both legacy and
unified `createNode` spellings in one file-order index each.
Editor mirrors with `m_tourCreatedNodes/Cvol/Cubemap/TerrainNodes` (rebuilt every replay).

### 4.3 Symbolic parents (`-p` quoted form)

```
parent -n 45 -p "Shop_Wall";
createLight -p "a1b2...32hex" -name "Spot_1" ...;
```

* Bare number = legacy numeric id; quoted string = symbolic (`parentRef`).
* Resolution (`ResolveParentTarget`): 32-hex uuid → name → DAG path suffix.
  Unresolved = structural error (strict) / warn-skip (ref replay).
* Exists so parents survive numeric id shifts.

### 4.4 Delete addressing + Maya identity layers

```
uuid -n 45 -v a1b2c3...;
delete -uuid a1b2c3... -n 45 -name "Shop_Wall";
```

Resolution (`ResolveDeleteTarget`): **uuid exact → unique full-name → DAG-path
suffix (`grp/mesh`, any depth) → id disambiguator** (id valid + still naming the
same node) → legacy nameless id. False = warn-and-skip.

Maya parity (`tour.h:437`):

| Maya | Here |
|---|---|
| `MUuid` (persistent) | `uuidForNode`, `SetNodeUuid` (rejects dup live holder; re-set own = no-op true) |
| `MObject` (runtime) | numeric node id (live scene only, never file identity) |
| `MDagPath` (location) | `DagFullPath` / `DagShortestUniquePath` / `ResolveDagPath` |
| name+namespace (human) | `getNodeName` (namespaced for refs) |

* `TourReparent` (`tour.cpp:1729`): index-preserving, cycle-guarded, refuses root
  and `MAX_NODE_LEVEL` overflow, appends as last child, fixes subtree levels.
* Delete never renumbers, never reclaims pool resources — only LINKS drop
  (`mesh/material/light/cvol/cubemapForNode`).

---

## 5. Command verbs

`Verb` enum (`tour.h:134`), `Command` struct (`tour.h:291`), writer
(`tour.cpp:114`), parser (`tour.cpp:1161`), replay (`tour.cpp:1967`),
one-liners (`Describe`, `tour.cpp:2828`).

### 5.1 Node updates

| File verb | `Verb` | Payload |
|---|---|---|
| `rename -n 45 "Shop_Wall"` | `Rename` | `text` = new name |
| `setAttr -n 45 -at translate -type "double3" -t 1.2 3.4 5.6` | `SetTranslate` | `vec` triple |
| `setAttr -n 45 -at rotate -type "double3" -t 0 90 0` | `SetRotate` | `vec` degrees XYZ (inspector convention) |
| `setAttr -n 45 -at scale -type "double3" -t 1 1 1` | `SetScale` | `vec` triple |
| `assignMaterial -n 46 -m 12` | `AssignMaterial` | `material` = cluster pool index |
| `parent -n 45 -p 12` | `Parent` | `node2` or `parentRef` |
| `setNoLightmap -n 45 -type "bool" -v 1` | `SetNoLightmap` | `flag` → `scene.noLightmap` |
| `setDitherCloseup -n 45 -type "bool" -v 1` | `SetDitherCloseup` | `flag` → `scene.ditherCloseup` |
| `setVisibility -n 45 -type "bool" -v 1` | `SetVisibility` | `flag` = hidden self (`scene.hidden`, hierarchical effect). Legacy alias `setHidden` parses identically. |
| `uuid -n 45 -v <hex>` | `SetUuid` | `uuid` recorded for later deletes |
| `delete -uuid <hex> -n 45 -name "..."` | `Delete` | `uuid` primary, `name` secondary, `node` fallback (`UINT32_MAX` when absent) |

* Unknown `setAttr -at` warns-and-skips. Non-scalar values carry explicit `-type`
  (`double3/float4/string/bool/enum`); scalars stay bare.
* Transform replay is a **second pass**: per-node TRS triples collected
  (tour order = last channel wins), then local matrices rebuilt
  (`Rz*Ry*Rx`, degrees) over decomposed current locals.

### 5.2 Material updates

```
setMatAttr -m 12 -at baseColor -type "float4" -t 1 0.5 0.2 1;
setMatAttr -m 12 -at roughness -v 0.8;
setMatAttr -m 12 -at alphaMode -t mask;
```

`text` = attr. `vec4` for `baseColor/emissive/layers`; enum word for `alphaMode`
(`opaque|mask|blend`); `number` otherwise. Cook mapping (`ApplyMatAttr`,
`tour.cpp:1818`): diffuse/emissive(+strength on `w`)/metallic/roughness/
alphaCutoff/emissiveStrength/normalScale/occlusionStrength/doubleSided/
castsShadows/`layers` (cluster-local pool ids, `w` = active count)/
texture slots (`baseColorTex/metallicRoughTex/normalTex/occlusionTex/emissiveTex`,
`-1` clears)/`alphaMode` (drives `shadingModel` + pipeline rebuild).

### 5.3 Creates + their property verbs

Tabulated under their create in new files (`-ref k` of the k-th family create).

**Lights** — `typeFloat 1=point, 2=spot`; angles DEGREES; `flag` = static
bake-only; `node2` = shadow request:

```
createLight -p 0 -name "Spot_1" -at spot -t 1 2 3 1500 25 20 35 0 1;
	setLightAttr -ref 0 -at intensity -v 1500;
```

`setLightAttr` attrs: `color/intensity/range/inner/outer/static/shadow/name/position/rotation`.
Triples ride `-t double3`; `name` rides `-v string`; `static/shadow` ride bool;
scalars ride `-v`. `position/rotation` rebuild local TRS with one channel replaced.

**Terrain** — `vec` = group-origin corner; `vec4` = `(size[m], res, height[m], freq[1/m])`;
`number` = seed; `material` = cluster pool idx; `node2` = `tilesX | (tilesZ<<16)`:

```
createTerrain -p 0 -name "Terrain" -at terrain -t 0 0 0 64 64 8 0.02 1 1 1 -m 0;
	terrainPaint -ref 0 -at raise -t <worldX> <worldZ> <radius> <flattenTarget> -v <strength>;
```

`-t` packs `x y z size res height freq seed tilesX tilesZ` (validation ranges in
parser). Replay builds group + `tilesX*tilesZ` tile children, applies all stamps
to the global grid, then uploads tiles (dirty-set only when painted). Missing
material mints deterministic `editor://default`.

`terrainPaint` ops: `raise|lower|flatten|smooth`. `-t` = worldX worldZ radius
flattenTarget; `-v` = strength.

**Convex volumes** — `vec` = box center; `svec` = full-extents size (m);
`vec4` = `(priority, spacing[m], insideFade[m], outsideFade[m])`, axis-aligned,
rotation ignored by design:

```
createCvolume -p 0 -name "CVol_1" -t 0 1 0 0 0.5 0.1 0.1 -s 8 4 8;
	setCvolAttr -ref 0 -at spacing -v 0.5;
```

Attrs: `priority/spacing/inside/outside/position/rotation/size/name`.

**Cubemap captures** — `vec` = pinned probe; `number` = influence radius (m, `>0`):

```
createCubemap -p 0 -name "Cube_1" -t 0 1.5 0 -v 1.5;
	setCubemapAttr -ref 0 -at radius -v 2.0;
```

Attrs: `position/radius/name`. Cook `--cubemap` stage harvests as `pin` rows.

**Plain transforms** (Maya group locators; reference-displacement placeholders):

```
createTransform -p 0 -name "grp" -t 1 2 3 0 0 0 -s 1 1 1;
```

`-t` = `x y z rx ry rz` (degrees), `-s` = `sx sy sz`.

### 5.4 Unified `createNode` (new files write ONLY this)

```
createNode -at transform -p 0 -name "grp" -t 1 2 3 0 0 0 -s 1 1 1;
createNode -at spotLight -p 0 -name "Spot_1" -t 1 2 3 1500 25 20 35 0 1;
createNode -at terrain -p 0 -name "Terrain" -t 0 0 0 64 64 8 0.02 1 1 1 -m 0;
createNode -at cvolume -p 0 -name "CVol_1" -t 0 1 0 0 0.5 0.1 0.1 -s 8 4 8;
createNode -at cubemap -p 0 -name "Cube_1" -t 0 1.5 0 -v 1.5;
```

* `-at` type table (`CreateNodeTypes`, `tour.cpp:907`): DAG kinds
  (`transform/pointLight/spotLight/terrain/cvolume/cubemap` — take scene presence)
  vs system kinds (`material/file/image/camera` — parsed+validated, skipped at
  cook replay with a log; editor-managed).
* DAG types **lower to legacy verbs at parse** (single payload path downstream);
  system types stay `Verb::CreateNode`.
* Legacy aliases `point/spot` accepted for `pointLight/spotLight`.
* System types require `-name`. Unknown `-at` is a hard error.

### 5.5 Wide overrides (Clarisse-style)

```
override -pattern "Building_.*" -at visibility -v 1 -enabled on;
override -pattern "Tree.*" -at material -m 3 -enabled on;
```

* `OverrideAttr`: `visibility/material/noLightmap/ditherCloseup`
  (aliases: `hidden/hide`, `no_light_map`, `dither/dither_closeup`).
* ECMAScript regex over node names (`regex_search`); bad regex warns-and-skips
  (validated at parse AND replay).
* Precedence: **source < wide rules (file order, later wins) < per-node local edits**.
  Rules skip nodes carrying a local command for the same attr.
* Evaluated AFTER direct commands at replay/cook. `-enabled off` skips.
* Editor: rules are NOT history entries; delete/disable restores captured
  before-values only where the scene still holds the rule's value (later locals
  never clobbered). `ReapplyAllOverrides` = unapply(reverse) + apply(order).

---

## 6. Replay semantics (cook vs editor)

### Cook (`ApplyTourToCluster`, `tour.cpp:1873`)

1. Direct commands in file order (transforms deferred to second pass).
2. Terrain tile upload pass (heights final after all stamps).
3. Transform TRS second pass + `recalculateGlobalTransforms`.
4. Wide-override pass (locals win, later rules win).
5. `editorSettings` ignored. System `createNode`s warn-skip. `fileNodes` consumed
   by KTX stage (not here).

`strict=true` aborts with `*err`; `strict=false` (reference replay) warns + skips.

### Editor (`ReplayTourDoc`, `TourToCmd`, `ExecCmd`)

* Parses doc → fingerprint check → adopts cook block → rebuilds `m_history`
  (`EditCmd` stack) via `TourToCmd`, executes forward with live-id recording
  (`m_replayingTour`, `m_tourCreated*`).
* Unresolved targets (stale ref): kept OUT of history but re-emitted VERBATIM as
  `m_orphanTourCmds` — reference round-trips never erase edits.
* Save (`CmdToTour` + `SaveTourTo`): emits `commands[0..cursor]` in cluster space,
  coalesced (`CoalescedSaveIndices`), tabulated `-ref` children, then overrides,
  `editorSettings`, cook block.

---

## 7. Editor context — overall design

`EditorApp : smvk::AppImpl` (`editor/src/editor_app.h:281`). The editor never
reimplements cooking/baking — the tour log is the interchange; PakCooker replays it.

### 7.1 Context layers (what "editor context" owns)

```
AppImpl (window, smvk::Context m_context, swapchain, frame loop)
└─ EditorApp
   ├─ RHI residency: m_context (device/queues/VMA), AllocateStaticVertexFmt heap,
   │  TextureStreamer (+TexMode Streaming/Coarse/Full), bindless remap
   ├─ Scene definition: m_gltfPath/HostPath, m_world (GPU World; CPU picks
   │  read the clusters' retained uploads through the world's asset registry),
   │  m_importImages (importer data records), m_clusterBase (world<->cluster
   │  translation), m_deletedNodes, m_disabledRefRoots
   ├─ Document: m_history/m_historyCursor (cap 300), m_orphanTourCmds, m_tourPath/Dirty,
   │  autosave (%APPDATA%/tourmalineEditor/autosave/<stem>.tour) + recovery dialog
   ├─ View: m_editorCams (Persp/Top/Side), m_viewports[4] (cam+ViewMode+rect+frame slice),
   │  SecondView inset, per-(flight,viewport) frame ring
   ├─ Interaction: EditorMode LAYOUT/LIGHTING/TEST/PROFILE (modal_bindings.h registry),
   │  selection (multi + Mesh/Face/Vertex highlight + World/NonDag pseudo-selection),
   │  gizmo drag state, override rules, references, env, placement, images, path remaps
   └─ Outputs: CookToPak (temp coalesced tour + PakCooker --tour worker + GiBaker stage),
      RunInApp (App.exe --pak/--gltf), LM PNG export, Flintbase publish
```

* **RHI context** (`m_context`, from `AppImpl`): device, upload/flush, buffer/texture/
  shader creation. Editor render path (`editor_render.cpp`) is a minimal forward
  viewport (Unlit/Lit/Lit+Shadows + UV/vertex/baked/meshlet/flat-UV debug modes),
  NOT the App rendergraph — same compiled shaders, own passes (main/shadow/inset/
  gizmo lines/lightmap-export).
* **World**: `m_world` GPU world + primary-cluster texture/material/light pools.
  Structural edits preserve indices (no Resolve); `RefreshNodeDrawState` mirrors
  `ResolveAndFixup_clusters` flag rules per node.
* **Cameras/viewports**: `EditorCam` orbit frames (target/yaw/pitch/dist + persp/ortho);
  layout Single or Quad 2×2; hovered viewport is ACTIVE (input/gizmo/shadows/framing).
  `ViewMode` is per-viewport (F-keys act on active). Central rect = window minus
  left/right/bottom docks + tool tabs (`kEditorLeftW/RightW/BottomH`).

### 7.2 Scene loading flows

* `OpenAnySceneFile`: `.tour` → `OpenTourFile`; else `OpenScene` (`.pak:N` vs `.gltf`).
* `LoadGltfIntoWorld(path, tourOverride)`: import → definition build from tour doc
  (root ref + `<stem>.tour` or override) → `ReplayTourDoc` → overrides/env/refs/placement
  restore → autosave check. `m_tourPath = TourPath()` (`<stem>.tour` next to source).
* `LoadPakIntoWorld`: read-only GPU world (`LoadClusterFromPak`), no glTF picking,
  no tour; `TryLoadSiblingSglm` (`gi/<stem>.sglm`) + `TryLoadCvolumes` (`.cvmanifest`+`.cvol`).
* `OpenTourFile(path)`: load differing source if needed, replay doc into fresh stack,
  adopt cook block, file becomes save target.
* `CloseScene`: reset world + history + refs/env/placement, save prefs
  (`Documents/tourmalineEditor/editor.prefs`).

### 7.3 Document = undo stack

* `EditCmd` (`editor_commands.h:20`): every scene edit with exact inverse
  (`Rename/Translate/Rotate/Scale/AssignMaterial/SetMatAttr/SetNoLightmap/
  SetDitherCloseup/SetVisibility/Parent/CreateLight/CreateTerrain/TerrainPaint/
  SetLightAttr/Delete/CreateTransform/CreateCVolume/SetCvolAttr/CreateCubemap/
  SetCubemapAttr`). Before-state rides `before*` (+ `terrainBefore` patch,
  `delNodeMaps/delDrawItems` for deletes).
* `Do` (execute+push+autosave), `PushDone` (already-executed live drags),
  `Undo/Redo`, `CoalesceHistory` (keep last per collapsible class; creates/paints kept),
  `ClearHistory`. Inspector/gizmo drags coalesce via `PendingCmd` (`TrackDrag/FinishDrag`).
* `SaveTour[To|Incremental]` (explicit save / `<stem>.NNNN.tour`), `LoadTour`
  (replay into stack, undoable), `AutosaveTour` on every mutating path,
  `LatestTourPath` + `CheckAutosaveOffer`/`DrawRecoverDialog` (never silent).
* `CmdToTour` drops editor-created deletes via creates' live-state emission;
  cluster-backed deletes persist as `Verb::Delete` (+ preceding `uuid` lines).

### 7.4 Subsystems hanging off the context

| Area | File | Notes |
|---|---|---|
| Modal keys | `editor_modal.cpp`, `modal_bindings.h` | One registry `kModalBindings`; exact key+mods match, file order = priority; `1-4` switch modes anywhere; `ValidateModalBindings` debug-checks overlaps |
| Picking/gizmo | `editor_pick.cpp`, `editor_gizmo.cpp` | CPU brute-force ray (no accel); face/vertex are highlight-only; shift = snap; one upload/one line pass per visible viewport |
| Overrides | `editor_overrides.cpp` | Regex expand + guarded restore; `HasLocalOverride` history scan; inspector badges (local vs inherited/wide) |
| References | `editor_references.cpp` | `m_references` master list + `m_refPlaceholders`; add/remove/replace/reload/enable; `ReloadReferencesViaTemp` (snapshot temp tour, no lost edits); `DisplaceCreatedUnderRef` placeholder dance (`REFPH_*`) |
| Env (IBL, never cooked) | `editor_app.h:1224` | `EditorEnv` default + per-node map; cooked sun seeds default; `editorSet` persistence |
| Placement (never cooked) | `editor_placement*.cpp` | Scatter graphs → `PlacementProgram`; live CPU preview; `place.*` settings |
| Images/file nodes | `editor_app.h:789` | `ImageDef` (path+wrap/colorspace/filter) → cluster texture on use; `fileNode` cooked, `editorSettings` not |
| Paths | `editor_paths.cpp` | Unified source/tour/ref/URI/cook/atlas view; live relink = undoable slot reassign + `path.remap.*`; copy-fix is cook-safe |
| Cook/Run | `editor_cook.cpp` | `CookToPak`: temp coalesced tour → `PakCooker --tour` pipe worker (`CookFeed` live console) → optional `GiBaker` cvol+world stages; `RunInApp`: `App.exe --pak/--gltf` (edits need Cook first); pak scenes refuse cook |
| LM inspect/export | `editor_lmexport.cpp` | `.sglm` atlas inspect, F8/F10 views, offscreen unwrapped PNG export |
| Branding/prefs/dialogs | `editor_branding.cpp`, `editor_native_dialog.cpp` | Tourmaline gem + generated logo (chrome only); native open/save; prefs auto-save on shutdown/close |

### 7.5 UI layout (docks)

Menu bar (gem mark) → tool topbar tabs (Select/Transform/Terrain/Placement/Modes) →
left Outliner (Scene/VFS tabs) + right Inspector (node/World/material/env/cvol/cubemap/
overrides) + bottom dock (Overrides/Console) → central viewport(s) + viewport overlay
(mode legend, Play bar, recovery/save/cook/terrain dialogs). Panels are siblings —
never overlap the rendered view; splitters resize (`m_outlinerW/inspectorW/bottomDockH`).

---

## 8. Worked example

```
 //Name: bistro.tour
 //Last modified: Fri, Sep 26, 2026 09:12:04 AM
 reference -root -n 0 -source "content/models/Bistro/bistro.gltf";
 fileInfo -k "application" -v "vulkantoy-tour";
 fileInfo -k "product" -v "VulkanToy";
 fileInfo -k "version" -v "1";
 rename -n 45 "Shop_Wall"; // Shop_Wall
 setAttr -n 45 -at translate -type "double3" -t 1.2 3.4 5.6; // Shop_Wall
 assignMaterial -n 46 -m 12;
 setMatAttr -m 12 -at baseColor -type "float4" -t 1 0.5 0.2 1;
 setNoLightmap -n 45 -type "bool" -v 1;
 setVisibility -n 45 -type "bool" -v 1;
 parent -n 45 -p 12;
 uuid -n 45 -v 9f3a4b1c2d3e4f5a6b7c8d9e0f1a2b3c;
 delete -uuid 9f3a4b1c2d3e4f5a6b7c8d9e0f1a2b3c -n 45 -name "Shop_Wall";
 createLight -p 0 -name "Spot_1" -at spot -t 1 2 3 1500 25 20 35 0 1;
 	setLightAttr -ref 0 -at intensity -v 1500;
 override -pattern "Building_.*" -at visibility -v 1 -enabled on;
 editorSet -k "env.sunAzimuth" -v "30";
 cook -outPak "" -meshopt on ...;
```

---

## 9. Conformance checklist (for future edits)

* [ ] Writer keeps section order §1; `;` terminates; strings quoted (`Quote`).
* [ ] New verbs/attrs: parser warns-and-skips old files; replay `strict=false` skips per-command.
* [ ] New `createNode -at`: register in `kCreateNodeTypes` + family mapping + replay case.
* [ ] Ids in-file are cluster-space; names verify-only; parents prefer symbolic `-p`.
* [ ] Deletes carry uuid+name+id; replay drops links only.
* [ ] Overrides: locals win, later rules win, evaluated last.
* [ ] `editorSettings` never read by cook; `fileNodes`/`cook` always honored by cook.
* [ ] Cook flows through temp coalesced tour; canonical `.tour` untouched until explicit save.
