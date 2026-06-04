#pragma once

#include <GLFW/glfw3.h>
#include <array>

class Input {
public:
    void beginFrame() {
        keysPressed.fill(false);
        keysReleased.fill(false);

        mousePressed.fill(false);
        mouseReleased.fill(false);

        mouseDeltaX = 0.0;
        mouseDeltaY = 0.0;
        scrollDeltaX = 0.0;
        scrollDeltaY = 0.0;
    }

    void onKey(int key, int scancode, int action, int mods) {
        if (key < 0 || key >= GLFW_KEY_LAST + 1) {
            return;
        }

        if (action == GLFW_PRESS) {
            keysDown[key] = true;
            keysPressed[key] = true;
        } else if (action == GLFW_RELEASE) {
            keysDown[key] = false;
            keysReleased[key] = true;
        }

        keyMods = mods;
    }

    void onMouseButton(int button, int action, int mods) {
        if (button < 0 || button >= GLFW_MOUSE_BUTTON_LAST + 1) {
            return;
        }

        if (action == GLFW_PRESS) {
            mouseDown[button] = true;
            mousePressed[button] = true;
        } else if (action == GLFW_RELEASE) {
            mouseDown[button] = false;
            mouseReleased[button] = true;
        }

        keyMods = mods;
    }

    void onCursorMove(double x, double y) {
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

        lastMouseX = x;
        lastMouseY = y;

        mouseX = x;
        mouseY = y;
    }

    void onScroll(double xoffset, double yoffset) {
        scrollDeltaX += xoffset;
        scrollDeltaY += yoffset;
    }

    bool isKeyHeld(int key) const {
        return validKey(key) && keysDown[key];
    }

    bool isKeyPressed(int key) const {
        return validKey(key) && keysPressed[key];
    }

    bool isKeyReleased(int key) const {
        return validKey(key) && keysReleased[key];
    }

    bool isMouseButtonHeld(int button) const {
        return validMouseButton(button) && mouseDown[button];
    }

    bool isMouseButtonPressed(int button) const {
        return validMouseButton(button) && mousePressed[button];
    }

    bool isMouseButtonReleased(int button) const {
        return validMouseButton(button) && mouseReleased[button];
    }

    double getMouseX() const {
        return mouseX;
    }

    double getMouseY() const {
        return mouseY;
    }

    double getMouseDeltaX() const {
        return mouseDeltaX;
    }

    double getMouseDeltaY() const {
        return mouseDeltaY;
    }

    double getScrollDeltaX() const {
        return scrollDeltaX;
    }

    double getScrollDeltaY() const {
        return scrollDeltaY;
    }

    int getMods() const {
        return keyMods;
    }

private:
    static constexpr int KeyCount = GLFW_KEY_LAST + 1;
    static constexpr int MouseButtonCount = GLFW_MOUSE_BUTTON_LAST + 1;

    std::array<bool, KeyCount> keysDown{};
    std::array<bool, KeyCount> keysPressed{};
    std::array<bool, KeyCount> keysReleased{};

    std::array<bool, MouseButtonCount> mouseDown{};
    std::array<bool, MouseButtonCount> mousePressed{};
    std::array<bool, MouseButtonCount> mouseReleased{};

    double mouseX = 0.0;
    double mouseY = 0.0;

    double lastMouseX = 0.0;
    double lastMouseY = 0.0;

    double mouseDeltaX = 0.0;
    double mouseDeltaY = 0.0;

    double scrollDeltaX = 0.0;
    double scrollDeltaY = 0.0;

    bool hasMousePosition = false;

    int keyMods = 0;

    static bool validKey(int key) {
        return key >= 0 && key < KeyCount;
    }

    static bool validMouseButton(int button) {
        return button >= 0 && button < MouseButtonCount;
    }
};