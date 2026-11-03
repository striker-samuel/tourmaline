# Pixel-art license and atlas layout

`sprites.png` and `font.png` are newly created, original assets for this
Asteroids sample. They do not contain images, glyph designs, or textures from
Sponza or from a third-party font. To the extent copyright or related rights
exist, the author dedicates these images to the public domain under
**CC0 1.0 Universal**: https://creativecommons.org/publicdomain/zero/1.0/

No attribution is required for reuse. The C++ and HLSL source code remains
subject to the repository's source-file license headers.

## sprites.png — RGBA8, 256 × 256

An 8-column × 8-row atlas of **32 × 32** cells, numbered left-to-right,
top-to-bottom, starting at 0:

- 0–15: 16 ship heading frames (starting right, increasing 22.5°)
- 16–31: corresponding ship frames with thrusters
- 32–39: eight large asteroid designs
- 40–47: eight medium asteroid designs
- 48–55: eight small asteroid designs
- 56: projectile; 57: impact spark
- 58–63: spare starburst effect frames

There is transparent padding within each cell. The sample uses nearest
filtering and an inset UV region, with one instanced UI rectangle per sprite.

## font.png — RGBA8, 128 × 60

A bespoke uppercase **5×7** bitmap face on an **8×10** cell grid, 16 columns
and six rows. Cell 0 maps to ASCII 32 (space); cells 0–95 map to ASCII 32–127.
All pixels are white or transparent, so the material tint controls color.

The sample submits **one textured rectangle per non-space glyph** with the
correct UV subrect, rather than 35 tiny rectangles per glyph.

Each atlas also has a first-class **SpriteSet** descriptor (`sprites.spriteset` and `font.spriteset`) with a grid, named animation clips and matching declared texture path. These authored metadata files and original pixel images are also dedicated under CC0 1.0. The descriptor is parsed through `Engine/Asset/spriteset.h` from PR #55. SpriteSet owns metadata but **not the texture**: the sample uploads the PNG via `ResourceManager` and supplies `SpriteSet::UV` to `GameUiRenderer`. All four files are shipped beside the executable at
`content/asteroids/`. You can edit these PNGs in any pixel
editor; keep dimensions, cell order, and alpha transparency unchanged.

## Music and effects

The sample also includes `generate_audio.py`, which synthesizes an original 8-bar chiptune at 120 BPM. Its emitted `audio/music.mid`, `audio/music.wav`, `audio/laser.wav`, `audio/hit.wav`, `audio/wave.wav`, and `audio/gameover.wav` are dedicated under **CC0 1.0 Universal** to the extent any such rights arise. The Python source retains its MIT header. The generated files are reproducible during the CMake build and are not copied from commercial game music or external soundfonts.
