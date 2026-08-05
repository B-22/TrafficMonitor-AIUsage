@echo off
rem ============================================================
rem  One-click deploy launcher (self-elevating, window stays open)
rem  Double-click this file. UAC prompt appears once, click Yes.
rem ============================================================

rem Elevation check: net session succeeds only when running as admin
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [*] Requesting administrator privileges (UAC)...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo [*] Running as administrator. Deploying...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy-local.ps1"

echo.
echo [*] Finished. Press any key to close this window.
pause >nul
