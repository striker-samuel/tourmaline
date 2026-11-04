@echo off
cd /d "%~dp0"
py -3 tools\setup_wizard.py %*
if errorlevel 1 pause
