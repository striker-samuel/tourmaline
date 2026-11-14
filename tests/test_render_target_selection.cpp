// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/render_target_selection.h>
#include <cassert>
using namespace tour::render;
int main() {
    const auto a = tour::scene::PackNodeHandle(7, 1);
    const auto recycled = tour::scene::PackNodeHandle(7, 2);
    const auto b = tour::scene::PackNodeHandle(8, 1);
    RenderSelection s;
    assert(s.Selects(a, 0));
    s.mode = RenderSelectionMode::LayerMask;
    s.layerMask = 0b10;
    assert(s.Selects(a, 0b10));
    assert(!s.Selects(a, 0b01));
    s.include = {a};
    assert(s.Selects(a, 0));
    assert(!s.Selects(recycled, 0)); // generation != generation
    s.exclude = {a};
    assert(!s.Selects(a, 0b10)); // deny wins over allow and layer
    s.mode = RenderSelectionMode::ExplicitNodes;
    assert(!s.Selects(a, 0));
    assert(!s.Selects(b, UINT32_MAX));
    s.exclude.clear();
    assert(s.Selects(a, 0));
    assert(!s.Selects(recycled, 0));
    assert(!s.Selects(tour::scene::kNodeInvalidHandle, 0));
    RenderTargetDesc d;
    assert(ValidRenderTargetDesc(d));
    d.width = 0;
    assert(!ValidRenderTargetDesc(d));
    assert(!RenderTargetHandle{});
    assert(static_cast<bool>(RenderTargetHandle{0, 1}));
}
