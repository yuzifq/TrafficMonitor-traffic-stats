@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

echo Update Traffic History
echo.
echo This launcher automatically detects and applies the required migration step.
echo TrafficMonitor must be closed when an adjustment is required.
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0UpdateTrafficHistory.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"

echo.
if not "%EXIT_CODE%"=="0" (
    echo Failed. Error code: %EXIT_CODE%
    echo Check the message above. Existing history files were kept when an operation failed.
)

echo.
pause
exit /b %EXIT_CODE%
