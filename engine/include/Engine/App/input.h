// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
//
// SDL3 input system: keyboard (scancodes), mouse, gamepads, touch-lite,
// text input and a small action-binding layer.
//
// Back-compat: old code used GLFW_KEY_* / GLFW_MOUSE_BUTTON_* constants.
// Those are re-provided here as aliases to SDL scancodes/buttons so existing
// call sites (m_input.isKeyPressed(GLFW_KEY_X)) keep compiling unchanged.
//
#include <SDL3/SDL.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// --- GLFW compat constants (only when GLFW headers are absent) ---
#ifndef GLFW_KEY_UNKNOWN
#define GLFW_KEY_UNKNOWN -1
#define GLFW_KEY_SPACE SDL_SCANCODE_SPACE
#define GLFW_KEY_APOSTROPHE SDL_SCANCODE_APOSTROPHE
#define GLFW_KEY_COMMA SDL_SCANCODE_COMMA
#define GLFW_KEY_MINUS SDL_SCANCODE_MINUS
#define GLFW_KEY_PERIOD SDL_SCANCODE_PERIOD
#define GLFW_KEY_SLASH SDL_SCANCODE_SLASH
#define GLFW_KEY_0 SDL_SCANCODE_0
#define GLFW_KEY_1 SDL_SCANCODE_1
#define GLFW_KEY_2 SDL_SCANCODE_2
#define GLFW_KEY_3 SDL_SCANCODE_3
#define GLFW_KEY_4 SDL_SCANCODE_4
#define GLFW_KEY_5 SDL_SCANCODE_5
#define GLFW_KEY_6 SDL_SCANCODE_6
#define GLFW_KEY_7 SDL_SCANCODE_7
#define GLFW_KEY_8 SDL_SCANCODE_8
#define GLFW_KEY_9 SDL_SCANCODE_9
#define GLFW_KEY_SEMICOLON SDL_SCANCODE_SEMICOLON
#define GLFW_KEY_EQUAL SDL_SCANCODE_EQUALS
#define GLFW_KEY_A SDL_SCANCODE_A
#define GLFW_KEY_B SDL_SCANCODE_B
#define GLFW_KEY_C SDL_SCANCODE_C
#define GLFW_KEY_D SDL_SCANCODE_D
#define GLFW_KEY_E SDL_SCANCODE_E
#define GLFW_KEY_F SDL_SCANCODE_F
#define GLFW_KEY_G SDL_SCANCODE_G
#define GLFW_KEY_H SDL_SCANCODE_H
#define GLFW_KEY_I SDL_SCANCODE_I
#define GLFW_KEY_J SDL_SCANCODE_J
#define GLFW_KEY_K SDL_SCANCODE_K
#define GLFW_KEY_L SDL_SCANCODE_L
#define GLFW_KEY_M SDL_SCANCODE_M
#define GLFW_KEY_N SDL_SCANCODE_N
#define GLFW_KEY_O SDL_SCANCODE_O
#define GLFW_KEY_P SDL_SCANCODE_P
#define GLFW_KEY_Q SDL_SCANCODE_Q
#define GLFW_KEY_R SDL_SCANCODE_R
#define GLFW_KEY_S SDL_SCANCODE_S
#define GLFW_KEY_T SDL_SCANCODE_T
#define GLFW_KEY_U SDL_SCANCODE_U
#define GLFW_KEY_V SDL_SCANCODE_V
#define GLFW_KEY_W SDL_SCANCODE_W
#define GLFW_KEY_X SDL_SCANCODE_X
#define GLFW_KEY_Y SDL_SCANCODE_Y
#define GLFW_KEY_Z SDL_SCANCODE_Z
#define GLFW_KEY_LEFT_BRACKET SDL_SCANCODE_LEFTBRACKET
#define GLFW_KEY_BACKSLASH SDL_SCANCODE_BACKSLASH
#define GLFW_KEY_RIGHT_BRACKET SDL_SCANCODE_RIGHTBRACKET
#define GLFW_KEY_GRAVE_ACCENT SDL_SCANCODE_GRAVE
#define GLFW_KEY_ESCAPE SDL_SCANCODE_ESCAPE
#define GLFW_KEY_ENTER SDL_SCANCODE_RETURN
#define GLFW_KEY_TAB SDL_SCANCODE_TAB
#define GLFW_KEY_BACKSPACE SDL_SCANCODE_BACKSPACE
#define GLFW_KEY_INSERT SDL_SCANCODE_INSERT
#define GLFW_KEY_DELETE SDL_SCANCODE_DELETE
#define GLFW_KEY_RIGHT SDL_SCANCODE_RIGHT
#define GLFW_KEY_LEFT SDL_SCANCODE_LEFT
#define GLFW_KEY_DOWN SDL_SCANCODE_DOWN
#define GLFW_KEY_UP SDL_SCANCODE_UP
#define GLFW_KEY_PAGE_UP SDL_SCANCODE_PAGEUP
#define GLFW_KEY_PAGE_DOWN SDL_SCANCODE_PAGEDOWN
#define GLFW_KEY_HOME SDL_SCANCODE_HOME
#define GLFW_KEY_END SDL_SCANCODE_END
#define GLFW_KEY_CAPS_LOCK SDL_SCANCODE_CAPSLOCK
#define GLFW_KEY_SCROLL_LOCK SDL_SCANCODE_SCROLLLOCK
#define GLFW_KEY_NUM_LOCK SDL_SCANCODE_NUMLOCKCLEAR
#define GLFW_KEY_PRINT_SCREEN SDL_SCANCODE_PRINTSCREEN
#define GLFW_KEY_PAUSE SDL_SCANCODE_PAUSE
#define GLFW_KEY_F1 SDL_SCANCODE_F1
#define GLFW_KEY_F2 SDL_SCANCODE_F2
#define GLFW_KEY_F3 SDL_SCANCODE_F3
#define GLFW_KEY_F4 SDL_SCANCODE_F4
#define GLFW_KEY_F5 SDL_SCANCODE_F5
#define GLFW_KEY_F6 SDL_SCANCODE_F6
#define GLFW_KEY_F7 SDL_SCANCODE_F7
#define GLFW_KEY_F8 SDL_SCANCODE_F8
#define GLFW_KEY_F9 SDL_SCANCODE_F9
#define GLFW_KEY_F10 SDL_SCANCODE_F10
#define GLFW_KEY_F11 SDL_SCANCODE_F11
#define GLFW_KEY_F12 SDL_SCANCODE_F12
#define GLFW_KEY_KP_0 SDL_SCANCODE_KP_0
#define GLFW_KEY_KP_1 SDL_SCANCODE_KP_1
#define GLFW_KEY_KP_2 SDL_SCANCODE_KP_2
#define GLFW_KEY_KP_3 SDL_SCANCODE_KP_3
#define GLFW_KEY_KP_4 SDL_SCANCODE_KP_4
#define GLFW_KEY_KP_5 SDL_SCANCODE_KP_5
#define GLFW_KEY_KP_6 SDL_SCANCODE_KP_6
#define GLFW_KEY_KP_7 SDL_SCANCODE_KP_7
#define GLFW_KEY_KP_8 SDL_SCANCODE_KP_8
#define GLFW_KEY_KP_9 SDL_SCANCODE_KP_9
#define GLFW_KEY_KP_DECIMAL SDL_SCANCODE_KP_PERIOD
#define GLFW_KEY_KP_DIVIDE SDL_SCANCODE_KP_DIVIDE
#define GLFW_KEY_KP_MULTIPLY SDL_SCANCODE_KP_MULTIPLY
#define GLFW_KEY_KP_SUBTRACT SDL_SCANCODE_KP_MINUS
#define GLFW_KEY_KP_ADD SDL_SCANCODE_KP_PLUS
#define GLFW_KEY_KP_ENTER SDL_SCANCODE_KP_ENTER
#define GLFW_KEY_KP_EQUAL SDL_SCANCODE_KP_EQUALS
#define GLFW_KEY_LEFT_SHIFT SDL_SCANCODE_LSHIFT
#define GLFW_KEY_LEFT_CONTROL SDL_SCANCODE_LCTRL
#define GLFW_KEY_LEFT_ALT SDL_SCANCODE_LALT
#define GLFW_KEY_LEFT_SUPER SDL_SCANCODE_LGUI
#define GLFW_KEY_RIGHT_SHIFT SDL_SCANCODE_RSHIFT
#define GLFW_KEY_RIGHT_CONTROL SDL_SCANCODE_RCTRL
#define GLFW_KEY_RIGHT_ALT SDL_SCANCODE_RALT
#define GLFW_KEY_RIGHT_SUPER SDL_SCANCODE_RGUI
#define GLFW_KEY_MENU SDL_SCANCODE_MENU
#define GLFW_KEY_LAST SDL_SCANCODE_COUNT
#define GLFW_MOUSE_BUTTON_1 SDL_BUTTON_LEFT
#define GLFW_MOUSE_BUTTON_2 SDL_BUTTON_RIGHT
#define GLFW_MOUSE_BUTTON_3 SDL_BUTTON_MIDDLE
#define GLFW_MOUSE_BUTTON_4 SDL_BUTTON_X1
#define GLFW_MOUSE_BUTTON_5 SDL_BUTTON_X2
#define GLFW_MOUSE_BUTTON_6 6
#define GLFW_MOUSE_BUTTON_7 7
#define GLFW_MOUSE_BUTTON_8 8
#define GLFW_MOUSE_BUTTON_LAST 8
#define GLFW_MOUSE_BUTTON_LEFT GLFW_MOUSE_BUTTON_1
#define GLFW_MOUSE_BUTTON_RIGHT GLFW_MOUSE_BUTTON_2
#define GLFW_MOUSE_BUTTON_MIDDLE GLFW_MOUSE_BUTTON_3
#endif

class Input {
public:
    static constexpr int KeyCount = SDL_SCANCODE_COUNT;
    // SDL mouse buttons are 1-based (LEFT=1 .. X2=5); keep slot 0 unused.
    static constexpr int MouseButtonCount = 9;

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

    // --- SDL event ingestion (called by App::PollEvents) ---
    // Returns true when the window should close (QUIT / window-close).
    bool onSDLEvent(const SDL_Event& e) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            quitRequested = true;
            return true;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            quitRequested = true;
            return true;
        case SDL_EVENT_KEY_DOWN:
            if (!e.key.repeat)
                onKey(static_cast<int>(e.key.scancode), true, e.key.mod);
            else
                keyMods = static_cast<int>(e.key.mod);
            return false;
        case SDL_EVENT_KEY_UP:
            onKey(static_cast<int>(e.key.scancode), false, e.key.mod);
            return false;
        case SDL_EVENT_TEXT_INPUT:
            textInput += e.text.text;
            return false;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            onMouseButton(static_cast<int>(e.button.button), true, 0);
            return false;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            onMouseButton(static_cast<int>(e.button.button), false, 0);
            return false;
        case SDL_EVENT_MOUSE_MOTION:
            onCursorMove(static_cast<double>(e.motion.x), static_cast<double>(e.motion.y),
                         e.motion.xrel, e.motion.yrel);
            return false;
        case SDL_EVENT_MOUSE_WHEEL:
            onScroll(static_cast<double>(e.wheel.x), static_cast<double>(e.wheel.y));
            return false;
        case SDL_EVENT_GAMEPAD_ADDED:
            openGamepad(e.gdevice.which);
            return false;
        case SDL_EVENT_GAMEPAD_REMOVED:
            closeGamepad(e.gdevice.which);
            return false;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            onGamepadAxis(e.gaxis.which, e.gaxis.axis, e.gaxis.value);
            return false;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            onGamepadButton(e.gbutton.which, e.gbutton.button, true);
            return false;
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            onGamepadButton(e.gbutton.which, e.gbutton.button, false);
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

    // --- Keyboard queries (scancode-based; GLFW_KEY_* aliases work) ---
    bool isKeyHeld(int key) const { return validKey(key) && keysDown[key]; }
    bool isKeyPressed(int key) const { return validKey(key) && keysPressed[key]; }
    bool isKeyReleased(int key) const { return validKey(key) && keysReleased[key]; }
    bool anyKeyPressed() const {
        for (bool b : keysPressed)
            if (b)
                return true;
        return false;
    }

    // --- Mouse queries (SDL_BUTTON_*; GLFW_MOUSE_BUTTON_* aliases work) ---
    bool isMouseButtonHeld(int button) const { return validMouseButton(button) && mouseDown[button]; }
    bool isMouseButtonPressed(int button) const { return validMouseButton(button) && mousePressed[button]; }
    bool isMouseButtonReleased(int button) const { return validMouseButton(button) && mouseReleased[button]; }

    double getMouseX() const { return mouseX; }
    double getMouseY() const { return mouseY; }
    double getMouseDeltaX() const { return mouseDeltaX; }
    double getMouseDeltaY() const { return mouseDeltaY; }
    // Relative-mode deltas straight from SDL motion events (for FPS look).
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
        SDL_JoystickID id = 0;
        SDL_Gamepad* handle = nullptr;
        std::string name; // SDL gamepad name at open time (for UI status)
        std::array<float, SDL_GAMEPAD_AXIS_COUNT> axes{};
        std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> btnDown{};
        std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> btnPressed{};
        std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> btnReleased{};
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
    float gamepadAxis(int index, SDL_GamepadAxis axis) const {
        const GamepadState* gp = gamepad(index);
        if (!gp || axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT)
            return 0.0f;
        const bool isTrigger = axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
            axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        const bool isRight = axis == SDL_GAMEPAD_AXIS_RIGHTX ||
            axis == SDL_GAMEPAD_AXIS_RIGHTY;
        const float dz = isTrigger ? triggerDeadzone_
            : (isRight ? lookDeadzone_ : stickDeadzone_);
        float v = gp->axes[axis];
        // Small dead-zone so resting sticks/triggers read exactly 0.
        if (v > -dz && v < dz)
            return 0.0f;
        return v;
    }
    // Radial stick pair with rescaling: inside the deadzone reads (0,0);
    // the ring just outside maps near 0 and full deflection stays 1.
    // left = true: LEFTX/LEFTY, else RIGHTX/RIGHTY.
    void gamepadStick(int index, bool left, float& outX, float& outY) const {
        outX = 0.0f;
        outY = 0.0f;
        const GamepadState* gp = gamepad(index);
        if (!gp)
            return;
        const int ax = left ? SDL_GAMEPAD_AXIS_LEFTX : SDL_GAMEPAD_AXIS_RIGHTX;
        const int ay = left ? SDL_GAMEPAD_AXIS_LEFTY : SDL_GAMEPAD_AXIS_RIGHTY;
        const float dz = left ? stickDeadzone_ : lookDeadzone_;
        const float x = gp->axes[ax];
        const float y = gp->axes[ay];
        const float mag = std::sqrt(x * x + y * y);
        if (mag <= dz || mag <= 1e-6f)
            return;
        const float s = (mag - dz) / ((1.0f - dz) * mag);
        outX = x * s;
        outY = y * s;
    }
    // Trigger 0..1 with deadzone cut + rescale (LEFT/RIGHT_TRIGGER only).
    float gamepadTrigger(int index, SDL_GamepadAxis axis) const {
        const GamepadState* gp = gamepad(index);
        if (!gp || (axis != SDL_GAMEPAD_AXIS_LEFT_TRIGGER &&
                    axis != SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
            return 0.0f;
        float v = std::fmin(1.0f, std::fmax(0.0f, gp->axes[axis]));
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
    bool gamepadButtonHeld(int index, SDL_GamepadButton b) const {
        const GamepadState* gp = gamepad(index);
        return gp && b >= 0 && b < SDL_GAMEPAD_BUTTON_COUNT && gp->btnDown[b];
    }
    bool gamepadButtonPressed(int index, SDL_GamepadButton b) const {
        const GamepadState* gp = gamepad(index);
        return gp && b >= 0 && b < SDL_GAMEPAD_BUTTON_COUNT && gp->btnPressed[b];
    }

    // Combined keyboard + left-stick move vector (x = strafe right+, y = forward+).
    void moveVector(float& outX, float& outY) const {
        outX = (isKeyHeld(SDL_SCANCODE_D) ? 1.0f : 0.0f) - (isKeyHeld(SDL_SCANCODE_A) ? 1.0f : 0.0f);
        outY = (isKeyHeld(SDL_SCANCODE_W) ? 1.0f : 0.0f) - (isKeyHeld(SDL_SCANCODE_S) ? 1.0f : 0.0f);
        if (hasGamepad()) {
            outX += gamepadAxis(0, SDL_GAMEPAD_AXIS_LEFTX);
            outY -= gamepadAxis(0, SDL_GAMEPAD_AXIS_LEFTY);
        }
    }
    // Combined mouse + right-stick look deltas (pixels + stick*sensitivity).
    void lookDelta(float& outDX, float& outDY, float stickSensitivity = 600.0f) const {
        outDX = static_cast<float>(mouseDeltaX);
        outDY = static_cast<float>(mouseDeltaY);
        if (hasGamepad()) {
            outDX += gamepadAxis(0, SDL_GAMEPAD_AXIS_RIGHTX) * stickSensitivity / 60.0f;
            outDY += gamepadAxis(0, SDL_GAMEPAD_AXIS_RIGHTY) * stickSensitivity / 60.0f;
        }
    }

    // --- Actions ---
    void BindAction(Action a, int scancode) { actionKeys_[static_cast<size_t>(a)].push_back(scancode); }
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
        BindAction(Action::MoveForward, SDL_SCANCODE_W);
        BindAction(Action::MoveBack, SDL_SCANCODE_S);
        BindAction(Action::MoveLeft, SDL_SCANCODE_A);
        BindAction(Action::MoveRight, SDL_SCANCODE_D);
        BindAction(Action::MoveUp, SDL_SCANCODE_E);
        BindAction(Action::MoveUp, SDL_SCANCODE_SPACE);
        BindAction(Action::MoveDown, SDL_SCANCODE_Q);
        BindAction(Action::Sprint, SDL_SCANCODE_LSHIFT);
        BindAction(Action::Sprint, SDL_SCANCODE_RSHIFT);
        BindAction(Action::Crouch, SDL_SCANCODE_LCTRL);
        BindAction(Action::UiCancel, SDL_SCANCODE_ESCAPE);
        BindAction(Action::UiConfirm, SDL_SCANCODE_RETURN);
    }

private:
    static bool validKey(int key) { return key >= 0 && key < KeyCount; }
    static bool validMouseButton(int b) { return b >= 0 && b < MouseButtonCount; }

    void openGamepad(SDL_JoystickID id) {
        for (const auto& g : gamepads_)
            if (g.id == id)
                return;
        SDL_Gamepad* gp = SDL_OpenGamepad(id);
        if (!gp)
            return;
        GamepadState st;
        st.id = id;
        st.handle = gp;
        if (const char* n = SDL_GetGamepadName(gp))
            st.name = n;
        gamepads_.push_back(st);
    }
    void closeGamepad(SDL_JoystickID id) {
        for (auto it = gamepads_.begin(); it != gamepads_.end(); ++it) {
            if (it->id == id) {
                if (it->handle)
                    SDL_CloseGamepad(it->handle);
                gamepads_.erase(it);
                return;
            }
        }
    }
    void onGamepadAxis(SDL_JoystickID id, int axis, Sint16 value) {
        for (auto& g : gamepads_) {
            if (g.id == id && axis >= 0 && axis < SDL_GAMEPAD_AXIS_COUNT) {
                g.axes[axis] = static_cast<float>(value) / 32767.0f;
                return;
            }
        }
    }
    void onGamepadButton(SDL_JoystickID id, int button, bool down) {
        for (auto& g : gamepads_) {
            if (g.id == id && button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT) {
                if (down) {
                    if (!g.btnDown[button])
                        g.btnPressed[button] = true;
                    g.btnDown[button] = true;
                } else {
                    if (g.btnDown[button])
                        g.btnReleased[button] = true;
                    g.btnDown[button] = false;
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
