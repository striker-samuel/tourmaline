// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// CPU-only contract test; build standalone:
// c++ -std=c++20 -Iengine/include tests/test_light_layers.cpp -o /tmp/test_light_layers
#include <Engine/Renderer/light_layers.h>

#include <cassert>
#include <cstdint>

int main() {
    using namespace smvk::lighting;
    // No-authored-mask parity: all existing lights still affect all objects.
    assert(Affects(kAllLayers, kAllLayers));
    assert(Affects(kAllLayers, kDefaultLayer));
    assert(Affects(kDefaultLayer, kAllLayers));

    // Opt-in isolation, multi-channel receivers, and high-bit support.
    assert(!Affects(Layer(0), Layer(1)));
    assert(Affects(Layer(0) | Layer(2), Layer(2)));
    assert(!Affects(Layer(0) | Layer(2), Layer(1)));
    assert(Affects(Layer(31), Layer(31)));
    assert(!Affects(Layer(31), Layer(30)));

    // Zero means intentionally unlinked, never a wildcard.
    assert(!Affects(kNoLayers, kAllLayers));
    assert(!Affects(kAllLayers, kNoLayers));
    assert(!Affects(kNoLayers, kNoLayers));
    assert(Layer(32) == kNoLayers);
    assert(Layer(999) == kNoLayers);
    return 0;
}
