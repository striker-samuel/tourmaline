// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include "editor_brush_presets.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <sstream>

int main() {
    using namespace editor::brush;
    Preset original;
    original.kind = Kind::Scatter;
    original.name = "Sparse Rocks";
    original.radius = 7.0f;
    original.strength = 0.25f;
    original.spacing = 0.3f;
    original.density = 0.4f;
    original.minScale = 0.6f;
    original.maxScale = 2.0f;
    original.minDistance = 3.0f;
    std::stringstream text;
    Write(text, original);
    Preset parsed;
    assert(Read(text, parsed));
    assert(parsed.kind == Kind::Scatter);
    assert(parsed.name == original.name);
    assert(std::fabs(parsed.radius - 7.0f) < 1e-5f);
    assert(std::fabs(parsed.minDistance - 3.0f) < 1e-5f);
    assert(std::fabs(parsed.density - 0.4f) < 1e-5f);
    // Invalid assets must not partially apply settings.
    Preset unchanged;
    unchanged.name = "untouched";
    std::stringstream invalid("version=2\nkind=scatter\nname=invalid\n");
    assert(!Read(invalid, unchanged));
    assert(unchanged.name == "untouched");
    std::stringstream unknown("version=1\nkind=terrain\nnmae=typo\n");
    assert(!Read(unknown, unchanged));
    std::stringstream outOfRange("version=1\nkind=terrain\nradius=-1\n");
    assert(!Read(outOfRange, unchanged));
    std::stringstream nan("version=1\nkind=terrain\nstrength=nan\n");
    assert(!Read(nan, unchanged));
    Preset terrain;
    terrain.kind = Kind::Terrain;
    terrain.name = "Detail Sculpt";
    terrain.operation = 3;
    std::stringstream terrainText;
    Write(terrainText, terrain);
    Preset terrainRead;
    assert(Read(terrainText, terrainRead));
    assert(terrainRead.operation == 3);
    return 0;
}
