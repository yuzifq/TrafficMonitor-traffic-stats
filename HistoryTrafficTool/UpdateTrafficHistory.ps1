$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

function Test-HistoryDataDirectory([string]$directory) {
    foreach ($name in @('app_traffic_history', 'app_traffic_history.tsv', 'app_traffic_state.tsv', 'network_traffic_history')) {
        if (Test-Path -LiteralPath (Join-Path $directory $name)) { return $true }
    }
    return $false
}

function Find-TrafficMonitorDirectory {
    if (-not [string]::IsNullOrWhiteSpace($env:TM_HISTORY_BASEDIR)) {
        return (Resolve-Path -LiteralPath $env:TM_HISTORY_BASEDIR).Path
    }

    $candidate = if (-not [string]::IsNullOrWhiteSpace($env:TM_HISTORY_TOOL)) {
        Split-Path -Parent $env:TM_HISTORY_TOOL
    }
    else {
        $PSScriptRoot
    }
    $historyCandidate = $null
    while (-not [string]::IsNullOrWhiteSpace($candidate)) {
        if (Test-Path -LiteralPath (Join-Path $candidate 'TrafficMonitor.exe') -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
        if ($null -eq $historyCandidate -and
            ((Test-HistoryDataDirectory $candidate) -or (Test-HistoryDataDirectory (Join-Path $candidate 'history')))) {
            $historyCandidate = (Resolve-Path -LiteralPath $candidate).Path
        }
        $parent = Split-Path -Parent $candidate
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $candidate) { break }
        $candidate = $parent
    }
    if ($null -ne $historyCandidate) { return $historyCandidate }
    throw 'TrafficMonitor.exe or a published traffic-history layout was not found above the tools directory.'
}

try {
    $baseDirectory = Find-TrafficMonitorDirectory
    if (-not (Get-Variable -Name embeddedMigrationSource -Scope Script -ErrorAction SilentlyContinue)) {
        $sourcePath = Join-Path $PSScriptRoot 'UpdateTrafficHistory.cs'
        $script:embeddedMigrationSource = [System.IO.File]::ReadAllText($sourcePath)
    }
    Add-Type -TypeDefinition $script:embeddedMigrationSource -Language CSharp
    $allowRunning = $env:TM_HISTORY_ALLOW_RUNNING -eq '1'
    exit [TrafficHistoryMigration]::Upgrade($baseDirectory, $allowRunning)
}
catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host 'Upgrade stopped. Keep the original files and safety backups; review the error above before retrying.'
    exit 1
}
