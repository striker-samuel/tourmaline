@rem Copyright (c) 2026 Samuel Martin
@rem SPDX-License-Identifier: MIT

@rem The common-case shortcut: build the App target. A thin wrapper
@rem around build.bat - MSVC discovery/reuse, the missing-build-dir
@rem auto-configure and the Embree prebuilt preference all live there
@rem (one implementation, no duplicated vcvarsall logic to drift).
@rem
@rem   build_app.bat              == build.bat App           (x64-Release)
@rem   build_app.bat x64-Debug    == build.bat App x64-Debug
@echo off
call "%~dp0build.bat" App %*
