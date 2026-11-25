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
        std::cerr << "FAIL " << label << ": " << value << " != " << expected << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    const auto nearBias = smvk::ComputeCascadeReceiverBias(0.01f, 10.0f);
    Near(nearBias.normalWorld, 0.01f, "near normal = one texel");
    Near(nearBias.depthNdc, 0.0005f, "near depth = half texel / Z range");

    // Regression: fixed 3cm/1.5cm world caps used to under-bias far
    // cascades. These must grow with texel footprint, even at 500m Z.
    const auto farBias = smvk::ComputeCascadeReceiverBias(0.25f, 500.0f);
    Near(farBias.normalWorld, 0.25f, "far normal has no meter cap");
    Near(farBias.depthNdc * 500.0f, 0.125f, "far depth has no meter cap");

    const auto finer = smvk::ComputeCascadeReceiverBias(0.125f, 500.0f);
    Near(finer.normalWorld, farBias.normalWorld * 0.5f, "2x resolution halves normal offset");
    Near(finer.depthNdc, farBias.depthNdc * 0.5f, "2x resolution halves NDC depth offset");
    const auto deeper = smvk::ComputeCascadeReceiverBias(0.25f, 1000.0f);
    Near(deeper.depthNdc, farBias.depthNdc * 0.5f, "2x depth range halves NDC bias");
    Near(deeper.normalWorld, farBias.normalWorld, "Z range leaves normal offset alone");

    const auto disabled = smvk::ComputeCascadeReceiverBias(0.25f, 0.0f);
    Near(disabled.depthNdc, 0.0f, "invalid Z range disables depth bias");
    const auto zero = smvk::ComputeCascadeReceiverBias(0.0f, 100.0f);
    Near(zero.depthNdc, 0.0f, "zero texel disables depth bias");
    Near(zero.normalWorld, 0.0f, "zero texel disables normal bias");
    // A depth histogram from an occluded frame can start well behind the
    // actual camera near plane. The next frame must still cover that gap.
    constexpr float cameraNear = 0.1f;
    constexpr float measuredNear = 8.0f;
    for (uint32_t count = 1; count <= smvk::kMaxCascades; ++count) {
        for (float lambda : {0.0f, 0.8f, 1.0f}) {
            const auto adaptive = smvk::ComputeCascadeSplits(
                measuredNear, 100.0f, lambda, count);
            const auto covered = smvk::ComputeCoveredCascadeSplits(
                cameraNear, measuredNear, 100.0f, lambda, count);
            Near(covered.front(), cameraNear, "SDSM covers camera near plane");
            for (uint32_t i = 1; i <= count; ++i)
                Near(covered[i], adaptive[i], "SDSM keeps adaptive split end");

            const auto fixed = smvk::ComputeCascadeSplits(
                cameraNear, 100.0f, lambda, count);
            const auto fallback = smvk::ComputeCoveredCascadeSplits(
                cameraNear, cameraNear, 100.0f, lambda, count);
            for (uint32_t i = 0; i <= count; ++i)
                Near(fallback[i], fixed[i], "fixed CSM unchanged");
        }
    }
    return failures ? 1 : 0;
}
