// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include "win32_host.h"

// A native-shell smoke test only. Editor.exe still uses SDL3.
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    editor::experimental::Win32Host host;
    if (!host.Create(instance, L"Tourmaline Win32 Host (Experimental)", 1280, 800))
        return 1;
    while (host.PumpMessages())
        WaitMessage();
    return 0;
}
