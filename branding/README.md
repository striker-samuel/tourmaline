# Tourmaline / Crystal 01 — editable SVG logo

This pack contains a faithful **vector interpretation** of the chosen Crystal (Simplified) concept, designed for a Vulkan 3D renderer and editor.

## Files
- `tourmaline-icon.svg` — primary 512 × 512 transparent vector mark with simple, editable facets and SVG gradients.
- `tourmaline-logo-horizontal.svg` — horizontal mark + `Tourmaline` wordmark. The text is outlined as vector paths (Inter Display Bold) to avoid font dependency.
- `tourmaline-logo-stacked.svg` — vertically stacked wordmark.
- `tourmaline-logo-horizontal-dark.svg` and `tourmaline-logo-stacked-dark.svg` — white outlined wordmarks for dark UI backgrounds.
- `tourmaline-icon-simple.svg` — smaller-detail gradient icon for tiny sizes.
- `tourmaline-icon-monochrome.svg` — single-color fallback.
- `tourmaline-icon-*.png` — transparent PNG raster exports.
- `tourmaline-app.ico` — multiresolution Windows app icon.
- `tourmaline-preview.png` — presentation preview.

## Implementation notes
- All gradients, paths and facets are native SVG; **no embedded bitmap assets**.
- Teal frame and gemstone face are separate named path elements.
- Most colors are in `<linearGradient>` definitions at the top of the SVG.
- No filters, mask effects, linked images or external fonts are required for the SVGs.
- The illustration is manually rebuilt from the selected image concept rather than a literal automatic vector trace.
- All exports have transparent backgrounds except files named `app-icon-*` and the preview.

Palette: teal `#00A99B`; orange `#FF940D`; magenta `#EA306A`; violet `#360960`.
