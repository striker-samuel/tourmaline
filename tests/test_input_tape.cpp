// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/App/input_tape.h>

#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#define CHECK(expression) do { \
    if (!(expression)) { \
        std::cerr << "CHECK failed: " << #expression << " at line " << __LINE__ << "\n"; \
        std::abort(); \
    } \
} while (false)

using namespace tour;
using namespace tour::input;

static Event KeyEvent(EventType type, Key key, uint16_t mods = 0) {
    Event e;
    e.type = type;
    e.key = key;
    e.mods = mods;
    return e;
}

int main() {
    std::string error;
    InputTape original;
    CHECK(original.BeginFrame(0, error));
    CHECK(original.SetDeltaTime(16'666'667, error));
    CHECK(original.Record(KeyEvent(EventType::KeyDown, Key::W), error));
    CHECK(original.Record(KeyEvent(EventType::KeyDown, Key::LeftShift,
                                    ModifierShift), error));
    Event motion;
    motion.type = EventType::MouseMotion;
    motion.x = 320.5f; motion.y = 120.0f;
    motion.xrel = -2.25f; motion.yrel = 4.5f;
    CHECK(original.Record(motion, error));
    Event gamepad;
    gamepad.type = EventType::GamepadAdded; gamepad.gamepadId = 73;
    gamepad.text = "Test pad";
    CHECK(original.Record(gamepad, error));
    gamepad.type = EventType::GamepadAxisMotion;
    gamepad.gamepadAxis = GamepadAxis::LeftX;
    gamepad.axis = -0.765625f; gamepad.text.clear();
    CHECK(original.Record(gamepad, error));
    CHECK(original.BeginFrame(1, error));
    CHECK(original.SetDeltaTime(16'666'667, error));
    Event repeat = KeyEvent(EventType::KeyDown, Key::W);
    repeat.repeat = true;
    CHECK(original.Record(repeat, error));
    Event text;
    text.type = EventType::TextInput; text.text = "hello";
    CHECK(original.Record(text, error));
    Event wheel;
    wheel.type = EventType::MouseWheel; wheel.x = 0.0f; wheel.y = -2.0f;
    CHECK(original.Record(wheel, error));
    CHECK(original.BeginFrame(2, error));
    CHECK(original.SetDeltaTime(20'000'000, error));
    CHECK(original.Record(KeyEvent(EventType::KeyUp, Key::W), error));
    CHECK(original.Record(KeyEvent(EventType::KeyUp, Key::LeftShift), error));
    gamepad.type = EventType::GamepadRemoved;
    CHECK(original.Record(gamepad, error));

    std::vector<uint8_t> bytes;
    CHECK(original.Encode(bytes, error));
    InputTape restored;
    CHECK(restored.Decode(bytes, error));
    CHECK(restored.Frames().size() == 3);
    CHECK(restored.Frame(2)->deltaTimeNs == 20'000'000);
    CHECK(restored.Frame(0)->events[2].xrel == -2.25f);
    CHECK(restored.Frame(0)->events[4].axis == -0.765625f);

    // Same inputs, including held/edge states and gamepad identity, must
    // produce identical engine Input states tick for tick.
    Input a, b;
    for (size_t tick = 0; tick < original.Frames().size(); ++tick) {
        a.beginFrame();
        b.beginFrame();
        for (const auto& e : original.Frame(tick)->events) a.onEvent(e);
        for (const auto& e : restored.Frame(tick)->events) b.onEvent(e);
        CHECK(a.isKeyHeld(Key::W) == b.isKeyHeld(Key::W));
        CHECK(a.isKeyPressed(Key::W) == b.isKeyPressed(Key::W));
        CHECK(a.isKeyReleased(Key::W) == b.isKeyReleased(Key::W));
        CHECK(a.getMods() == b.getMods());
        CHECK(a.getTextInput() == b.getTextInput());
        CHECK(a.getRelativeDeltaX() == b.getRelativeDeltaX());
        CHECK(a.getScrollDeltaY() == b.getScrollDeltaY());
        CHECK(a.gamepadCount() == b.gamepadCount());
        if (a.gamepadCount())
            CHECK(a.gamepad(0)->id == b.gamepad(0)->id);
    }
    CHECK(!a.isKeyHeld(Key::W));
    CHECK(a.isKeyReleased(Key::W));

    const auto destination = std::filesystem::temp_directory_path() /
        "tourmaline_input_tape_test.bin";
    std::error_code ec;
    std::filesystem::remove(destination, ec);
    CHECK(original.Save(destination.string(), error));
    CHECK(!original.Save(destination.string(), error)); // refuse overwrite
    InputTape fromDisk;
    CHECK(fromDisk.Load(destination.string(), error));
    CHECK(fromDisk.Frames().size() == 3);
    std::filesystem::remove(destination, ec);

    // Fail closed on corrupt, truncated, appended, or unknown-version tapes.
    auto corrupt = bytes;
    corrupt[0] = 0;
    CHECK(!restored.Decode(corrupt, error));
    corrupt = bytes;
    corrupt[8] = 2; // version
    CHECK(!restored.Decode(corrupt, error));
    corrupt = bytes;
    corrupt.pop_back();
    CHECK(!restored.Decode(corrupt, error));
    corrupt = bytes;
    corrupt.push_back(0);
    CHECK(!restored.Decode(corrupt, error));
    CHECK(restored.Frames().size() == 3); // failed decode is transactional

    InputTape invalid;
    CHECK(!invalid.BeginFrame(1, error));
    CHECK(invalid.BeginFrame(0, error));
    CHECK(!invalid.BeginFrame(2, error));
    Event bad = KeyEvent(EventType::KeyDown, Key::W);
    bad.x = std::numeric_limits<float>::quiet_NaN();
    CHECK(!invalid.Record(bad, error));
    CHECK(invalid.Record(KeyEvent(EventType::Quit, Key::W), error));
    CHECK(invalid.Frames()[0].events.empty());
    std::cout << "input_tape_tests: passed\n";
}
