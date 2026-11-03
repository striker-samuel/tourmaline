// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// CPU-only regression for directional CSM texel and light-depth scaling.
#include <cmath>
#include <iostream>

#include <Engine/Memory/pool.h>
#include <Engine/Rendergraph/cascadedshadowmap.h>

namespace {
int failures = 0;
void Near(float value, float expected, const char* label) {
    if (std::fabs(value - expected) <= 1e-6f * (1.0f + std::fabs(expected)))
        std::cout << "PASS " << label << '\n';
    else {
        std::cerr << "FAIL " << label << ": " << value
                  << " != " << expected << '\n';
        ++failures;
    }
}
}

int main() {
    const auto near = smvk::ComputeCascadeReceiverBias(0.01f, 10.0f);
    Near(near.normalWorld, 0.01f, "near normal = one texel");
    Near(near.depthNdc, 0.0005f, "near depth = half texel / Z range");

    // Regression: fixed 3cm/1.5cm world caps used to under-bias far
    // cascades. These must grow with texel footprint, even at 500m Z.
    const auto far = smvk::ComputeCascadeReceiverBias(0.25f, 500.0f);
    Near(far.normalWorld, 0.25f, "far normal has no meter cap");
    Near(far.depthNdc * 500.0f, 0.125f, "far depth has no meter cap");

    const auto finer = smvk::ComputeCascadeReceiverBias(0.125f, 500.0f);
    Near(finer.normalWorld, far.normalWorld * 0.5f, "2x resolution halves normal offset");
    Near(finer.depthNdc, far.depthNdc * 0.5f, "2x resolution halves NDC depth offset");
    const auto deeper = smvk::ComputeCascadeReceiverBias(0.25f, 1000.0f);
    Near(deeper.depthNdc, far.depthNdc * 0.5f, "2x depth range halves NDC bias");
    Near(deeper.normalWorld, far.normalWorld, "Z range leaves normal offset alone");

    const auto disabled = smvk::ComputeCascadeReceiverBias(0.25f, 0.0f);
    Near(disabled.depthNdc, 0.0f, "invalid Z range disables depth bias");
    const auto zero = smvk::ComputeCascadeReceiverBias(0.0f, 100.0f);
    Near(zero.depthNdc, 0.0f, "zero texel disables depth bias");
    Near(zero.normalWorld, 0.0f, "zero texel disables normal bias");
    return failures ? 1 : 0;
}
