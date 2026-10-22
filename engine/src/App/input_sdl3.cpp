// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// SDL3 input translation. Keeps every SDL input type behind this boundary:
// Input (and its consumers) only see input::Event and the engine enums.
// Key ids are USB HID usages, which is also SDL's scancode numbering - the
// static_asserts below pin that contract so a backend upgrade cannot
// silently remap the engine's physical keys.

#include <Engine/App/input_sdl3.h>

namespace {

#define TL_ASSERT_KEY(engineName, sdlName)                                    \
    static_assert(static_cast<uint16_t>(input::Key::engineName)               \
            == SDL_SCANCODE_##sdlName,                                        \
        "input::Key::" #engineName " must match SDL_SCANCODE_" #sdlName)

TL_ASSERT_KEY(A, A);
TL_ASSERT_KEY(B, B);
TL_ASSERT_KEY(C, C);
TL_ASSERT_KEY(D, D);
TL_ASSERT_KEY(E, E);
TL_ASSERT_KEY(F, F);
TL_ASSERT_KEY(G, G);
TL_ASSERT_KEY(H, H);
TL_ASSERT_KEY(I, I);
TL_ASSERT_KEY(J, J);
TL_ASSERT_KEY(K, K);
TL_ASSERT_KEY(L, L);
TL_ASSERT_KEY(M, M);
TL_ASSERT_KEY(N, N);
TL_ASSERT_KEY(O, O);
TL_ASSERT_KEY(P, P);
TL_ASSERT_KEY(Q, Q);
TL_ASSERT_KEY(R, R);
TL_ASSERT_KEY(S, S);
TL_ASSERT_KEY(T, T);
TL_ASSERT_KEY(U, U);
TL_ASSERT_KEY(V, V);
TL_ASSERT_KEY(W, W);
TL_ASSERT_KEY(X, X);
TL_ASSERT_KEY(Y, Y);
TL_ASSERT_KEY(Z, Z);
TL_ASSERT_KEY(Num1, 1);
TL_ASSERT_KEY(Num2, 2);
TL_ASSERT_KEY(Num3, 3);
TL_ASSERT_KEY(Num4, 4);
TL_ASSERT_KEY(Num5, 5);
TL_ASSERT_KEY(Num6, 6);
TL_ASSERT_KEY(Num7, 7);
TL_ASSERT_KEY(Num8, 8);
TL_ASSERT_KEY(Num9, 9);
TL_ASSERT_KEY(Num0, 0);
TL_ASSERT_KEY(Enter, RETURN);
TL_ASSERT_KEY(Escape, ESCAPE);
TL_ASSERT_KEY(Backspace, BACKSPACE);
TL_ASSERT_KEY(Tab, TAB);
TL_ASSERT_KEY(Space, SPACE);
TL_ASSERT_KEY(Minus, MINUS);
TL_ASSERT_KEY(Equal, EQUALS);
TL_ASSERT_KEY(LeftBracket, LEFTBRACKET);
TL_ASSERT_KEY(RightBracket, RIGHTBRACKET);
TL_ASSERT_KEY(Backslash, BACKSLASH);
TL_ASSERT_KEY(Semicolon, SEMICOLON);
TL_ASSERT_KEY(Apostrophe, APOSTROPHE);
TL_ASSERT_KEY(GraveAccent, GRAVE);
TL_ASSERT_KEY(Comma, COMMA);
TL_ASSERT_KEY(Period, PERIOD);
TL_ASSERT_KEY(Slash, SLASH);
TL_ASSERT_KEY(CapsLock, CAPSLOCK);
TL_ASSERT_KEY(F1, F1);
TL_ASSERT_KEY(F2, F2);
TL_ASSERT_KEY(F3, F3);
TL_ASSERT_KEY(F4, F4);
TL_ASSERT_KEY(F5, F5);
TL_ASSERT_KEY(F6, F6);
TL_ASSERT_KEY(F7, F7);
TL_ASSERT_KEY(F8, F8);
TL_ASSERT_KEY(F9, F9);
TL_ASSERT_KEY(F10, F10);
TL_ASSERT_KEY(F11, F11);
TL_ASSERT_KEY(F12, F12);
TL_ASSERT_KEY(PrintScreen, PRINTSCREEN);
TL_ASSERT_KEY(ScrollLock, SCROLLLOCK);
TL_ASSERT_KEY(Pause, PAUSE);
TL_ASSERT_KEY(Insert, INSERT);
TL_ASSERT_KEY(Home, HOME);
TL_ASSERT_KEY(PageUp, PAGEUP);
TL_ASSERT_KEY(Delete, DELETE);
TL_ASSERT_KEY(End, END);
TL_ASSERT_KEY(PageDown, PAGEDOWN);
TL_ASSERT_KEY(Right, RIGHT);
TL_ASSERT_KEY(Left, LEFT);
TL_ASSERT_KEY(Down, DOWN);
TL_ASSERT_KEY(Up, UP);
TL_ASSERT_KEY(NumLock, NUMLOCKCLEAR);
TL_ASSERT_KEY(KeypadDivide, KP_DIVIDE);
TL_ASSERT_KEY(KeypadMultiply, KP_MULTIPLY);
TL_ASSERT_KEY(KeypadSubtract, KP_MINUS);
TL_ASSERT_KEY(KeypadAdd, KP_PLUS);
TL_ASSERT_KEY(KeypadEnter, KP_ENTER);
TL_ASSERT_KEY(Keypad1, KP_1);
TL_ASSERT_KEY(Keypad2, KP_2);
TL_ASSERT_KEY(Keypad3, KP_3);
TL_ASSERT_KEY(Keypad4, KP_4);
TL_ASSERT_KEY(Keypad5, KP_5);
TL_ASSERT_KEY(Keypad6, KP_6);
TL_ASSERT_KEY(Keypad7, KP_7);
TL_ASSERT_KEY(Keypad8, KP_8);
TL_ASSERT_KEY(Keypad9, KP_9);
TL_ASSERT_KEY(Keypad0, KP_0);
TL_ASSERT_KEY(KeypadDecimal, KP_PERIOD);
TL_ASSERT_KEY(KeypadEqual, KP_EQUALS);
TL_ASSERT_KEY(Menu, MENU);
TL_ASSERT_KEY(LeftControl, LCTRL);
TL_ASSERT_KEY(LeftShift, LSHIFT);
TL_ASSERT_KEY(LeftAlt, LALT);
TL_ASSERT_KEY(LeftSuper, LGUI);
TL_ASSERT_KEY(RightControl, RCTRL);
TL_ASSERT_KEY(RightShift, RSHIFT);
TL_ASSERT_KEY(RightAlt, RALT);
TL_ASSERT_KEY(RightSuper, RGUI);

#undef TL_ASSERT_KEY

#define TL_ASSERT_PAD_BUTTON(engineName, sdlName)                             \
    static_assert(static_cast<int>(input::GamepadButton::engineName)          \
            == SDL_GAMEPAD_BUTTON_##sdlName,                                  \
        "input::GamepadButton::" #engineName " must match SDL's layout")

TL_ASSERT_PAD_BUTTON(South, SOUTH);
TL_ASSERT_PAD_BUTTON(East, EAST);
TL_ASSERT_PAD_BUTTON(West, WEST);
TL_ASSERT_PAD_BUTTON(North, NORTH);
TL_ASSERT_PAD_BUTTON(Back, BACK);
TL_ASSERT_PAD_BUTTON(Guide, GUIDE);
TL_ASSERT_PAD_BUTTON(Start, START);
TL_ASSERT_PAD_BUTTON(LeftStick, LEFT_STICK);
TL_ASSERT_PAD_BUTTON(RightStick, RIGHT_STICK);
TL_ASSERT_PAD_BUTTON(LeftShoulder, LEFT_SHOULDER);
TL_ASSERT_PAD_BUTTON(RightShoulder, RIGHT_SHOULDER);
TL_ASSERT_PAD_BUTTON(DpadUp, DPAD_UP);
TL_ASSERT_PAD_BUTTON(DpadDown, DPAD_DOWN);
TL_ASSERT_PAD_BUTTON(DpadLeft, DPAD_LEFT);
TL_ASSERT_PAD_BUTTON(DpadRight, DPAD_RIGHT);
TL_ASSERT_PAD_BUTTON(Misc1, MISC1);
TL_ASSERT_PAD_BUTTON(RightPaddle1, RIGHT_PADDLE1);
TL_ASSERT_PAD_BUTTON(LeftPaddle1, LEFT_PADDLE1);
TL_ASSERT_PAD_BUTTON(RightPaddle2, RIGHT_PADDLE2);
TL_ASSERT_PAD_BUTTON(LeftPaddle2, LEFT_PADDLE2);
TL_ASSERT_PAD_BUTTON(Touchpad, TOUCHPAD);
TL_ASSERT_PAD_BUTTON(Misc2, MISC2);
TL_ASSERT_PAD_BUTTON(Misc3, MISC3);
TL_ASSERT_PAD_BUTTON(Misc4, MISC4);
TL_ASSERT_PAD_BUTTON(Misc5, MISC5);
TL_ASSERT_PAD_BUTTON(Misc6, MISC6);

#undef TL_ASSERT_PAD_BUTTON

#define TL_ASSERT_PAD_AXIS(engineName, sdlName)                               \
    static_assert(static_cast<int>(input::GamepadAxis::engineName)            \
            == SDL_GAMEPAD_AXIS_##sdlName,                                    \
        "input::GamepadAxis::" #engineName " must match SDL's layout")

TL_ASSERT_PAD_AXIS(LeftX, LEFTX);
TL_ASSERT_PAD_AXIS(LeftY, LEFTY);
TL_ASSERT_PAD_AXIS(RightX, RIGHTX);
TL_ASSERT_PAD_AXIS(RightY, RIGHTY);
TL_ASSERT_PAD_AXIS(LeftTrigger, LEFT_TRIGGER);
TL_ASSERT_PAD_AXIS(RightTrigger, RIGHT_TRIGGER);

#undef TL_ASSERT_PAD_AXIS

input::Key KeyFromScancode(SDL_Scancode scancode) {
    const int v = static_cast<int>(scancode);
    if (v < 0 || v >= input::KeyCount)
        return input::Key::Unknown;
    return static_cast<input::Key>(v);
}

uint16_t ModsFromSDL(SDL_Keymod mod) {
    uint16_t m = input::ModifierNone;
    if (mod & SDL_KMOD_SHIFT)
        m |= input::ModifierShift;
    if (mod & SDL_KMOD_CTRL)
        m |= input::ModifierCtrl;
    if (mod & SDL_KMOD_ALT)
        m |= input::ModifierAlt;
    if (mod & SDL_KMOD_GUI)
        m |= input::ModifierSuper;
    if (mod & SDL_KMOD_CAPS)
        m |= input::ModifierCapsLock;
    if (mod & SDL_KMOD_NUM)
        m |= input::ModifierNumLock;
    return m;
}

} // namespace

smvk::SdlInputBackend::~SdlInputBackend() {
    for (auto& gp : gamepads_)
        if (gp.handle)
            SDL_CloseGamepad(gp.handle);
    gamepads_.clear();
}

bool smvk::SdlInputBackend::Translate(const SDL_Event& src, input::Event& out) {
    out = input::Event{};
    switch (src.type) {
    case SDL_EVENT_QUIT:
        out.type = input::EventType::Quit;
        return true;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        out.type = input::EventType::WindowClose;
        return true;
    case SDL_EVENT_KEY_DOWN:
        out.type = input::EventType::KeyDown;
        out.key = KeyFromScancode(src.key.scancode);
        out.repeat = src.key.repeat;
        out.mods = ModsFromSDL(src.key.mod);
        return true;
    case SDL_EVENT_KEY_UP:
        out.type = input::EventType::KeyUp;
        out.key = KeyFromScancode(src.key.scancode);
        out.mods = ModsFromSDL(src.key.mod);
        return true;
    case SDL_EVENT_TEXT_INPUT:
        out.type = input::EventType::TextInput;
        if (src.text.text)
            out.text = src.text.text;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        out.type = input::EventType::MouseButtonDown;
        out.mouseButton = static_cast<input::MouseButton>(src.button.button);
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        out.type = input::EventType::MouseButtonUp;
        out.mouseButton = static_cast<input::MouseButton>(src.button.button);
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        out.type = input::EventType::MouseMotion;
        out.x = src.motion.x;
        out.y = src.motion.y;
        out.xrel = src.motion.xrel;
        out.yrel = src.motion.yrel;
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        out.type = input::EventType::MouseWheel;
        out.x = src.wheel.x;
        out.y = src.wheel.y;
        return true;
    case SDL_EVENT_GAMEPAD_ADDED: {
        const input::GamepadId id = static_cast<input::GamepadId>(src.gdevice.which);
        for (const auto& gp : gamepads_)
            if (gp.id == id)
                return false;
        SDL_Gamepad* handle = SDL_OpenGamepad(src.gdevice.which);
        if (!handle)
            return false;
        gamepads_.push_back({id, handle});
        out.type = input::EventType::GamepadAdded;
        out.gamepadId = id;
        if (const char* name = SDL_GetGamepadName(handle))
            out.text = name;
        return true;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
        const input::GamepadId id = static_cast<input::GamepadId>(src.gdevice.which);
        for (auto it = gamepads_.begin(); it != gamepads_.end(); ++it) {
            if (it->id == id) {
                if (it->handle)
                    SDL_CloseGamepad(it->handle);
                gamepads_.erase(it);
                break;
            }
        }
        out.type = input::EventType::GamepadRemoved;
        out.gamepadId = id;
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        out.type = input::EventType::GamepadAxisMotion;
        out.gamepadId = static_cast<input::GamepadId>(src.gaxis.which);
        out.gamepadAxis = static_cast<input::GamepadAxis>(src.gaxis.axis);
        out.axis = static_cast<float>(src.gaxis.value) / 32767.0f;
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        out.type = input::EventType::GamepadButtonDown;
        out.gamepadId = static_cast<input::GamepadId>(src.gbutton.which);
        out.gamepadButton = static_cast<input::GamepadButton>(src.gbutton.button);
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        out.type = input::EventType::GamepadButtonUp;
        out.gamepadId = static_cast<input::GamepadId>(src.gbutton.which);
        out.gamepadButton = static_cast<input::GamepadButton>(src.gbutton.button);
        return true;
    case SDL_EVENT_DROP_FILE:
        if (!src.drop.data)
            return false;
        out.type = input::EventType::FileDrop;
        out.text = src.drop.data;
        out.x = src.drop.x;
        out.y = src.drop.y;
        return true;
    default:
        return false;
    }
}
