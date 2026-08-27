$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$mainPath = Join-Path $PSScriptRoot 'UpdateTrafficHistory.ps1'
$sourcePath = Join-Path $PSScriptRoot 'UpdateTrafficHistory.cs'
$outputPath = Join-Path $root 'tools\UpdateTrafficHistory.cmd'

$header = @'
@echo off
setlocal
chcp 65001 >nul
set "TM_HISTORY_TOOL=%~f0"
title Update Traffic History

echo Update Traffic History
echo.
echo This tool directly converts all published GitHub history formats to the latest format.
echo Already updated history is detected automatically and left unchanged.
echo TrafficMonitor must be closed only when an adjustment is required.
echo.
powershell.exe -NoProfile -Command "Write-Host (-join [char[]](0x6CE8,0x610F,0xFF1A,0x4F7F,0x7528,0x524D,0x52A1,0x5FC5,0x81EA,0x884C,0x5907,0x4EFD,0x3002))"
echo NOTICE: You must back up your traffic history before use.
echo.
set "CONFIRM=Y"
if not "%TM_HISTORY_ASSUME_YES%"=="1" set "CONFIRM="
if not "%TM_HISTORY_ASSUME_YES%"=="1" powershell.exe -NoProfile -Command "Write-Host (-join [char[]](0x8F93,0x5165,0x20,0x59,0x20,0x8FDB,0x5165,0x4E0B,0x4E00,0x9636,0x6BB5,0x3002))"
if not "%TM_HISTORY_ASSUME_YES%"=="1" set /p "CONFIRM=Enter Y to continue: "
if /I not "%CONFIRM%"=="Y" (
    echo.
    powershell.exe -NoProfile -Command "Write-Host (-join [char[]](0x5DF2,0x53D6,0x6D88,0xFF0C,0x672A,0x4FEE,0x6539,0x4EFB,0x4F55,0x6587,0x4EF6,0x3002))"
    echo Cancelled. No files were changed.
    echo.
    if not "%TM_HISTORY_NO_PAUSE%"=="1" pause
    exit /b 0
)

echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$raw=[IO.File]::ReadAllText($env:TM_HISTORY_TOOL); $marker='# POWERSHELL_PAYLOAD'; $start=$raw.LastIndexOf($marker); if($start -lt 0){throw 'Embedded migration payload was not found.'}; & ([scriptblock]::Create($raw.Substring($start+$marker.Length)))"
set "EXIT_CODE=%ERRORLEVEL%"

echo.
if not "%EXIT_CODE%"=="0" (
    echo Failed. Error code: %EXIT_CODE%
    echo Existing history files were kept when an operation failed.
)
echo.
if not "%TM_HISTORY_NO_PAUSE%"=="1" pause
exit /b %EXIT_CODE%

# POWERSHELL_PAYLOAD
'@

$main = [System.IO.File]::ReadAllText($mainPath)
$source = [System.IO.File]::ReadAllText($sourcePath)
$embeddedSource = "`$script:embeddedMigrationSource = @'`r`n" + $source + "`r`n'@`r`n"
$content = $header + "`r`n" + $embeddedSource + $main
[System.IO.File]::WriteAllText($outputPath, $content, [System.Text.UTF8Encoding]::new($false))
Write-Host "Built $outputPath"
