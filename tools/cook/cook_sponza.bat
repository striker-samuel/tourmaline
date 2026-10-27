@rem Copyright (c) 2026 Samuel Martin
@rem SPDX-License-Identifier: MIT

@rem THE PROJECT BUILD (the canonical pipeline — see the asset-pipeline
@rem spec): the script is a thin entry point onto Project.exe; every
@rem setting (sun/LOD/meshlets/mask-transmission/cubemap tuning) lives in
@rem the PROJECT and the SCENE'S exportSettings node, never duplicated
@rem here. First run creates the project + imports the source + seeds the
@rem authored scene; every later run is the incremental build (reimport
@rem stale only -> Cook -> GiBaker -> Pak with the freshness gate).
@rem
@rem The pak output lands at <project>/paks/Sponza.pak and syncs into the
@rem ninja build content dirs the App loads.
@echo off
setlocal

set "ROOT=%~dp0..\.."
set "PROJECT=F:\projects\Sponza"
set "GLTF=F:\content\models\sponza\Sponza.gltf"
set "TOUR=F:\content\models\sponza\Sponza.tour"
set "TOOL=%ROOT%\out\build\x64-Release\Project.exe"
set "OUTPAK=%PROJECT%\paks\Sponza.pak"

if not exist "%TOOL%" (
  echo ERROR: %TOOL% not found. Build it first: cmake --build out/build/x64-Release --target Project
  exit /b 1
)

if not exist "%PROJECT%\project.tourproj" (
  "%TOOL%" init --project "%PROJECT%" --from-gltf "%GLTF%" --from-scene "%TOUR%"
  if errorlevel 1 exit /b 1
)

"%TOOL%" build --project "%PROJECT%" --output "%OUTPAK%"
if errorlevel 1 exit /b 1

copy /y "%OUTPAK%" "%ROOT%\out\build\x64-Release\content\paks\sponza.pak"
if errorlevel 1 exit /b 1
if exist "%ROOT%\out\build\x64-Debug" copy /y "%OUTPAK%" "%ROOT%\out\build\x64-Debug\content\paks\sponza.pak" >nul

echo sponza.pak built through the project pipeline and synced.
exit /b 0
