// UV debug view, channel 1 (F10): the lightmap-atlas UV stream (uv2 /
// TEXCOORD1, cooked by PakCooker --lightmap) as a checker + gradient +
// derivative glow. Chart-island layout, gutter bleed and seam-corner
// streaks (triangles interpolating across two atlas rects) read directly.
#define UVDBG_CHANNEL 1
#include "default_uv_debug.hlsli"
