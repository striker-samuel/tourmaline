// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

#ifndef _WIN32
#error "The experimental Win32 editor host is Windows-only"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <vulkan/vulkan.h>

namespace editor::experimental {

// Standalone shell prototype, NOT a replacement for tour::App yet.
// Owns an HWND and the Win32 message pump; rendering stays with the engine.
class Win32Host final {
public:
    Win32Host() = default;
    ~Win32Host();
    Win32Host(const Win32Host&) = delete;
    Win32Host& operator=(const Win32Host&) = delete;

    bool Create(HINSTANCE instance, const wchar_t* title, int width, int height);
    bool PumpMessages();
    HWND Window() const noexcept { return window_; }
    VkResult CreateVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) const;

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    bool running_ = false;
};
} // namespace editor::experimental
