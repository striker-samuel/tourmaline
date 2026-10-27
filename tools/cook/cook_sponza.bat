@rem Copyright (c) 2026 Samuel Martin
@rem SPDX-License-Identifier: MIT

@rem THE SPONZA DEMO BUILD (the canonical project pipeline — see the
@rem asset-pipeline spec + README "Content pipeline"):
@rem   Project.exe init  --project <dir> --from-gltf <glTF> --from-scene <tour>
@rem   Project.exe build --project <dir> --output <pak>
@rem First run: creates the project, copies the glTF + its dependencies IN,
@rem derives the renderer-ready .geometry (LOD chains + meshlets bake at
@rem import), seeds the authored scene, then cooks + bakes + paks with the
@rem bake-freshness gate. Later runs are incremental (reimport stale only,
@rem unchanged bakes stand) — and after the first successful build the
@rem project is SELF-CONTAINED: it builds again with no external content
@rem store present at all.
@rem
@rem Every setting (sun/LOD/meshlets/mask-transmission/cubemap tuning) lives
@rem in the PROJECT and the SCENE'S exportSettings node — never here.
@rem
@rem PATHS (machine-flexible, all overridable):
@rem   TOURMALINE_PROJECT_DIR  the project dir (default:
@rem                           %USERPROFILE%\Documents\Tourmaline\Sponza —
@rem                           YOUR user profile, never the repo tree)
@rem   TOURMALINE_CONTENT_DIR   the external content store (default: F:\content)
@rem                            — must contain models\sponza\Sponza.gltf
@rem                              + Sponza.bin + textures + Sponza.tour
@rem The Sponza source glTF is NOT in git (large binaries live in the
@rem external store). Point TOURMALINE_CONTENT_DIR at your store, or copy
@rem an already-built project directory over — the build is self-contained
@rem once the project exists.
@echo off
setlocal

set "ROOT=%~dp0..\.."
set "TOOL=%ROOT%\out\build\x64-Release\Project.exe"

rem ---- the project: the USER'S Documents (overridable) ---------------------
set "PROJECT=%USERPROFILE%\Documents\Tourmaline\Sponza"
if not "%TOURMALINE_PROJECT_DIR%"=="" set "PROJECT=%TOURMALINE_PROJECT_DIR%"

rem ---- the content store (overridable) --------------------------------------
set "CONTENT=F:\content"
if not "%TOURMALINE_CONTENT_DIR%"=="" set "CONTENT=%TOURMALINE_CONTENT_DIR%"
set "GLTF=%CONTENT%\models\sponza\Sponza.gltf"
set "TOUR=%CONTENT%\models\sponza\Sponza.tour"
set "OUTPAK=%PROJECT%\paks\Sponza.pak"

if not exist "%TOOL%" (
  echo ERROR: %TOOL% not found.
  echo Build the tree first: cmake --build out/build/x64-Release --target Project
  exit /b 1
)

if not exist "%PROJECT%\project.tourproj" (
  if not exist "%GLTF%" (
    echo ERROR: the Sponza source glTF was not found:
    echo    %GLTF%
    echo.
    echo The demo content is not in git. Either:
    echo   1. set TOURMALINE_CONTENT_DIR to your content store root ^(it
    echo      must contain models\sponza\Sponza.gltf + Sponza.bin + the
    echo      textures + the authored Sponza.tour^), or
    echo   2. copy an already-built project directory ^(it is
    echo      self-contained: later builds need no external store^) to:
    echo      %PROJECT%
    exit /b 1
  )
  if not exist "%TOUR%" (
    echo ERROR: the authored Sponza.tour was not found:
    echo    %TOUR%
    echo ^(the scene carries the GI volumes + env + export settings^)
    exit /b 1
  )
  echo Creating project: %PROJECT%
  echo   from glTF: %GLTF%
  "%TOOL%" init --project "%PROJECT%" --from-gltf "%GLTF%" --from-scene "%TOUR%"
  if errorlevel 1 exit /b 1
) else (
  echo Project: %PROJECT%
)

rem ---- the incremental build (reimport stale only -> Cook -> GiBaker -> Pak)
"%TOOL%" build --project "%PROJECT%" --output "%OUTPAK%"
if errorlevel 1 exit /b 1

rem ---- sync the pak next to App.exe (the demo the viewer loads) ----------
copy /y "%OUTPAK%" "%ROOT%\out\build\x64-Release\content\paks\sponza.pak"
if errorlevel 1 exit /b 1
if exist "%ROOT%\out\build\x64-Debug" copy /y "%OUTPAK%" "%ROOT%\out\build\x64-Debug\content\paks\sponza.pak" >nul

echo.
echo sponza.pak built through the project pipeline and synced.
echo Run the demo:
echo    cd /d "%ROOT%\out\build\x64-Release"
echo    App.exe --pak content/paks/sponza.pak
exit /b 0
