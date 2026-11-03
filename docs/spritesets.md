# SpriteSet (.spriteset) authored asset

A sprite set is an independent first-class project asset with a stable `project::SpriteSetKey`. It references a separately owned texture. The spriteset stores metadata only (no duplicate pixels). Use the normal project recipe `include` patterns to include both `*.spriteset` and the referenced images in the same package, or declare the texture-owning pak as a dependency.

## Format

```text
spriteset 1
texture content/ui/characters.png
size 128 64
sprite button 0 0 32 32 pivot 0.25 0.75 border 4 5 6 7
sprite trimmed 32 0 16 14 pivot 0.5 1 trim 20 20 2 3
grid run 0 32 4 1 32 32 pivot 0.5 1
anim running 10 loop run_0 run_1 run_2 run_3
anim one_shot 4 once button trimmed
```

- Coordinate system is **pixel top-left / Y-down**; sizes and bounds refer to the actual source texture and are strictly validated.
- `pivot x y`: normalized pivot in original **untrimmed** sprite space, default `0.5 0.5`.
- `trim sourceW sourceH offsetX offsetY`: stores the uncropped dimensions and top-left crop offset; sprite `rect` remains the cropped portion of the sheet.
- `border left top right bottom`: nine-slice insets in **sprite pixels**. Trimming and nine-slice on the same sprite are rejected.
- `grid prefix x y columns rows cellW cellH` produces `prefix_0`, `prefix_1` etc., in row-major order. Optional `pivot` applies to each generated sprite.
- `anim name fps loop|once frame0 ...`: references existing sprite names; playback timing belongs to the application. `SpriteAnimation::FrameAt(seconds)` is only a sampling helper.
- Repeated names, invalid rectangles, missing animation references, invalid source paths and unknown directives are errors; the parser leaves its output unchanged on failure.

The shared CPU model is `Engine/Asset/spriteset.h`: `ParseSpriteSetText`, `SpriteSet::Find`, `FindAnimation`, `UV`. Runtime bytes can be obtained through `app::ProjectRuntime::ReadAsset(project::spritesets::NAME, err)` and decoded with `ParseSpriteSetText`; this respects provider overrides and keeps texture ownership separate.

## Rendering

`GameUiRenderer` already supports a host-owned texture and UV subranges: use `SpriteSet::UV` to populate `UiDrawRect2D::uvMin/uvMax`, and use sprite borders for app-owned nine-slice submission. `World2DRenderer` currently accepts regions from its own RGBA8-packed `SpriteAtlas2D`: zero-copy sprite-set submission is **not** part of this change and needs a separate texture-source extension. Do not conflate sprite-set stable asset keys with runtime atlas region IDs.

## Packaging

`Project.exe ids` assigns stable `0x8xxx` IDs and emits typed `project::spritesets` constants; `Pak.exe` validates and stores the authored text verbatim as `PakAssetKind::SpriteSet`. The content registry maps catalog kind 7 to pak kind 17, without renumbering existing kinds.

A `spriteset` asset is metadata and does not automatically embed its image; the recipe must include the image explicitly. The authored dimensions must match the actual source image (or KTX2) for correct UVs; image-dimension verification is a possible follow-up.