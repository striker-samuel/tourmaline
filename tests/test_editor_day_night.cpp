// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Standalone smoke test: c++ -std=c++17 tests/test_editor_day_night.cpp -o /tmp/tour_sun_test && /tmp/tour_sun_test
#include "../editor/src/editor_day_night.h"
// NDEBUG-stripped asserts would leave this Release test vacuous - the
// whole body is assert-based (the #74 lesson: assert RESULTS is fine,
// but the macro must actually live in Release builds too).
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <limits>

int main() {
    using namespace editor::daynight;
    Settings s;
    assert(!s.enabled);
    s.enabled = true;
    s.physical = true;
    s.latitude = 0.0f;
    s.longitude = 0.0f;
    s.utcOffset = 0.0f;
    s.year = 2026;
    s.month = 3;
    s.day = 20;
    s.hour = 12.0f;
    auto equatorNoon = SolarPosition(s);
    assert(equatorNoon.elevation > 85.0f);
    s.hour = 0.0f;
    assert(SolarPosition(s).elevation < -80.0f);
    // In the northern hemisphere the June solstice sun is higher than December.
    s.latitude = 40.0f;
    s.hour = 12.0f;
    s.month = 6;
    s.day = 21;
    const float summer = SolarPosition(s).elevation;
    s.month = 12;
    s.day = 21;
    const float winter = SolarPosition(s).elevation;
    assert(summer > winter + 40.0f);
    s.latitude = 80.0f;
    s.month = 6;
    assert(SolarPosition(s).elevation > 0.0f);
    s.month = 12;
    assert(SolarPosition(s).elevation < 0.0f);
    assert(DaysInMonth(2028, 2) == 29);
    assert(DaysInMonth(2026, 2) == 28);
    s.physical = false;
    assert(std::fabs(ArtisticPosition(6.0f).elevation) < 0.01f);
    assert(ArtisticPosition(12.0f).elevation > 70.0f);
    s.hour = 23.0f;
    s.daySeconds = 24.0f;
    s.playing = true;
    Advance(s, 2.0f);
    assert(std::fabs(s.hour - 1.0f) < 0.01f);
    s.enabled = false;
    Advance(s, 5.0f);
    assert(std::fabs(s.hour - 1.0f) < 0.01f);

    // Scrubbed values stay stable when playback is paused or time runs
    // backwards; crossing midnight always wraps to [0, 24).
    s.enabled = true;
    s.hour = 6.0f;
    s.playing = false;
    Advance(s, 12.0f);
    assert(s.hour == 6.0f);
    s.playing = true;
    Advance(s, -12.0f);
    assert(s.hour == 6.0f);
    Advance(s, 12.0f);
    assert(std::fabs(s.hour - 18.0f) < 0.01f);
    s.hour = 23.0f;
    Advance(s, 2.0f);
    assert(std::fabs(s.hour - 1.0f) < 0.01f);

    // A UI-driven preview must not persist non-finite hours after corrupt
    // input or a huge frame delta; finite large deltas still wrap.
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    s.hour = 10.0f;
    Advance(s, inf);
    assert(s.hour == 10.0f);
    Advance(s, nan);
    assert(s.hour == 10.0f);
    s.daySeconds = inf;
    Advance(s, 1.0f);
    assert(s.hour == 10.0f);
    s.daySeconds = 24.0f;
    Advance(s, std::numeric_limits<float>::max());
    assert(std::isfinite(s.hour) && s.hour >= 0.0f && s.hour < 24.0f);
    s.hour = nan;
    Advance(s, 1.0f);
    assert(std::isnan(s.hour)); // invalid original state is not silently reset
}
