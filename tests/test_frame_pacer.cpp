// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/App/FramePacer.h>

#include <cstdio>
#include <cstdlib>
#include <chrono>

namespace {
void Require(bool ok) {
    if (!ok) {
        std::fputs("frame_pacer_tests: invariant failed\n", stderr);
        std::abort();
    }
}
}

int main() {
    using Pacer = tour::FramePacer;
    const auto period = Pacer::PeriodForFps(60);
    const Pacer::TimePoint origin{};
    Require(Pacer::PeriodForFps(0) == Pacer::Duration::zero());
    Require(period >= std::chrono::milliseconds(16));
    Require(period < std::chrono::milliseconds(17));
    Require(Pacer::NextDeadline(origin, origin - period, period) == origin);
    Require(Pacer::NextDeadline(origin, origin, period) == origin + period);
    Require(Pacer::NextDeadline(origin, origin + period - Pacer::Duration{1}, period) == origin + period);
    Require(Pacer::NextDeadline(origin, origin + period, period) == origin + period * 2);
    Require(Pacer::NextDeadline(origin, origin + period * 8 + period / 2, period) == origin + period * 9);
    Require(Pacer::NextDeadline(origin, origin + period * 10, period) == origin + period * 11);
    Require(Pacer::NextDeadline(origin, origin, Pacer::Duration::zero()) == origin);

    Require(Pacer::VrrTargetFps(144.0) == 142);
    Require(Pacer::VrrTargetFps(143.86) == 142);
    Require(Pacer::VrrTargetFps(59.94) == 58);
    Require(Pacer::VrrTargetFps(240.0) == 238);
    Require(Pacer::VrrTargetFps(0.0) == 0);
    Require(Pacer::VrrTargetFps(-1.0) == 0);
    Require(Pacer::VrrTargetFps(1001.0) == 0);
    Pacer pacer;
    Require(pacer.GetTargetFps() == 0);
    pacer.SetTargetFps(144);
    Require(pacer.GetTargetFps() == 144);
    pacer.BeginFrame();
    pacer.Reset(); // reset clears a previously armed deadline
    pacer.EndFrame(); // reset means no wait
    pacer.SetTargetFps(0);
    pacer.BeginFrame();
    pacer.EndFrame(); // uncapped mode never waits
}
