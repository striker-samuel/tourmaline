// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include "win32_host.h"

namespace editor::experimental {
namespace {
constexpr wchar_t kClassName[] = L"TourmalineExperimentalWin32EditorHost";
}

Win32Host::~Win32Host() {
    if (window_)
        DestroyWindow(window_);
    if (instance_)
        UnregisterClassW(kClassName, instance_);
}

bool Win32Host::Create(HINSTANCE instance, const wchar_t* title, int width, int height) {
    if (!instance || window_)
        return false;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = &Win32Host::WindowProc;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_OWNDC;
    if (!RegisterClassExW(&wc))
        return false;
    instance_ = instance;

    RECT bounds{0, 0, width, height};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRectEx(&bounds, style, FALSE, 0);
    window_ = CreateWindowExW(0, kClassName, title, style,
        CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
        bounds.bottom - bounds.top, nullptr, nullptr, instance, this);
    if (!window_)
        return false;
    running_ = true;
    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);
    return true;
}

bool Win32Host::PumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            running_ = false;
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return running_;
}

VkResult Win32Host::CreateVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) const {
    if (!instance || !window_ || !surface)
        return VK_ERROR_INITIALIZATION_FAILED;
    // The caller must enable VK_KHR_surface + VK_KHR_win32_surface
    // when creating the VkInstance and owns destruction of the surface.
    VkWin32SurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = instance_;
    info.hwnd = window_;
    return vkCreateWin32SurfaceKHR(instance, &info, nullptr, surface);
}

LRESULT CALLBACK Win32Host::WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    Win32Host* host = reinterpret_cast<Win32Host*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        host = static_cast<Win32Host*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }
    return host ? host->HandleMessage(hwnd, msg, wparam, lparam)
                : DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT Win32Host::HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        window_ = nullptr;
        running_ = false;
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}
} // namespace editor::experimental
