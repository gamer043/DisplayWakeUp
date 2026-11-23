@echo off
rem Double-click to install. Pass -Uninstall [-Restore] to remove.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
echo.
pause
