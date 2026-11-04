# Editor day/night solar preview

Open a scene, then the **World > Environment** panel. Enable **day/night preview** to
animate the resolved viewport environment without altering its authored base values.

- **Artistic**: sunrise at 06:00, sunset at 18:00, noon altitude 75 degrees.
- **Physical**: enter latitude (north positive), longitude (east positive),
  local civil clock time, explicit UTC offset, year, month and day.
  The implementation uses NOAA's approximate fractional-year solar declination
  and equation-of-time model. The resulting azimuth is measured clockwise
  from north. Tourmaline's +Z is north and +X is east.
- **Season presets**: March/September equinox and June/December solstice
  dates, using the selected year.
- **Preview playback**: the clock advances according to full-day seconds
  while the Environment panel is drawn. Playback is not serialized.
- **Persistence**: `daynight.*` editorSet values are saved in the .tour,
  including mode, clock, duration, location, UTC offset and calendar date.
  Older scenes default to a disabled cycle.

The physical model is approximate. It ignores atmospheric refraction, horizon
occlusion, terrain, weather, and the observer's elevation. UTC offset must be
adjusted manually for daylight saving time. The season follows the date and
hemisphere; there is no separate artificial "season" variable.

**Current limitations:** This is an editor *preview* feature, not a
gameplay/runtime day/night clock. The editorSet values are not carried into
the cooked scene as a new runtime schema, and the running game does not advance
the sun. An explicit authored directional light may override the environment
sun. Sky intensity and color fade from the selected authored environment and
are heuristic, not physically based atmosphere scattering. Baked lighting does
not update when the preview clock moves.

Standalone unit test:

```sh
c++ -std=c++17 tests/test_editor_day_night.cpp -o /tmp/tour_sun_test
/tmp/tour_sun_test
```
