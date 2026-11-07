// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Editor-authored solar preview, independent of the immutable scene environment.
// Azimuth convention: 0 = +Z (north), 90 = +X (east), 180 = -Z (south).
#include <algorithm>
#include <cmath>

namespace editor::daynight {

constexpr float kPi = 3.14159265358979323846f;
inline float Degrees(float radians) {
    return radians * (180.0f / kPi);
}
inline float Radians(float degrees) {
    return degrees * (kPi / 180.0f);
}
inline float Clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}
inline float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}
inline float Smooth(float a, float b, float v) {
    const float t = Clamp01((v - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}

struct Settings {
    bool enabled = false;
    bool playing = false;  // preview UI state; never serialized
    bool physical = false; // false = artistic 06:00 sunrise / 18:00 sunset
    float hour = 12.0f;    // local civil time [0, 24)
    float daySeconds = 1200.0f;
    float latitude = 40.4168f;  // degrees north
    float longitude = -3.7038f; // degrees east
    float utcOffset = 1.0f;     // hours, explicit to avoid DST/host locale
    int year = 2026;
    int month = 6;
    int day = 21;
};

struct SunPosition {
    float azimuth = 180.0f;
    float elevation = 60.0f; // signed degrees; below horizon is negative
};

inline bool LeapYear(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}
inline int DaysInMonth(int year, int month) {
    static constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    month = std::clamp(month, 1, 12);
    return days[month - 1] + ((month == 2 && LeapYear(year)) ? 1 : 0);
}
inline int DayOfYear(int year, int month, int day) {
    int result = 0;
    for (int m = 1; m < std::clamp(month, 1, 12); ++m)
        result += DaysInMonth(year, m);
    return result + std::clamp(day, 1, DaysInMonth(year, month));
}

inline SunPosition SolarPosition(const Settings& settings) {
    const float lat = Radians(std::clamp(settings.latitude, -90.0f, 90.0f));
    const float lon = std::clamp(settings.longitude, -180.0f, 180.0f);
    const int year = std::clamp(settings.year, 1, 9999);
    const int n = DayOfYear(year, settings.month, settings.day);
    const float days = LeapYear(year) ? 366.0f : 365.0f;
    const float hour = std::clamp(settings.hour, 0.0f, 24.0f);
    // NOAA fractional-year approximation: declination & equation of time.
    const float gamma = 2.0f * kPi / days * (n - 1 + (hour - 12.0f) / 24.0f);
    const float eqTime =
        229.18f * (0.000075f + 0.001868f * std::cos(gamma) - 0.032077f * std::sin(gamma) -
                   0.014615f * std::cos(2.0f * gamma) - 0.040849f * std::sin(2.0f * gamma));
    const float decl = 0.006918f - 0.399912f * std::cos(gamma) + 0.070257f * std::sin(gamma) -
                       0.006758f * std::cos(2.0f * gamma) + 0.000907f * std::sin(2.0f * gamma) -
                       0.002697f * std::cos(3.0f * gamma) + 0.00148f * std::sin(3.0f * gamma);
    const float offset = eqTime + 4.0f * lon - 60.0f * settings.utcOffset;
    float solarMinutes = std::fmod(hour * 60.0f + offset, 1440.0f);
    if (solarMinutes < 0.0f)
        solarMinutes += 1440.0f;
    const float ha = Radians(solarMinutes / 4.0f - 180.0f);
    const float sinAlt =
        std::sin(lat) * std::sin(decl) + std::cos(lat) * std::cos(decl) * std::cos(ha);
    const float altitude = std::asin(std::clamp(sinAlt, -1.0f, 1.0f));
    // East and north components of the sun vector on the local horizon.
    const float east = -std::cos(decl) * std::sin(ha);
    const float north =
        std::cos(lat) * std::sin(decl) - std::sin(lat) * std::cos(decl) * std::cos(ha);
    float azimuth = Degrees(std::atan2(east, north));
    if (azimuth < 0.0f)
        azimuth += 360.0f;
    return {azimuth, Degrees(altitude)};
}

inline SunPosition ArtisticPosition(float hour) {
    const float phase = 2.0f * kPi * (hour - 6.0f) / 24.0f;
    float azimuth = std::fmod(90.0f + (hour - 6.0f) * 15.0f + 360.0f, 360.0f);
    return {azimuth, 75.0f * std::sin(phase)};
}

inline void Advance(Settings& settings, float deltaSeconds) {
    // Scrubbing/preview can receive invalid editor inputs. Never poison the
    // stored hour with NaN, and don't let a large frame pause overflow the
    // intermediate float multiplication before applying the daily wrap.
    if (!settings.enabled || !settings.playing || !std::isfinite(settings.hour) ||
        !std::isfinite(settings.daySeconds) || !std::isfinite(deltaSeconds) ||
        settings.daySeconds <= 0.0f || deltaSeconds <= 0.0f)
        return;
    const double elapsedHours = std::fmod(
        static_cast<double>(deltaSeconds) * 24.0 / static_cast<double>(settings.daySeconds), 24.0);
    double hour = std::fmod(static_cast<double>(settings.hour) + elapsedHours, 24.0);
    if (hour < 0.0)
        hour += 24.0;
    settings.hour = static_cast<float>(hour);
}

} // namespace editor::daynight
