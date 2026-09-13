@echo off
rem Recook sponza.pak with the current engine structs (64-byte StaticVertex,
rem 128-byte MaterialValues) and sync it into the build content dirs the App
rem loads (content/paks relative to the App working directory).
rem Usage: run from anywhere (paths anchor at the repo root via this script).
setlocal
pushd "%~dp0..\.."
if not exist build\Release\PakCooker.exe (
  echo ERROR: build\Release\PakCooker.exe not found.
  echo Build it first: cmake --build build --config Release --target PakCooker
  popd
  exit /b 1
)
build\Release\PakCooker.exe --content-dir F:\content --in-vfs models/sponza/Sponza.gltf --out-pak F:\content\paks\sponza.pak --meshlets on --verify on
if errorlevel 1 goto fail
copy /y F:\content\paks\sponza.pak build\Debug\content\paks\sponza.pak
if errorlevel 1 goto fail
copy /y F:\content\paks\sponza.pak build\Release\content\paks\sponza.pak
if errorlevel 1 goto fail
echo sponza.pak recooked and synced.
popd
exit /b 0
:fail
popd
exit /b 1
