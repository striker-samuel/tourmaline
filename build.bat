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
rem   build.bat App             - one target (App / Editor / Cook / Pak /
rem                               GiBaker / Import / test_<name> / ...)
rem   build.bat App x64-Debug   - target + config (x64-Release default)
rem
rem CONFIGURE: auto-fills a missing build dir (Ninja, the repo standard -
rem never the VS generator). Embree takes the PREBUILT route whenever a
rem package is found (F:\tools\embree-4.4.1, then C:\embree): building
rem Embree from source ICEs MSVC on the AVX curve-intersector TUs (C1001
rem + 0xC0000005) - the crash and the hundreds of TUs dodged. Without a
rem package it falls back to the source route like a bare configure.
rem vcvarsall is REQUIRED for cl.exe from a plain shell (AGENTS.md).
setlocal
pushd "%~dp0"

set "TARGET=%~1"
set "CONFIG=%~2"
if "%CONFIG%"=="" set "CONFIG=x64-Release"

set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
  echo ERROR: vcvarsall not found: "%VCVARS%"
  popd
  exit /b 1
)
call "%VCVARS%" x64 >nul
if errorlevel 1 (
  echo ERROR: vcvarsall x64 failed.
  popd
  exit /b 1
)

rem The Embree prebuilt preference (computed OUTSIDE the configure block:
rem plain %VAR% expands at parse time, so anything set inside the same
rem parenthesized block would read empty here).
set "EMBREE_FLAGS="
if exist "F:\tools\embree-4.4.1\lib\cmake" set "EMBREE_FLAGS=-DEMBREE_PREBUILT_DIR=F:/tools/embree-4.4.1"
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
