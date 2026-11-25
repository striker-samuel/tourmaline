@echo off
REM ============================================================================
REM Quick debug preset: BACKGROUND IS PURE RED, GEOMETRY LIT NORMALLY.
REM   clear color -> pure red (TOURMALINE_CLEAR_COLOR)  bg where no geometry
REM   skydome     -> OFF       (TOURMALINE_SKYDOME=0)   no skybox drawn
REM
REM   sun + GI stay ON and the sky COLOR stays authored/neutral, so the red
REM   NEVER leaks into ambient/IBL (the sky COLOR drives lighting, not the
REM   clear). This is the "clear color pure red without skybox" debug mode:
REM   the background screams red, the geometry keeps its real lighting.
REM
REM   The clear value is HDR (the target is cleared pre-tonemap + exposure),
REM   so a large value (1000,0,0) is required to saturate to pure red after
REM   the tonemap shoulder - (1,0,0) would read as a dim red.
REM
REM Anything passed on the command line is forwarded to App.exe (%*).
REM   e.g.  debug_bg_red.cmd --bench 40 --bench_move 4 --capture
REM ============================================================================
cd /d "%~dp0out\build\x64-Release"

set TOURMALINE_CLEAR_COLOR=1000,0,0
set TOURMALINE_SKYDOME=0
set TOURMALINE_BLOOM_INTENSITY=0
set TOURMALINE_CAM_POS=24.26,1.82,-9.52
set TOURMALINE_CAM_FWD=-0.947,-0.022,-0.321
set TOURMALINE_AUTO_EXPOSURE=0
set TOURMALINE_AA=off

App.exe --pak content\paks\bistro.pak %*
