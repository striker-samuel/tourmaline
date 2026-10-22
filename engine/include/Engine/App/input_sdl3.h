// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
//
// SDL3 -> engine input bridge. This is the ONLY translation unit allowed to
// name SDL input types: it owns the opened SDL_Gamepad handles (hotplug +
// shutdown lifetime) and converts each SDL_Event into the backend-neutral
// input::Event consumed by Input / App::OnInputEvent.
//
#include <Engine/App/input.h>

#include <SDL3/SDL.h>

#include <vector>

namespace smvk {

class SdlInputBackend {
public:
    SdlInputBackend() = default;
    ~SdlInputBackend();

    SdlInputBackend(const SdlInputBackend&) = delete;
    SdlInputBackend& operator=(const SdlInputBackend&) = delete;

    // Translates one SDL event. Returns false for events the input layer
    // does not own (window/expose/display/etc.). Gamepad add/remove events
    // open/close the backend-owned handle and still emit an engine event
    // (with the device name) so Input can track the device.
    bool Translate(const SDL_Event& src, input::Event& out);

private:
    struct OpenGamepad {
        input::GamepadId id = 0;
        SDL_Gamepad* handle = nullptr;
    };

    std::vector<OpenGamepad> gamepads_;
};

} // namespace smvk
