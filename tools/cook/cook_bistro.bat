@echo off
rem Stage (reuse) + recook bistro.pak with the current engine structs
rem (64-byte StaticVertex, 128-byte MaterialValues) and sync it into the
rem build content dirs the App loads (content/paks relative to the App
rem working directory).
rem --skip-convert reuses the already-staged .ktx2 files in F:/content/models.
rem Drop it (or run tools/cook_bistro.py manually) for a from-scratch convert.
rem Usage: run from anywhere (paths anchor at the repo root via this script).
setlocal
pushd "%~dp0..\.."
if not exist build\Release\PakCooker.exe (
  echo ERROR: build\Release\PakCooker.exe not found.
  echo Build it first: cmake --build build --config Release --target PakCooker
  popd
  exit /b 1
)
py tools\cook_bistro.py --skip-convert --app-content F:/content --pakcooker "%CD%\build\Release\PakCooker.exe"
if errorlevel 1 goto fail
copy /y F:\content\paks\bistro.pak build\Debug\content\paks\bistro.pak
if errorlevel 1 goto fail
copy /y F:\content\paks\bistro.pak build\Release\content\paks\bistro.pak
if errorlevel 1 goto fail
echo bistro.pak recooked and synced.
popd
exit /b 0
:fail
popd
exit /b 1
