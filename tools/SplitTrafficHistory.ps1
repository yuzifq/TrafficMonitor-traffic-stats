param(
    [string]$BaseDir,
    [switch]$DeleteOriginal,
    [switch]$AllowRunning,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($BaseDir)) {
    $candidateDirs = @()
    $candidateDir = $PSScriptRoot
    while (-not [string]::IsNullOrWhiteSpace($candidateDir)) {
        $candidateDirs += $candidateDir
        $parentDir = Split-Path -Parent $candidateDir
        if ([string]::IsNullOrWhiteSpace($parentDir) -or $parentDir -eq $candidateDir) {
            break
        }
        $candidateDir = $parentDir
    }

    $BaseDir = $candidateDirs |
        Where-Object { Test-Path -LiteralPath (Join-Path $_ "TrafficMonitor.exe") } |
        Select-Object -First 1

    if ([string]::IsNullOrWhiteSpace($BaseDir)) {
        $BaseDir = $candidateDirs |
        Where-Object { Test-Path -LiteralPath (Join-Path $_ "app_traffic_history.tsv") } |
        Select-Object -First 1
    }

    if ([string]::IsNullOrWhiteSpace($BaseDir)) {
        $BaseDir = $PSScriptRoot
    }
}

$sourcePath = Join-Path $BaseDir "app_traffic_history.tsv"
$targetDir = Join-Path $BaseDir "app_traffic_history"
$tempDir = Join-Path $BaseDir "app_traffic_history_migrating"

if (-not $AllowRunning -and (Get-Process TrafficMonitor -ErrorAction SilentlyContinue)) {
    throw "TrafficMonitor is running. Exit TrafficMonitor first, or rerun with -AllowRunning if you know the file is not being written."
}

if (-not (Test-Path -LiteralPath $sourcePath)) {
    throw "Source file not found: $sourcePath"
}

if ((Test-Path -LiteralPath $tempDir) -and -not $Force) {
    throw "Temp folder already exists: $tempDir. Remove it or rerun with -Force."
}

if (Test-Path -LiteralPath $tempDir) {
    Remove-Item -LiteralPath $tempDir -Recurse -Force
}

New-Item -ItemType Directory -Path $tempDir | Out-Null

$reader = [System.IO.StreamReader]::new($sourcePath, [System.Text.Encoding]::UTF8, $true)
$writers = @{}
$totalLines = 0
$writtenLines = 0
$badLines = 0

try {
    while (($line = $reader.ReadLine()) -ne $null) {
        $totalLines++
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }

        $parts = $line -split "`t", 4
        if ($parts.Count -ne 4 -or $parts[0].Length -lt 16) {
            $badLines++
            continue
        }

        $dateKey = $parts[0].Substring(0, 10)
        $timeKey = $parts[0].Substring(11, 5)
        if ($dateKey -notmatch '^\d{4}-\d{2}-\d{2}$' -or $timeKey -notmatch '^\d{2}:\d{2}$') {
            $badLines++
            continue
        }

        if (-not $writers.ContainsKey($dateKey)) {
            $dayPath = Join-Path $tempDir "$dateKey.tsv"
            $writers[$dateKey] = [System.IO.StreamWriter]::new($dayPath, $true, [System.Text.UTF8Encoding]::new($false))
        }

        $writers[$dateKey].WriteLine("{0}`t{1}`t{2}`t{3}", $timeKey, $parts[1], $parts[2], $parts[3])
        $writtenLines++
    }
}
finally {
    $reader.Dispose()
    foreach ($writer in $writers.Values) {
        $writer.Dispose()
    }
}

if ($writtenLines -eq 0) {
    Remove-Item -LiteralPath $tempDir -Recurse -Force
    throw "No valid rows were written. The source file was left untouched."
}

if (-not (Test-Path -LiteralPath $targetDir)) {
    New-Item -ItemType Directory -Path $targetDir | Out-Null
}

foreach ($file in Get-ChildItem -LiteralPath $tempDir -Filter "*.tsv") {
    $destination = Join-Path $targetDir $file.Name
    if ((Test-Path -LiteralPath $destination) -and -not $Force) {
        throw "Target file already exists: $destination. Rerun with -Force to append migrated rows."
    }

    if (Test-Path -LiteralPath $destination) {
        $inputStream = [System.IO.StreamReader]::new($file.FullName, [System.Text.Encoding]::UTF8, $true)
        $outputStream = [System.IO.StreamWriter]::new($destination, $true, [System.Text.UTF8Encoding]::new($false))
        try {
            while (($line = $inputStream.ReadLine()) -ne $null) {
                $outputStream.WriteLine($line)
            }
        }
        finally {
            $inputStream.Dispose()
            $outputStream.Dispose()
        }
        Remove-Item -LiteralPath $file.FullName -Force
    }
    else {
        Move-Item -LiteralPath $file.FullName -Destination $destination
    }
}

Remove-Item -LiteralPath $tempDir -Recurse -Force

if ($DeleteOriginal) {
    Remove-Item -LiteralPath $sourcePath -Force
}

[PSCustomObject]@{
    Source = $sourcePath
    Target = $targetDir
    TotalLines = $totalLines
    WrittenLines = $writtenLines
    SkippedBadLines = $badLines
    DeletedOriginal = [bool]$DeleteOriginal
}
