param(
    [string]$BaseDir,
    [switch]$AllowRunning
)

$ErrorActionPreference = "Stop"

function Find-TrafficMonitorDirectory {
    if (-not [string]::IsNullOrWhiteSpace($BaseDir)) {
        return (Resolve-Path -LiteralPath $BaseDir).Path
    }

    $candidate = $PSScriptRoot
    while (-not [string]::IsNullOrWhiteSpace($candidate)) {
        if (Test-Path -LiteralPath (Join-Path $candidate "TrafficMonitor.exe")) {
            return $candidate
        }

        $parent = Split-Path -Parent $candidate
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $candidate) {
            break
        }
        $candidate = $parent
    }

    throw "TrafficMonitor.exe was not found above the tools directory."
}

function Get-DailyFiles {
    param([string]$Directory)

    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) {
        return @()
    }

    return @(Get-ChildItem -LiteralPath $Directory -File -Filter "*.tsv" |
        Where-Object { $_.BaseName -match '^\d{4}-\d{2}-\d{2}$' } |
        Sort-Object FullName)
}

function Test-LatestHistoryFile {
    param([System.IO.FileInfo]$File)

    $reader = [System.IO.StreamReader]::new($File.FullName, [System.Text.Encoding]::UTF8, $true)
    $currentMinute = $null
    $lastExplicitMinute = $null
    $hasCompactContinuation = $false
    $hasRepeatedExplicitMinute = $false

    try {
        while (($line = $reader.ReadLine()) -ne $null) {
            if ([string]::IsNullOrWhiteSpace($line)) {
                continue
            }

            $parts = $line.Split("`t")
            if ($parts.Count -ne 4 -or [string]::IsNullOrWhiteSpace($parts[1])) {
                return $false
            }

            if ([string]::IsNullOrEmpty($parts[0])) {
                if ($null -eq $currentMinute) {
                    return $false
                }
                $hasCompactContinuation = $true
            }
            else {
                if ($parts[0] -notmatch '^\d{2}:\d{2}$') {
                    return $false
                }
                if ($parts[0] -eq $lastExplicitMinute) {
                    $hasRepeatedExplicitMinute = $true
                }

                $currentMinute = $parts[0]
                $lastExplicitMinute = $currentMinute
            }

            [uint64]$rx = 0
            [uint64]$tx = 0
            if (-not [uint64]::TryParse($parts[2], [ref]$rx) -or
                -not [uint64]::TryParse($parts[3], [ref]$tx)) {
                return $false
            }
        }
    }
    finally {
        $reader.Dispose()
    }

    return $hasCompactContinuation -or -not $hasRepeatedExplicitMinute
}

function Test-DirectoryNeedsOptimization {
    param([string]$Directory)

    foreach ($file in Get-DailyFiles -Directory $Directory) {
        if (-not (Test-LatestHistoryFile -File $file)) {
            return $true
        }
    }
    return $false
}

function Invoke-ChildScript {
    param([string[]]$Arguments)

    & powershell.exe -NoProfile -ExecutionPolicy Bypass @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "A migration step failed with error code $LASTEXITCODE."
    }
}

try {
    $BaseDir = Find-TrafficMonitorDirectory
    $historyDirectory = Join-Path $BaseDir "app_traffic_history"
    $oldDirectory = Join-Path $historyDirectory "old"
    $legacyFile = Join-Path $BaseDir "app_traffic_history.tsv"
    $splitScript = Join-Path $PSScriptRoot "SplitTrafficHistory.ps1"
    $optimizeScript = Join-Path $PSScriptRoot "OptimizeTrafficHistory.ps1"

    $needsSplit = Test-Path -LiteralPath $legacyFile -PathType Leaf
    $needsCurrentOptimization = Test-DirectoryNeedsOptimization -Directory $historyDirectory
    $needsOldOptimization = Test-DirectoryNeedsOptimization -Directory $oldDirectory

    if (-not $needsSplit -and -not $needsCurrentOptimization -and -not $needsOldOptimization) {
        Write-Host "Latest format detected. No adjustment is required."
        exit 0
    }

    if (-not $AllowRunning -and (Get-Process TrafficMonitor -ErrorAction SilentlyContinue)) {
        throw "TrafficMonitor is running. Exit TrafficMonitor, then run this launcher again."
    }

    if ($needsSplit) {
        Write-Host "Step 1/2: Splitting the legacy history file..."
        $arguments = @(
            "-File", $splitScript,
            "-BaseDir", $BaseDir,
            "-DeleteOriginal",
            "-Force")
        if ($AllowRunning) { $arguments += "-AllowRunning" }
        Invoke-ChildScript -Arguments $arguments
        $needsCurrentOptimization = $true
    }
    else {
        Write-Host "Step 1/2: Legacy split is already complete."
    }

    if ($needsOldOptimization) {
        Write-Host "Optimizing archived daily files..."
        $arguments = @(
            "-File", $optimizeScript,
            "-Path", $oldDirectory,
            "-ReplaceOriginals")
        if ($AllowRunning) { $arguments += "-AllowRunning" }
        Invoke-ChildScript -Arguments $arguments
    }

    if ($needsCurrentOptimization) {
        Write-Host "Step 2/2: Optimizing current daily files..."
        $arguments = @(
            "-File", $optimizeScript,
            "-Path", $historyDirectory,
            "-ReplaceOriginals")
        if ($AllowRunning) { $arguments += "-AllowRunning" }
        Invoke-ChildScript -Arguments $arguments
    }
    else {
        Write-Host "Step 2/2: Current daily files are already optimized."
    }

    Write-Host "Traffic history is now in the latest format."
}
catch {
    Write-Error $_
    exit 1
}
