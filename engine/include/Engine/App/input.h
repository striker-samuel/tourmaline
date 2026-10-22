// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
//
// Backend-neutral input state: keyboard, mouse, gamepads, text input and a
// small action-binding layer.
//
// THIS HEADER MUST NOT INCLUDE A PLATFORM BACKEND. The SDL3 translation
// lives in engine/src/App/input_sdl3.cpp (Engine/App/input_sdl3.h) and
// hands Input a normalized input::Event; hosts never spell an SDL type to
// read input. Physical KEY ids are USB HID keyboard usages (the same
// numbering SDL exposes as scancodes), so the adapter can cast them after
// a range check and the engine keeps a stable, hardware-defined key ABI.
//
// Call sites spell keys either as input::Key / input::MouseButton or as the
// engine-owned TOUR_KEY_* / TOUR_MOUSE_BUTTON_* constants (same ids; see the
// alias block below). No GLFW naming survives — this engine is SDL3-only.
//
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace input {

// Physical key ids: USB HID usage page 0x07 (SDL scancode numbering).
// Only the keys the engine names are listed; any other scancode value in
// [0, KeyCount) still round-trips through the array-backed state.
enum class Key : uint16_t {
    Unknown = 0,
    A = 4, B = 5, C = 6, D = 7, E = 8, F = 9, G = 10, H = 11,
    I = 12, J = 13, K = 14, L = 15, M = 16, N = 17, O = 18, P = 19,
    Q = 20, R = 21, S = 22, T = 23, U = 24, V = 25, W = 26, X = 27,
    Y = 28, Z = 29,
    Num1 = 30, Num2 = 31, Num3 = 32, Num4 = 33, Num5 = 34,
    Num6 = 35, Num7 = 36, Num8 = 37, Num9 = 38, Num0 = 39,
    Enter = 40, Escape = 41, Backspace = 42, Tab = 43, Space = 44,
    Minus = 45, Equal = 46, LeftBracket = 47, RightBracket = 48,
    Backslash = 49, Semicolon = 51, Apostrophe = 52,
    GraveAccent = 53, Comma = 54, Period = 55, Slash = 56,
    CapsLock = 57,
    F1 = 58, F2 = 59, F3 = 60, F4 = 61, F5 = 62, F6 = 63,
    F7 = 64, F8 = 65, F9 = 66, F10 = 67, F11 = 68, F12 = 69,
    PrintScreen = 70, ScrollLock = 71, Pause = 72, Insert = 73,
    Home = 74, PageUp = 75, Delete = 76, End = 77,
    PageDown = 78, Right = 79, Left = 80, Down = 81, Up = 82,
    NumLock = 83,
    KeypadDivide = 84, KeypadMultiply = 85, KeypadSubtract = 86,
    KeypadAdd = 87, KeypadEnter = 88,
    Keypad1 = 89, Keypad2 = 90, Keypad3 = 91, Keypad4 = 92,
    Keypad5 = 93, Keypad6 = 94, Keypad7 = 95, Keypad8 = 96,
    Keypad9 = 97, Keypad0 = 98, KeypadDecimal = 99,
    KeypadEqual = 103,
    Menu = 118,
    LeftControl = 224, LeftShift = 225, LeftAlt = 226, LeftSuper = 227,
    RightControl = 228, RightShift = 229, RightAlt = 230, RightSuper = 231,
};

// USB HID scancode space (SDL_SCANCODE_COUNT). Not a key.
inline constexpr int KeyCount = 512;

// SDL mouse buttons are 1-based (LEFT=1 .. X2=5); slot 0 is unused.
enum class MouseButton : int {
    Left = 1, Middle = 3, Right = 2, X1 = 4, X2 = 5,
};
inline constexpr int MouseButtonCount = 9;

// Standard console-pad layout. Mirroring SDL's gamepad ordering keeps the
// adapter a cast (verified by static_assert in input_sdl3.cpp).
enum class GamepadButton : int {
    South = 0, East, West, North,
    Back, Guide, Start, LeftStick, RightStick,
    LeftShoulder, RightShoulder,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    Misc1, RightPaddle1, LeftPaddle1, RightPaddle2, LeftPaddle2,
    Touchpad, Misc2, Misc3, Misc4, Misc5, Misc6,
    Count
};
enum class GamepadAxis : int {
    LeftX = 0, LeftY, RightX, RightY,
    LeftTrigger, RightTrigger,
    Count
};
inline constexpr int GamepadButtonCount = static_cast<int>(GamepadButton::Count);
inline constexpr int GamepadAxisCount = static_cast<int>(GamepadAxis::Count);
using GamepadId = uint32_t;

// Modifier bits carried by key events (generic, not SDL_Keymod).
enum Modifier : uint16_t {
    ModifierNone = 0,
    ModifierShift = 1u << 0,
    ModifierCtrl = 1u << 1,
    ModifierAlt = 1u << 2,
    ModifierSuper = 1u << 3,
    ModifierCapsLock = 1u << 4,
    ModifierNumLock = 1u << 5,
};

enum class EventType : uint8_t {
    None = 0,
    Quit,
    WindowClose,
    KeyDown,
    KeyUp,
    TextInput,
    MouseButtonDown,
    MouseButtonUp,
    MouseMotion,
    MouseWheel,
    GamepadAdded,
    GamepadRemoved,
    GamepadAxisMotion,
    GamepadButtonDown,
    GamepadButtonUp,
    FileDrop,
};

// One normalized event. Deliberately a flat POD-ish struct: the adapter
// fills only the fields its EventType defines, Input consumes it, and
// hosts (App::OnInputEvent) can inspect any of them without a backend
// type. Text input and dropped paths ride `text`.
struct Event {
    EventType type = EventType::None;
    Key key = Key::Unknown;
    MouseButton mouseButton = MouseButton::Left;
    GamepadButton gamepadButton = GamepadButton::South;
    GamepadAxis gamepadAxis = GamepadAxis::LeftX;
    GamepadId gamepadId = 0;
    uint16_t mods = 0;
    float axis = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    float xrel = 0.0f;
    float yrel = 0.0f;
    bool repeat = false;
    std::string text;
};

} // namespace input

// --- Engine key aliases ---
// Short, engine-owned names for the physical key ids. These are NOT a
// backend shim (there is no GLFW in the tree): TOUR_KEY_* is simply the
// readable spelling of input::Key, kept as constant-expression ints so the
// existing int query API and the typed API accept the same names. New code
// may use input::Key::A directly; both resolve to the same id.
inline constexpr int TOUR_KEY_UNKNOWN = -1;
inline constexpr int TOUR_KEY_SPACE = static_cast<int>(input::Key::Space);
inline constexpr int TOUR_KEY_APOSTROPHE = static_cast<int>(input::Key::Apostrophe);
inline constexpr int TOUR_KEY_COMMA = static_cast<int>(input::Key::Comma);
inline constexpr int TOUR_KEY_MINUS = static_cast<int>(input::Key::Minus);
inline constexpr int TOUR_KEY_PERIOD = static_cast<int>(input::Key::Period);
inline constexpr int TOUR_KEY_SLASH = static_cast<int>(input::Key::Slash);
inline constexpr int TOUR_KEY_0 = static_cast<int>(input::Key::Num0);
inline constexpr int TOUR_KEY_1 = static_cast<int>(input::Key::Num1);
inline constexpr int TOUR_KEY_2 = static_cast<int>(input::Key::Num2);
inline constexpr int TOUR_KEY_3 = static_cast<int>(input::Key::Num3);
inline constexpr int TOUR_KEY_4 = static_cast<int>(input::Key::Num4);
inline constexpr int TOUR_KEY_5 = static_cast<int>(input::Key::Num5);
inline constexpr int TOUR_KEY_6 = static_cast<int>(input::Key::Num6);
inline constexpr int TOUR_KEY_7 = static_cast<int>(input::Key::Num7);
inline constexpr int TOUR_KEY_8 = static_cast<int>(input::Key::Num8);
inline constexpr int TOUR_KEY_9 = static_cast<int>(input::Key::Num9);
inline constexpr int TOUR_KEY_SEMICOLON = static_cast<int>(input::Key::Semicolon);
inline constexpr int TOUR_KEY_EQUAL = static_cast<int>(input::Key::Equal);
inline constexpr int TOUR_KEY_A = static_cast<int>(input::Key::A);
inline constexpr int TOUR_KEY_B = static_cast<int>(input::Key::B);
inline constexpr int TOUR_KEY_C = static_cast<int>(input::Key::C);
inline constexpr int TOUR_KEY_D = static_cast<int>(input::Key::D);
inline constexpr int TOUR_KEY_E = static_cast<int>(input::Key::E);
inline constexpr int TOUR_KEY_F = static_cast<int>(input::Key::F);
inline constexpr int TOUR_KEY_G = static_cast<int>(input::Key::G);
inline constexpr int TOUR_KEY_H = static_cast<int>(input::Key::H);
inline constexpr int TOUR_KEY_I = static_cast<int>(input::Key::I);
inline constexpr int TOUR_KEY_J = static_cast<int>(input::Key::J);
inline constexpr int TOUR_KEY_K = static_cast<int>(input::Key::K);
inline constexpr int TOUR_KEY_L = static_cast<int>(input::Key::L);
inline constexpr int TOUR_KEY_M = static_cast<int>(input::Key::M);
inline constexpr int TOUR_KEY_N = static_cast<int>(input::Key::N);
inline constexpr int TOUR_KEY_O = static_cast<int>(input::Key::O);
inline constexpr int TOUR_KEY_P = static_cast<int>(input::Key::P);
inline constexpr int TOUR_KEY_Q = static_cast<int>(input::Key::Q);
inline constexpr int TOUR_KEY_R = static_cast<int>(input::Key::R);
inline constexpr int TOUR_KEY_S = static_cast<int>(input::Key::S);
inline constexpr int TOUR_KEY_T = static_cast<int>(input::Key::T);
inline constexpr int TOUR_KEY_U = static_cast<int>(input::Key::U);
inline constexpr int TOUR_KEY_V = static_cast<int>(input::Key::V);
inline constexpr int TOUR_KEY_W = static_cast<int>(input::Key::W);
inline constexpr int TOUR_KEY_X = static_cast<int>(input::Key::X);
inline constexpr int TOUR_KEY_Y = static_cast<int>(input::Key::Y);
inline constexpr int TOUR_KEY_Z = static_cast<int>(input::Key::Z);
inline constexpr int TOUR_KEY_LEFT_BRACKET = static_cast<int>(input::Key::LeftBracket);
inline constexpr int TOUR_KEY_BACKSLASH = static_cast<int>(input::Key::Backslash);
inline constexpr int TOUR_KEY_RIGHT_BRACKET = static_cast<int>(input::Key::RightBracket);
inline constexpr int TOUR_KEY_GRAVE_ACCENT = static_cast<int>(input::Key::GraveAccent);
inline constexpr int TOUR_KEY_ESCAPE = static_cast<int>(input::Key::Escape);
inline constexpr int TOUR_KEY_ENTER = static_cast<int>(input::Key::Enter);
inline constexpr int TOUR_KEY_TAB = static_cast<int>(input::Key::Tab);
inline constexpr int TOUR_KEY_BACKSPACE = static_cast<int>(input::Key::Backspace);
inline constexpr int TOUR_KEY_INSERT = static_cast<int>(input::Key::Insert);
inline constexpr int TOUR_KEY_DELETE = static_cast<int>(input::Key::Delete);
inline constexpr int TOUR_KEY_RIGHT = static_cast<int>(input::Key::Right);
inline constexpr int TOUR_KEY_LEFT = static_cast<int>(input::Key::Left);
inline constexpr int TOUR_KEY_DOWN = static_cast<int>(input::Key::Down);
inline constexpr int TOUR_KEY_UP = static_cast<int>(input::Key::Up);
inline constexpr int TOUR_KEY_PAGE_UP = static_cast<int>(input::Key::PageUp);
inline constexpr int TOUR_KEY_PAGE_DOWN = static_cast<int>(input::Key::PageDown);
inline constexpr int TOUR_KEY_HOME = static_cast<int>(input::Key::Home);
inline constexpr int TOUR_KEY_END = static_cast<int>(input::Key::End);
inline constexpr int TOUR_KEY_CAPS_LOCK = static_cast<int>(input::Key::CapsLock);
inline constexpr int TOUR_KEY_SCROLL_LOCK = static_cast<int>(input::Key::ScrollLock);
inline constexpr int TOUR_KEY_NUM_LOCK = static_cast<int>(input::Key::NumLock);
inline constexpr int TOUR_KEY_PRINT_SCREEN = static_cast<int>(input::Key::PrintScreen);
inline constexpr int TOUR_KEY_PAUSE = static_cast<int>(input::Key::Pause);
inline constexpr int TOUR_KEY_F1 = static_cast<int>(input::Key::F1);
inline constexpr int TOUR_KEY_F2 = static_cast<int>(input::Key::F2);
inline constexpr int TOUR_KEY_F3 = static_cast<int>(input::Key::F3);
inline constexpr int TOUR_KEY_F4 = static_cast<int>(input::Key::F4);
inline constexpr int TOUR_KEY_F5 = static_cast<int>(input::Key::F5);
inline constexpr int TOUR_KEY_F6 = static_cast<int>(input::Key::F6);
inline constexpr int TOUR_KEY_F7 = static_cast<int>(input::Key::F7);
inline constexpr int TOUR_KEY_F8 = static_cast<int>(input::Key::F8);
inline constexpr int TOUR_KEY_F9 = static_cast<int>(input::Key::F9);
inline constexpr int TOUR_KEY_F10 = static_cast<int>(input::Key::F10);
inline constexpr int TOUR_KEY_F11 = static_cast<int>(input::Key::F11);
inline constexpr int TOUR_KEY_F12 = static_cast<int>(input::Key::F12);
inline constexpr int TOUR_KEY_KP_0 = static_cast<int>(input::Key::Keypad0);
inline constexpr int TOUR_KEY_KP_1 = static_cast<int>(input::Key::Keypad1);
inline constexpr int TOUR_KEY_KP_2 = static_cast<int>(input::Key::Keypad2);
inline constexpr int TOUR_KEY_KP_3 = static_cast<int>(input::Key::Keypad3);
inline constexpr int TOUR_KEY_KP_4 = static_cast<int>(input::Key::Keypad4);
inline constexpr int TOUR_KEY_KP_5 = static_cast<int>(input::Key::Keypad5);
inline constexpr int TOUR_KEY_KP_6 = static_cast<int>(input::Key::Keypad6);
inline constexpr int TOUR_KEY_KP_7 = static_cast<int>(input::Key::Keypad7);
inline constexpr int TOUR_KEY_KP_8 = static_cast<int>(input::Key::Keypad8);
inline constexpr int TOUR_KEY_KP_9 = static_cast<int>(input::Key::Keypad9);
inline constexpr int TOUR_KEY_KP_DECIMAL = static_cast<int>(input::Key::KeypadDecimal);
inline constexpr int TOUR_KEY_KP_DIVIDE = static_cast<int>(input::Key::KeypadDivide);
inline constexpr int TOUR_KEY_KP_MULTIPLY = static_cast<int>(input::Key::KeypadMultiply);
inline constexpr int TOUR_KEY_KP_SUBTRACT = static_cast<int>(input::Key::KeypadSubtract);
inline constexpr int TOUR_KEY_KP_ADD = static_cast<int>(input::Key::KeypadAdd);
inline constexpr int TOUR_KEY_KP_ENTER = static_cast<int>(input::Key::KeypadEnter);
inline constexpr int TOUR_KEY_KP_EQUAL = static_cast<int>(input::Key::KeypadEqual);
inline constexpr int TOUR_KEY_LEFT_SHIFT = static_cast<int>(input::Key::LeftShift);
inline constexpr int TOUR_KEY_LEFT_CONTROL = static_cast<int>(input::Key::LeftControl);
inline constexpr int TOUR_KEY_LEFT_ALT = static_cast<int>(input::Key::LeftAlt);
inline constexpr int TOUR_KEY_LEFT_SUPER = static_cast<int>(input::Key::LeftSuper);
inline constexpr int TOUR_KEY_RIGHT_SHIFT = static_cast<int>(input::Key::RightShift);
inline constexpr int TOUR_KEY_RIGHT_CONTROL = static_cast<int>(input::Key::RightControl);
inline constexpr int TOUR_KEY_RIGHT_ALT = static_cast<int>(input::Key::RightAlt);
inline constexpr int TOUR_KEY_RIGHT_SUPER = static_cast<int>(input::Key::RightSuper);
inline constexpr int TOUR_KEY_MENU = static_cast<int>(input::Key::Menu);
inline constexpr int TOUR_KEY_LAST = input::KeyCount;
inline constexpr int TOUR_MOUSE_BUTTON_1 = static_cast<int>(input::MouseButton::Left);
inline constexpr int TOUR_MOUSE_BUTTON_2 = static_cast<int>(input::MouseButton::Right);
inline constexpr int TOUR_MOUSE_BUTTON_3 = static_cast<int>(input::MouseButton::Middle);
inline constexpr int TOUR_MOUSE_BUTTON_4 = static_cast<int>(input::MouseButton::X1);
inline constexpr int TOUR_MOUSE_BUTTON_5 = static_cast<int>(input::MouseButton::X2);
inline constexpr int TOUR_MOUSE_BUTTON_6 = 6;
inline constexpr int TOUR_MOUSE_BUTTON_7 = 7;
inline constexpr int TOUR_MOUSE_BUTTON_8 = 8;
inline constexpr int TOUR_MOUSE_BUTTON_LAST = 8;
inline constexpr int TOUR_MOUSE_BUTTON_LEFT = TOUR_MOUSE_BUTTON_1;
inline constexpr int TOUR_MOUSE_BUTTON_RIGHT = TOUR_MOUSE_BUTTON_2;
inline constexpr int TOUR_MOUSE_BUTTON_MIDDLE = TOUR_MOUSE_BUTTON_3;

class Input {
public:
    static constexpr int KeyCount = input::KeyCount;
    static constexpr int MouseButtonCount = input::MouseButtonCount;

    enum class Action : uint8_t {
        MoveForward, MoveBack, MoveLeft, MoveRight, MoveUp, MoveDown,
        Sprint, Crouch, Jump,
        LookUp, LookDown, LookLeft, LookRight,
        UiConfirm, UiCancel,
        Count
    };

    Input() { SetDefaultBindings(); }

    void beginFrame() {
        keysPressed.fill(false);
        keysReleased.fill(false);
        mousePressed.fill(false);
        mouseReleased.fill(false);
        mouseDeltaX = 0.0;
        mouseDeltaY = 0.0;
        relMouseDeltaX = 0.0f;
        relMouseDeltaY = 0.0f;
        scrollDeltaX = 0.0;
        scrollDeltaY = 0.0;
        textInput.clear();
        // Gamepad edge states are per-frame.
        for (auto& gp : gamepads_)
            gp.beginFrame();
    }

    // --- Backend-neutral event ingestion (called by App::PollEvents with
    // events produced by the platform adapter) ---
    // Returns true when the window should close (QUIT / window-close).
    bool onEvent(const input::Event& e) {
        switch (e.type) {
        case input::EventType::Quit:
            quitRequested = true;
            return true;
        case input::EventType::WindowClose:
            quitRequested = true;
            return true;
        case input::EventType::KeyDown:
            if (!e.repeat)
                onKey(e.key, true, e.mods);
            else
                keyMods = static_cast<int>(e.mods);
            return false;
        case input::EventType::KeyUp:
            onKey(e.key, false, e.mods);
            return false;
        case input::EventType::TextInput:
            textInput += e.text;
            return false;
        case input::EventType::MouseButtonDown:
            onMouseButton(static_cast<int>(e.mouseButton), true, 0);
            return false;
        case input::EventType::MouseButtonUp:
            onMouseButton(static_cast<int>(e.mouseButton), false, 0);
            return false;
        case input::EventType::MouseMotion:
            onCursorMove(static_cast<double>(e.x), static_cast<double>(e.y),
                         e.xrel, e.yrel);
            return false;
        case input::EventType::MouseWheel:
            onScroll(static_cast<double>(e.x), static_cast<double>(e.y));
            return false;
        case input::EventType::GamepadAdded:
            addGamepad(e.gamepadId, e.text);
            return false;
        case input::EventType::GamepadRemoved:
            removeGamepad(e.gamepadId);
            return false;
        case input::EventType::GamepadAxisMotion:
            onGamepadAxis(e.gamepadId, e.gamepadAxis, e.axis);
            return false;
        case input::EventType::GamepadButtonDown:
            onGamepadButton(e.gamepadId, e.gamepadButton, true);
            return false;
        case input::EventType::GamepadButtonUp:
            onGamepadButton(e.gamepadId, e.gamepadButton, false);
            return false;
        default:
            return false;
        }
    }

    void onKey(int scancode, bool down, int mods = 0) {
        if (scancode < 0 || scancode >= KeyCount)
            return;
        if (down) {
            if (!keysDown[scancode])
                keysPressed[scancode] = true;
            keysDown[scancode] = true;
        } else {
            if (keysDown[scancode])
                keysReleased[scancode] = true;
            keysDown[scancode] = false;
        }
        keyMods = mods;
    }
    void onKey(input::Key key, bool down, uint16_t mods = 0) {
        onKey(static_cast<int>(key), down, static_cast<int>(mods));
    }

    void onMouseButton(int button, bool down, int /*mods*/) {
        if (button < 0 || button >= MouseButtonCount)
            return;
        if (down) {
            if (!mouseDown[button])
                mousePressed[button] = true;
            mouseDown[button] = true;
        } else {
            if (mouseDown[button])
                mouseReleased[button] = true;
            mouseDown[button] = false;
        }
    }

    void onCursorMove(double x, double y, float xrel = 0.0f, float yrel = 0.0f) {
        if (!hasMousePosition) {
            mouseX = x;
            mouseY = y;
            lastMouseX = x;
            lastMouseY = y;
            hasMousePosition = true;
            return;
        }
        mouseDeltaX += x - lastMouseX;
        mouseDeltaY += y - lastMouseY;
        relMouseDeltaX += xrel;
        relMouseDeltaY += yrel;
        lastMouseX = x;
        lastMouseY = y;
        mouseX = x;
        mouseY = y;
    }

    void onScroll(double xoffset, double yoffset) {
        scrollDeltaX += xoffset;
        scrollDeltaY += yoffset;
    }

    // --- Keyboard queries (physical-key ids; TOUR_KEY_* aliases work) ---
    bool isKeyHeld(int key) const { return validKey(key) && keysDown[key]; }
    bool isKeyPressed(int key) const { return validKey(key) && keysPressed[key]; }
    bool isKeyReleased(int key) const { return validKey(key) && keysReleased[key]; }
    bool isKeyHeld(input::Key key) const { return isKeyHeld(static_cast<int>(key)); }
    bool isKeyPressed(input::Key key) const { return isKeyPressed(static_cast<int>(key)); }
    bool isKeyReleased(input::Key key) const { return isKeyReleased(static_cast<int>(key)); }
    bool anyKeyPressed() const {
        for (bool b : keysPressed)
            if (b)
                return true;
        return false;
    }

    // --- Mouse queries (TOUR_MOUSE_BUTTON_* aliases work) ---
    bool isMouseButtonHeld(int button) const { return validMouseButton(button) && mouseDown[button]; }
    bool isMouseButtonPressed(int button) const { return validMouseButton(button) && mousePressed[button]; }
    bool isMouseButtonReleased(int button) const { return validMouseButton(button) && mouseReleased[button]; }
    bool isMouseButtonHeld(input::MouseButton button) const { return isMouseButtonHeld(static_cast<int>(button)); }
    bool isMouseButtonPressed(input::MouseButton button) const { return isMouseButtonPressed(static_cast<int>(button)); }
    bool isMouseButtonReleased(input::MouseButton button) const { return isMouseButtonReleased(static_cast<int>(button)); }

    double getMouseX() const { return mouseX; }
    double getMouseY() const { return mouseY; }
    double getMouseDeltaX() const { return mouseDeltaX; }
    double getMouseDeltaY() const { return mouseDeltaY; }
    // Relative-mode deltas straight from the platform motion events (FPS look).
    float getRelativeDeltaX() const { return relMouseDeltaX; }
    float getRelativeDeltaY() const { return relMouseDeltaY; }
    double getScrollDeltaX() const { return scrollDeltaX; }
    double getScrollDeltaY() const { return scrollDeltaY; }
    int getMods() const { return keyMods; }
    const std::string& getTextInput() const { return textInput; }
    bool isQuitRequested() const { return quitRequested; }
    void clearQuitRequested() { quitRequested = false; }

    // --- Gamepad ---
    struct GamepadState {
        input::GamepadId id = 0;
        std::string name; // device name at open time (for UI status)
        std::array<float, input::GamepadAxisCount> axes{};
        std::array<bool, input::GamepadButtonCount> btnDown{};
        std::array<bool, input::GamepadButtonCount> btnPressed{};
        std::array<bool, input::GamepadButtonCount> btnReleased{};
        void beginFrame() {
            btnPressed.fill(false);
            btnReleased.fill(false);
        }
    };

    int gamepadCount() const { return static_cast<int>(gamepads_.size()); }
    bool hasGamepad() const { return !gamepads_.empty(); }
    const GamepadState* gamepad(int index = 0) const {
        if (index < 0 || index >= static_cast<int>(gamepads_.size()))
            return nullptr;
        return &gamepads_[index];
    }
    // Display name of gamepad `index` ("Controller 1" fallback). Empty when none.
    std::string gamepadName(int index = 0) const {
        const GamepadState* gp = gamepad(index);
        if (!gp)
            return {};
        if (!gp->name.empty())
            return gp->name;
        return "Controller " + std::to_string(index + 1);
    }
    float gamepadAxis(int index, input::GamepadAxis axis) const {
        const GamepadState* gp = gamepad(index);
        if (!gp || axis < input::GamepadAxis::LeftX || axis >= input::GamepadAxis::Count)
            return 0.0f;
        const bool isTrigger = axis == input::GamepadAxis::LeftTrigger ||
            axis == input::GamepadAxis::RightTrigger;
        const bool isRight = axis == input::GamepadAxis::RightX ||
            axis == input::GamepadAxis::RightY;
        const float dz = isTrigger ? triggerDeadzone_
            : (isRight ? lookDeadzone_ : stickDeadzone_);
        float v = gp->axes[static_cast<int>(axis)];
        // Small dead-zone so resting sticks/triggers read exactly 0.
        if (v > -dz && v < dz)
            return 0.0f;
        return v;
    }
    // Legacy numeric overload: SDL_GamepadAxis values map 1:1 to the engine
    // enum, so external hosts that still spell backend constants keep working.
    float gamepadAxis(int index, int axis) const {
        return gamepadAxis(index, static_cast<input::GamepadAxis>(axis));
    }
    // Radial stick pair with rescaling: inside the deadzone reads (0,0);
    // the ring just outside maps near 0 and full deflection stays 1.
    // left = true: LeftX/LeftY, else RightX/RightY.
    void gamepadStick(int index, bool left, float& outX, float& outY) const {
        outX = 0.0f;
        outY = 0.0f;
        const GamepadState* gp = gamepad(index);
        if (!gp)
            return;
        const input::GamepadAxis ax = left ? input::GamepadAxis::LeftX
            : input::GamepadAxis::RightX;
        const input::GamepadAxis ay = left ? input::GamepadAxis::LeftY
            : input::GamepadAxis::RightY;
        const float dz = left ? stickDeadzone_ : lookDeadzone_;
        const float x = gp->axes[static_cast<int>(ax)];
        const float y = gp->axes[static_cast<int>(ay)];
        const float mag = std::sqrt(x * x + y * y);
        if (mag <= dz || mag <= 1e-6f)
            return;
        const float s = (mag - dz) / ((1.0f - dz) * mag);
        outX = x * s;
        outY = y * s;
    }
    // Trigger 0..1 with deadzone cut + rescale (triggers only).
    float gamepadTrigger(int index, input::GamepadAxis axis) const {
        const GamepadState* gp = gamepad(index);
        if (!gp || (axis != input::GamepadAxis::LeftTrigger &&
                    axis != input::GamepadAxis::RightTrigger))
            return 0.0f;
        float v = std::fmin(1.0f, std::fmax(0.0f,
            gp->axes[static_cast<int>(axis)]));
        if (v <= triggerDeadzone_)
            return 0.0f;
        return (v - triggerDeadzone_) / (1.0f - triggerDeadzone_);
    }
    // Per-control deadzones (radial for sticks, cut+rescale for triggers).
    void setDeadzone(float dz) { stickDeadzone_ = lookDeadzone_ = dz; } // legacy: both sticks
    void setStickDeadzone(float dz) { stickDeadzone_ = dz; }
    void setLookDeadzone(float dz) { lookDeadzone_ = dz; }
    void setTriggerDeadzone(float dz) { triggerDeadzone_ = dz; }
    float stickDeadzone() const { return stickDeadzone_; }
    float lookDeadzone() const { return lookDeadzone_; }
    float triggerDeadzone() const { return triggerDeadzone_; }
    bool gamepadButtonHeld(int index, input::GamepadButton b) const {
        const GamepadState* gp = gamepad(index);
        return gp && b >= input::GamepadButton::South && b < input::GamepadButton::Count
            && gp->btnDown[static_cast<int>(b)];
    }
    bool gamepadButtonPressed(int index, input::GamepadButton b) const {
        const GamepadState* gp = gamepad(index);
        return gp && b >= input::GamepadButton::South && b < input::GamepadButton::Count
            && gp->btnPressed[static_cast<int>(b)];
    }
    // Legacy numeric overloads (SDL_GamepadButton ordering, see above).
    bool gamepadButtonHeld(int index, int b) const {
        return gamepadButtonHeld(index, static_cast<input::GamepadButton>(b));
    }
    bool gamepadButtonPressed(int index, int b) const {
        return gamepadButtonPressed(index, static_cast<input::GamepadButton>(b));
    }

    // Combined keyboard + left-stick move vector (x = strafe right+, y = forward+).
    void moveVector(float& outX, float& outY) const {
        outX = (isKeyHeld(input::Key::D) ? 1.0f : 0.0f)
            - (isKeyHeld(input::Key::A) ? 1.0f : 0.0f);
        outY = (isKeyHeld(input::Key::W) ? 1.0f : 0.0f)
            - (isKeyHeld(input::Key::S) ? 1.0f : 0.0f);
        if (hasGamepad()) {
            outX += gamepadAxis(0, input::GamepadAxis::LeftX);
            outY -= gamepadAxis(0, input::GamepadAxis::LeftY);
        }
    }
    // Combined mouse + right-stick look deltas (pixels + stick*sensitivity).
    void lookDelta(float& outDX, float& outDY, float stickSensitivity = 600.0f) const {
        outDX = static_cast<float>(mouseDeltaX);
        outDY = static_cast<float>(mouseDeltaY);
        if (hasGamepad()) {
            outDX += gamepadAxis(0, input::GamepadAxis::RightX) * stickSensitivity / 60.0f;
            outDY += gamepadAxis(0, input::GamepadAxis::RightY) * stickSensitivity / 60.0f;
        }
    }

    // --- Actions ---
    void BindAction(Action a, int scancode) { actionKeys_[static_cast<size_t>(a)].push_back(scancode); }
    void BindAction(Action a, input::Key key) { BindAction(a, static_cast<int>(key)); }
    void ClearAction(Action a) { actionKeys_[static_cast<size_t>(a)].clear(); }
    bool IsActionHeld(Action a) const {
        for (int k : actionKeys_[static_cast<size_t>(a)])
            if (isKeyHeld(k))
                return true;
        return false;
    }
    bool IsActionPressed(Action a) const {
        for (int k : actionKeys_[static_cast<size_t>(a)])
            if (isKeyPressed(k))
                return true;
        return false;
    }
    bool IsActionReleased(Action a) const {
        for (int k : actionKeys_[static_cast<size_t>(a)])
            if (isKeyReleased(k))
                return true;
        return false;
    }

    void SetDefaultBindings() {
        for (auto& v : actionKeys_)
            v.clear();
        BindAction(Action::MoveForward, input::Key::W);
        BindAction(Action::MoveBack, input::Key::S);
        BindAction(Action::MoveLeft, input::Key::A);
        BindAction(Action::MoveRight, input::Key::D);
        BindAction(Action::MoveUp, input::Key::E);
        BindAction(Action::MoveUp, input::Key::Space);
        BindAction(Action::MoveDown, input::Key::Q);
        BindAction(Action::Sprint, input::Key::LeftShift);
        BindAction(Action::Sprint, input::Key::RightShift);
        BindAction(Action::Crouch, input::Key::LeftControl);
        BindAction(Action::UiCancel, input::Key::Escape);
        BindAction(Action::UiConfirm, input::Key::Enter);
    }

private:
    static bool validKey(int key) { return key >= 0 && key < KeyCount; }
    static bool validMouseButton(int b) { return b >= 0 && b < MouseButtonCount; }

    void addGamepad(input::GamepadId id, const std::string& name) {
        for (const auto& g : gamepads_)
            if (g.id == id)
                return;
        GamepadState st;
        st.id = id;
        st.name = name;
        gamepads_.push_back(std::move(st));
    }
    void removeGamepad(input::GamepadId id) {
        for (auto it = gamepads_.begin(); it != gamepads_.end(); ++it) {
            if (it->id == id) {
                gamepads_.erase(it);
                return;
            }
        }
    }
    void onGamepadAxis(input::GamepadId id, input::GamepadAxis axis, float value) {
        for (auto& g : gamepads_) {
            if (g.id == id && axis >= input::GamepadAxis::LeftX &&
                axis < input::GamepadAxis::Count) {
                g.axes[static_cast<int>(axis)] = value;
                return;
            }
        }
    }
    void onGamepadButton(input::GamepadId id, input::GamepadButton button, bool down) {
        for (auto& g : gamepads_) {
            if (g.id == id && button >= input::GamepadButton::South &&
                button < input::GamepadButton::Count) {
                const int b = static_cast<int>(button);
                if (down) {
                    if (!g.btnDown[b])
                        g.btnPressed[b] = true;
                    g.btnDown[b] = true;
                } else {
                    if (g.btnDown[b])
                        g.btnReleased[b] = true;
                    g.btnDown[b] = false;
                }
                return;
            }
        }
    }

    std::array<bool, KeyCount> keysDown{};
    std::array<bool, KeyCount> keysPressed{};
    std::array<bool, KeyCount> keysReleased{};

    std::array<bool, MouseButtonCount> mouseDown{};
    std::array<bool, MouseButtonCount> mousePressed{};
    std::array<bool, MouseButtonCount> mouseReleased{};

    double mouseX = 0.0, mouseY = 0.0;
    double lastMouseX = 0.0, lastMouseY = 0.0;
    double mouseDeltaX = 0.0, mouseDeltaY = 0.0;
    float relMouseDeltaX = 0.0f, relMouseDeltaY = 0.0f;
    double scrollDeltaX = 0.0, scrollDeltaY = 0.0;
    bool hasMousePosition = false;
    int keyMods = 0;
    std::string textInput;
    bool quitRequested = false;

    std::vector<GamepadState> gamepads_;
    float stickDeadzone_ = 0.12f;   // left stick (radial)
    float lookDeadzone_ = 0.20f;    // right stick (radial; drifty pads need it big)
    float triggerDeadzone_ = 0.05f;  // triggers (0..1 cut + rescale)
    std::array<std::vector<int>, static_cast<size_t>(Action::Count)> actionKeys_;
};
