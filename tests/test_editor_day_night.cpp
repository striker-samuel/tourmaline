// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Standalone smoke test: c++ -std=c++17 tests/test_editor_day_night.cpp -o /tmp/tour_sun_test && /tmp/tour_sun_test
#include "../editor/src/editor_day_night.h"
#include <cassert>
#include <cmath>

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
}
