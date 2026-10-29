@rem Copyright (c) 2026 Samuel Martin
@rem SPDX-License-Identifier: MIT

@echo off
rem Fast build wrapper (Ninja incremental - the no-op path when nothing
rem changed is near-instant). Runs from anywhere (anchors at the repo
rem root, so double-click or call from any CWD).
rem
rem   build.bat                 - full build (everything, but ninja
rem                               skips unchanged targets - the common
rem                               "edit one file, rebuild" path)
rem   build.bat App             - one target (App / Editor / Project /
rem                               Import / Cook / Pak / GiBaker / ...)
rem   build.bat App x64-Debug   - target + config (x64-Release default)
rem
rem MSVC (NO hardcoded install path): an already-initialized environment
rem wins (the "Developer PowerShell for Visual Studio", or any shell that
rem ran vcvarsall - detected via cl.exe on PATH). Otherwise vswhere - the
rem official discovery tool shipped by every VS install - finds the
rem latest Visual Studio with the C++ workload and its vcvarsall is
rem called. No usable MSVC = a descriptive error, not a mystery crash.
rem
rem CONFIGURE: auto-fills a missing build dir (Ninja, the repo standard -
rem never the VS generator). Embree takes the PREBUILT route whenever a
rem package is available: set EMBREE_PREBUILT_DIR=<package root> (same
rem variable cmake takes), or drop one at C:\embree (the CI convention).
rem Building Embree from source ICEs MSVC on the AVX curve-intersector
rem TUs (C1001 + 0xC0000005) - the crash and the hundreds of TUs dodged.
rem Without a package it falls back to the source route like a bare
rem configure.
setlocal
pushd "%~dp0"

set "TARGET=%~1"
set "CONFIG=%~2"
if "%CONFIG%"=="" set "CONFIG=x64-Release"

rem --- MSVC environment: reuse an initialized shell, else discover VS ----
where cl.exe >nul 2>&1
if errorlevel 1 (
  call :init_msvc
  if errorlevel 1 (
    popd
    exit /b 1
  )
)

rem The Embree prebuilt preference (computed OUTSIDE the configure block:
rem plain %VAR% expands at parse time, so anything set inside the same
rem parenthesized block would read empty here).
set "EMBREE_FLAGS="
if defined EMBREE_PREBUILT_DIR set "EMBREE_FLAGS=-DEMBREE_PREBUILT_DIR=%EMBREE_PREBUILT_DIR%"
if not defined EMBREE_FLAGS if exist "C:\embree\lib\cmake" set "EMBREE_FLAGS=-DEMBREE_PREBUILT_DIR=C:/embree"

rem Auto-configure a missing build dir (fresh clone / wiped out/).
rem CMAKE_BUILD_TYPE derives from the config dir name (x64-Release ->
rem Release, x64-Debug -> Debug).
if not exist "out\build\%CONFIG%\CMakeCache.txt" (
  set "TYPE=%CONFIG:x64-=%"
  call :configure
  if errorlevel 1 goto failcfg
)

rem The build itself: full tree by default (ninja skips what is unchanged
rem - the fast path), or the one requested target.
if "%TARGET%"=="" (
  cmake --build out/build/%CONFIG%
) else (
  echo build.bat: target %TARGET% (%CONFIG%^)
  cmake --build out/build/%CONFIG% --target %TARGET%
)
if errorlevel 1 goto fail

echo build.bat: OK (%CONFIG%, target: %TARGET%)
popd
exit /b 0

:init_msvc
rem vswhere: the officially supported VS discovery tool (lives in the
rem VS installer directory). -products * covers Community / Pro /
rem Enterprise / BuildTools alike; -prerelease ALSO covers Preview /
rem Insiders channels (vswhere hides them by default - without the flag
rem a prerelease-only machine discovers nothing); the -requires filter
rem keeps only installs carrying the C++ desktop workload's VC tools.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: vswhere not found - no Visual Studio installation detected.
  echo        Install Visual Studio 2022 or newer with the
  echo        "Desktop development with C++" workload, or build from the
  echo        "Developer PowerShell for Visual Studio" which comes
  echo        pre-initialized.
  exit /b 1
)
set "VSROOT="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
  echo ERROR: vswhere found no Visual Studio with the C++ workload
  echo        ^(component Microsoft.VisualStudio.Component.VC.Tools.x86.x64^).
  echo        Install the workload, or build from the "Developer PowerShell
  echo        for Visual Studio".
  exit /b 1
)
set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
  echo ERROR: vcvarsall.bat not found under "%VSROOT%" - unexpected VS
  echo        layout; initialize MSVC yourself and re-run.
  exit /b 1
)
call "%VCVARS%" x64 >nul
if errorlevel 1 (
  echo ERROR: vcvarsall x64 failed under "%VSROOT%".
  exit /b 1
)
echo build.bat: MSVC initialized from "%VSROOT%"
exit /b 0

:configure
echo build.bat: configuring out\build\%CONFIG% (Ninja, %TYPE%^)
if defined EMBREE_FLAGS echo build.bat: Embree prebuilt route %EMBREE_FLAGS%
cmake -B out/build/%CONFIG% -G Ninja -DCMAKE_BUILD_TYPE=%TYPE% %EMBREE_FLAGS% .
exit /b %errorlevel%

:failcfg
echo ERROR: configure failed (see the cmake output above^).
popd
exit /b 1

:fail
echo ERROR: build failed (see the ninja output above^).
popd
exit /b 1
