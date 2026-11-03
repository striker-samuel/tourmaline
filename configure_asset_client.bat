@echo off
cd /d "%~dp0"
py -3 tools\configure_asset_client.py %*
if errorlevel 1 pause
