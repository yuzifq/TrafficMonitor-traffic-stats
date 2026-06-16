@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

echo Split Traffic History
echo.
echo This launcher will split app_traffic_history.tsv into app_traffic_history\YYYY-MM-DD.tsv.
echo Please close TrafficMonitor before continuing.
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0SplitTrafficHistory.ps1" -DeleteOriginal -Force %*
set "EXIT_CODE=%ERRORLEVEL%"

echo.
if "%EXIT_CODE%"=="0" (
    echo Done.
) else (
    echo Failed. Error code: %EXIT_CODE%
    echo Check the message above. The original file is kept unless the script finished successfully.
)

if not defined SPLIT_TRAFFIC_HISTORY_NO_PAUSE pause
exit /b %EXIT_CODE%
