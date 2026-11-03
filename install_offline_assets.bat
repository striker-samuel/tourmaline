@echo off
cd /d "%~dp0"
py -3 tools\install_offline_assets.py %*
if errorlevel 1 pause
