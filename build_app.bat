@rem Copyright (c) 2026 Samuel Martin
@rem SPDX-License-Identifier: MIT

@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat" x64
cmake --build out/build/x64-Release --target App