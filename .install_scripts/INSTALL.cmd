@echo off
>nul 2>&1 net session && goto :run
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
exit /b
:run
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-drivers.ps1"
echo.
pause
