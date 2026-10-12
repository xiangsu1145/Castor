@echo off
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1"
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build failed.
    pause
    exit /b 1
)
echo [SUCCESS] Output: "%~dp0build\CastorDesktop.exe"
pause
