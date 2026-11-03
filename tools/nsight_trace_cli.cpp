// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Small Windows CLI wrapper around tools/perf_harness.py. No Nsight SDK,
// rendering engine, Python embedding or profiler DLL linked into this exe.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cwchar>
#include <cstdlib>

namespace fs = std::filesystem;

static std::wstring QuoteWindows(const std::wstring& input) {
    // Correct CommandLineToArgvW-style escaping for embedded quotes and
    // backslashes before a quote or the closing quote.
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : input) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        out.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"') out += L'\\';
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

static fs::path FindScript(fs::path from) {
    // The exe is placed in out/build/<config> and the script is at
    // <repo>/tools/perf_harness.py. Working directory is irrelevant.
    for (int depth = 0; depth < 9; ++depth) {
        const auto candidate = from / "tools" / "perf_harness.py";
        if (fs::is_regular_file(candidate)) return fs::absolute(candidate);
        if (!from.has_parent_path() || from == from.parent_path()) break;
        from = from.parent_path();
    }
    throw std::runtime_error("tools/perf_harness.py not found above NsightTrace.exe");
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc <= 1) {
            std::wcout <<
                L"NsightTrace.exe --scenario ID [--trace-frames 10] "
                L"[--capture-dir D:/Captures] [--scratch-dir E:/Scratch]\n"
                L"NsightTrace.exe --save-paths --capture-dir D:/Captures --scratch-dir E:/Scratch\n"
                L"NsightTrace.exe --show-paths\n"
                L"NsightTrace.exe --help\n";
            return 0;
        }
        std::wstring module(32768, L'\0');
        const DWORD n = GetModuleFileNameW(nullptr, module.data(), DWORD(module.size()));
        if (!n || n >= module.size()) throw std::runtime_error("GetModuleFileNameW failed");
        module.resize(n);
        const auto binDir = fs::path(module).parent_path();
        const auto script = FindScript(binDir);

        const wchar_t* pythonOverride = _wgetenv(L"TOURMALINE_PYTHON");
        const std::wstring python = pythonOverride && *pythonOverride
            ? pythonOverride : L"python.exe";
        std::vector<std::wstring> cmd = {python, script.wstring(), L"trace"};
        bool hasExe = false;
        for (int i = 1; i < argc; ++i) {
            if (std::wstring(argv[i]) == L"--exe") hasExe = true;
            cmd.emplace_back(argv[i]);
        }
        // Default to the Benchmark compiled beside this launcher, not the
        // harness's historical x64-Release default.
        if (!hasExe) {
            cmd.emplace_back(L"--exe");
            cmd.emplace_back((binDir / "Benchmark.exe").wstring());
        }

        std::wstring commandLine;
        for (const auto& arg : cmd) {
            if (!commandLine.empty()) commandLine += L' ';
            commandLine += QuoteWindows(arg);
        }
        std::vector<wchar_t> command(commandLine.begin(), commandLine.end());
        command.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                            0, nullptr, nullptr, &startup, &child)) {
            std::wcerr << L"NsightTrace: cannot start Python (Win32 error "
                       << GetLastError() << L"). Set TOURMALINE_PYTHON to python.exe path.\n";
            return 2;
        }
        CloseHandle(child.hThread);
        const DWORD wait = WaitForSingleObject(child.hProcess, INFINITE);
        DWORD code = 1;
        if (wait == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &code);
        CloseHandle(child.hProcess);
        return static_cast<int>(code);
    } catch (const std::exception& e) {
        std::cerr << "NsightTrace: " << e.what() << '\n';
        return 2;
    }
}
