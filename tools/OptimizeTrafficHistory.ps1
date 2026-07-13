param(
    [Parameter(Position = 0)]
    [string[]]$Path,

    [string]$OutputDirectory,

    [string]$BaseDir,

    [switch]$ReplaceOriginals,

    [switch]$AllowRunning
)

$ErrorActionPreference = "Stop"

function Get-InputFiles {
    param([string[]]$InputPaths)

    if (-not $InputPaths -or $InputPaths.Count -eq 0) {
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
                    Where-Object { Test-Path -LiteralPath (Join-Path $_ "app_traffic_history") } |
                    Select-Object -First 1
            }

            if ([string]::IsNullOrWhiteSpace($BaseDir)) {
                $BaseDir = $PSScriptRoot
            }
        }

        $historyDir = Join-Path $BaseDir "app_traffic_history"
        if (Test-Path -LiteralPath $historyDir) {
            $InputPaths = @($historyDir)
        }
        else {
            $InputPaths = @($BaseDir)
        }
    }

    $files = foreach ($inputPath in $InputPaths) {
        $resolved = (Resolve-Path -LiteralPath $inputPath).Path
        if (Test-Path -LiteralPath $resolved -PathType Container) {
            Get-ChildItem -LiteralPath $resolved -File -Filter "*.tsv" |
                Where-Object { $_.BaseName -match '^\d{4}-\d{2}-\d{2}$' }
        }
        else {
            Get-Item -LiteralPath $resolved
        }
    }

    return @($files | Sort-Object FullName -Unique)
}

function Optimize-TrafficHistoryFile {
    param(
        [System.IO.FileInfo]$InputFile,
        [string]$DestinationDirectory
    )

    if ($InputFile.Extension -ne ".tsv") {
        throw "Input file must be a TSV file: $($InputFile.FullName)"
    }

    if ([string]::IsNullOrWhiteSpace($DestinationDirectory)) {
        $DestinationDirectory = Join-Path $InputFile.DirectoryName "optimized"
    }

    [System.IO.Directory]::CreateDirectory($DestinationDirectory) | Out-Null
    $outputPath = Join-Path $DestinationDirectory $InputFile.Name
    $temporaryPath = "$outputPath.tmp"

    $reader = [System.IO.StreamReader]::new($InputFile.FullName, [System.Text.Encoding]::UTF8, $true)
    $writer = [System.IO.StreamWriter]::new($temporaryPath, $false, [System.Text.UTF8Encoding]::new($false))
    $currentMinute = $null
    $minuteTotals = [ordered]@{}
    [long]$inputLines = 0
    $stats = [PSCustomObject]@{
        OutputLines = [long]0
        RepairedLines = [long]0
    }
    $completed = $false

    $flushMinute = {
        if ($null -eq $currentMinute) {
            return
        }

        $showTime = $true
        foreach ($entry in $minuteTotals.GetEnumerator()) {
            $timeText = if ($showTime) { $currentMinute } else { "" }
            $writer.WriteLine(
                "{0}`t{1}`t{2}`t{3}",
                $timeText,
                $entry.Key,
                $entry.Value.Rx,
                $entry.Value.Tx)
            $showTime = $false
            $stats.OutputLines++
        }
    }

    try {
        while (($line = $reader.ReadLine()) -ne $null) {
            if ([string]::IsNullOrWhiteSpace($line)) {
                continue
            }

            $inputLines++
            $parts = $line.Split("`t")
            if ($parts.Count -lt 4) {
                throw "Invalid row at line ${inputLines}: $line"
            }

            $timeFieldCount = $parts.Count - 3
            $minute = $null
            for ($index = 0; $index -lt $timeFieldCount; $index++) {
                if ($parts[$index] -match '^(?:\d{4}-\d{2}-\d{2} )?(\d{2}:\d{2})$') {
                    $minute = $Matches[1]
                }
            }

            if ($null -eq $minute -and $parts.Count -eq 4 -and
                [string]::IsNullOrEmpty($parts[0]) -and $null -ne $currentMinute) {
                $minute = $currentMinute
            }

            $appName = $parts[$parts.Count - 3]
            if ($null -eq $minute -or [string]::IsNullOrWhiteSpace($appName)) {
                throw "Invalid row at line ${inputLines}: $line"
            }

            [uint64]$rx = 0
            [uint64]$tx = 0
            if (-not [uint64]::TryParse($parts[$parts.Count - 2], [ref]$rx) -or
                -not [uint64]::TryParse($parts[$parts.Count - 1], [ref]$tx)) {
                throw "Invalid traffic value at line ${inputLines}: $line"
            }

            if ($parts.Count -ne 4 -or $parts[0] -ne $minute) {
                $stats.RepairedLines++
            }
            if ($null -ne $currentMinute -and $minute -ne $currentMinute) {
                & $flushMinute
                $minuteTotals.Clear()
            }
            $currentMinute = $minute

            if (-not $minuteTotals.Contains($appName)) {
                $minuteTotals[$appName] = [PSCustomObject]@{ Rx = [uint64]0; Tx = [uint64]0 }
            }
            $minuteTotals[$appName].Rx += $rx
            $minuteTotals[$appName].Tx += $tx
        }

        & $flushMinute
        $completed = $true
    }
    finally {
        $reader.Dispose()
        $writer.Dispose()
        if (-not $completed -and (Test-Path -LiteralPath $temporaryPath)) {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }

    Move-Item -LiteralPath $temporaryPath -Destination $outputPath -Force
    [PSCustomObject]@{
        Source = $InputFile.FullName
        Output = $outputPath
        InputLines = $inputLines
        OutputLines = $stats.OutputLines
        SavedLines = $inputLines - $stats.OutputLines
        RepairedLines = $stats.RepairedLines
        OriginalBytes = $InputFile.Length
        OptimizedBytes = (Get-Item -LiteralPath $outputPath).Length
    }
}

function Replace-HistoryDirectory {
    param([string]$HistoryDirectory)

    $historyPath = [System.IO.Path]::GetFullPath($HistoryDirectory)
    $parentDirectory = Split-Path -Parent $historyPath
    $historyName = Split-Path -Leaf $historyPath
    $optimizedPath = Join-Path $historyPath "optimized"
    $readyPath = Join-Path $parentDirectory "${historyName}_optimized_ready"
    $backupPath = Join-Path $parentDirectory "${historyName}_original_backup"

    if (-not (Test-Path -LiteralPath $optimizedPath -PathType Container)) {
        throw "Optimized directory not found: $optimizedPath"
    }
    if ((Test-Path -LiteralPath $readyPath) -or (Test-Path -LiteralPath $backupPath)) {
        throw "A previous replacement staging folder exists. Remove it before retrying: $readyPath or $backupPath"
    }
    if (-not $AllowRunning -and (Get-Process TrafficMonitor -ErrorAction SilentlyContinue)) {
        throw "TrafficMonitor started while optimization was running. Exit TrafficMonitor, then run the script again. Original files were not replaced."
    }

    Get-ChildItem -LiteralPath $historyPath -Force |
        Where-Object {
            $_.Name -ne "optimized" -and
            -not ($_.PSIsContainer -eq $false -and $_.Name -match '^\d{4}-\d{2}-\d{2}\.tsv$')
        } |
        ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $optimizedPath -Recurse -Force
        }

    Move-Item -LiteralPath $optimizedPath -Destination $readyPath
    Move-Item -LiteralPath $historyPath -Destination $backupPath
    try {
        Move-Item -LiteralPath $readyPath -Destination $historyPath
        $newFileCount = (Get-ChildItem -LiteralPath $historyPath -File -Filter "*.tsv").Count
        if ($newFileCount -eq 0) {
            throw "Replacement verification failed: no TSV files were found in $historyPath"
        }
    }
    catch {
        if (Test-Path -LiteralPath $historyPath) {
            Move-Item -LiteralPath $historyPath -Destination $readyPath -Force
        }
        Move-Item -LiteralPath $backupPath -Destination $historyPath -Force
        if (Test-Path -LiteralPath $readyPath) {
            Move-Item -LiteralPath $readyPath -Destination (Join-Path $historyPath "optimized") -Force
        }
        throw
    }

    Remove-Item -LiteralPath $backupPath -Recurse -Force
    [PSCustomObject]@{
        ReplacedHistoryDirectory = $historyPath
        FileCount = $newFileCount
        OriginalsReplaced = $true
        OptimizedFolderRemoved = -not (Test-Path -LiteralPath (Join-Path $historyPath "optimized"))
    }
}

try {
    $inputFiles = Get-InputFiles -InputPaths $Path
    if ($inputFiles.Count -eq 0) {
        throw "No YYYY-MM-DD.tsv files were found."
    }

    $historyDirectory = $null
    if ($ReplaceOriginals) {
        if ($OutputDirectory) {
            throw "Do not use -OutputDirectory together with -ReplaceOriginals."
        }
        if (-not $AllowRunning -and (Get-Process TrafficMonitor -ErrorAction SilentlyContinue)) {
            throw "TrafficMonitor is running. Exit TrafficMonitor before optimizing and replacing history files."
        }

        $sourceDirectories = @($inputFiles | ForEach-Object { $_.DirectoryName } | Sort-Object -Unique)
        if ($sourceDirectories.Count -ne 1) {
            throw "Replacement requires all source files to be in one history directory."
        }
        $historyDirectory = $sourceDirectories[0]
        $allDailyFiles = @(Get-ChildItem -LiteralPath $historyDirectory -File -Filter "*.tsv" |
            Where-Object { $_.BaseName -match '^\d{4}-\d{2}-\d{2}$' })
        if ($allDailyFiles.Count -ne $inputFiles.Count) {
            throw "Replacement requires processing every YYYY-MM-DD.tsv file in the history directory."
        }
    }

    foreach ($inputFile in $inputFiles) {
        $destination = if ($OutputDirectory) { $OutputDirectory } else { Join-Path $inputFile.DirectoryName "optimized" }
        Optimize-TrafficHistoryFile -InputFile $inputFile -DestinationDirectory $destination
    }

    if ($ReplaceOriginals) {
        Replace-HistoryDirectory -HistoryDirectory $historyDirectory
    }
}
catch {
    Write-Error $_
    exit 1
}
