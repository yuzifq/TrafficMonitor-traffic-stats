$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$fixture = Join-Path $root ('build\migration-checks-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $fixture
$utf8 = [Text.UTF8Encoding]::new($false)
$source = [IO.File]::ReadAllText((Join-Path $root 'HistoryTrafficTool\UpdateTrafficHistory.cs'))
Add-Type -TypeDefinition $source

function Check([bool]$condition, [string]$message) { if (!$condition) { throw $message } }
function Write-Fixture([string]$path, [string]$text) {
    $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($path)) -Force
    [IO.File]::WriteAllText($path, $text, $utf8)
}
function Read-Base62([string]$text) {
    $digits = '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'
    [decimal]$value = 0
    foreach ($c in $text.ToCharArray()) {
        $digit = $digits.IndexOf($c)
        Check ($digit -ge 0) 'Invalid Base62 digit in migrated bytes'
        $value = $value * 62 + $digit
    }
    return [UInt64]$value
}
function Check-Case([string]$directory, [UInt64]$rx, [UInt64]$tx) {
    Check ([TrafficHistoryMigration]::Run($directory, $true) -eq 0) 'Migration failed'
    $state = [IO.File]::ReadAllLines((Join-Path $directory 'app_traffic_state.tsv'), $utf8)
    Check ($state -contains "all_time`t$rx`t$tx") 'Migrated state lost or duplicated traffic'
    [decimal]$actualRx = 0; [decimal]$actualTx = 0
    $history = Join-Path $directory 'app_traffic_history'
    foreach ($file in Get-ChildItem -LiteralPath $history -Recurse -File -Filter '*.tsv') {
        if ($file.BaseName -notmatch '^\d{4}-\d{2}-\d{2}$') { continue }
        foreach ($line in [IO.File]::ReadAllLines($file.FullName, $utf8)) {
            if (!$line) { continue }
            $fields = $line.Split([char]9)
            $actualRx += Read-Base62 $fields[2]; $actualTx += Read-Base62 $fields[3]
        }
    }
    Check ($actualRx -eq $rx -and $actualTx -eq $tx) 'Migrated raw bytes differ from the source'
    $before = @(Get-ChildItem -LiteralPath $directory -Recurse -File | Sort-Object FullName | Get-FileHash).Hash -join ','
    Check ([TrafficHistoryMigration]::Run($directory, $true) -eq 0) 'Latest-layout repeat detection failed'
    $after = @(Get-ChildItem -LiteralPath $directory -Recurse -File | Sort-Object FullName | Get-FileHash).Hash -join ','
    Check ($before -eq $after) 'Repeating migration changed already migrated history'
    Check (@(Get-ChildItem -LiteralPath $directory -Directory -Filter '.traffic_history_migration_*').Count -eq 1) 'Original backup was not retained'
}

$single = Join-Path $fixture 'single'
Write-Fixture (Join-Path $single 'app_traffic_history.tsv') "2026-10-16 14:49`tlarge.exe`t4294967296`t17`n2026-10-16 14:49`tlarge.exe`t233`t261`n"
Check-Case $single 4294967529 278
$daily = Join-Path $fixture 'daily'
Write-Fixture (Join-Path $daily 'app_traffic_history\2026-10-16.tsv') "14:49`tlarge.exe`t3221225472`t19`n`tother.exe`t120`t80`n"
Write-Fixture (Join-Path $daily 'app_traffic_history\old\2026-09-16.tsv') "14:49`told.exe`t7`t11`n"
Check-Case $daily 3221225599 110
$invalid = Join-Path $fixture 'invalid'
Write-Fixture (Join-Path $invalid 'app_traffic_history.tsv') "bad original input`n"
$original = [IO.File]::ReadAllText((Join-Path $invalid 'app_traffic_history.tsv'))
$failed = $false
try { [void][TrafficHistoryMigration]::Run($invalid, $true) } catch { $failed = $true }
Check ($failed -and [IO.File]::ReadAllText((Join-Path $invalid 'app_traffic_history.tsv')) -eq $original) 'Invalid input did not fail without changing the source'
foreach ($kind in @('single-daily-overlap', 'daily-old-overlap')) {
    $ambiguous = Join-Path $fixture $kind
    $row = "09:00`tcopy.exe`t123`t456`n"
    Write-Fixture (Join-Path $ambiguous 'app_traffic_history\2026-10-16.tsv') $row
    if ($kind -eq 'single-daily-overlap') {
        Write-Fixture (Join-Path $ambiguous 'app_traffic_history.tsv') "2026-10-16 09:00`tcopy.exe`t123`t456`n"
    }
    else { Write-Fixture (Join-Path $ambiguous 'app_traffic_history\old\2026-10-16.tsv') $row }
    $before = @(Get-ChildItem -LiteralPath $ambiguous -Recurse -File | Sort-Object FullName | Get-FileHash).Hash -join ','
    $failed = $false
    try { [void][TrafficHistoryMigration]::Run($ambiguous, $true) } catch { $failed = $true }
    $after = @(Get-ChildItem -LiteralPath $ambiguous -Recurse -File | Sort-Object FullName | Get-FileHash).Hash -join ','
    Check ($failed -and $before -eq $after) 'Ambiguous overlap was added twice or changed originals'
}
Check ([IO.File]::ReadAllText((Join-Path $root 'HistoryTrafficTool\UpdateTrafficHistory.cmd')).Contains($source)) 'Packaged CMD source differs from the tested converter'
$relocated = Join-Path $fixture 'relocated'
$null = New-Item -ItemType Directory -Path $relocated
Copy-Item -LiteralPath $single -Destination (Join-Path $relocated 'history') -Recurse
$before = @(Get-ChildItem -LiteralPath $relocated -Recurse -File | Sort-Object FullName | Get-FileHash).Hash -join ','
$savedBase = $env:TM_HISTORY_BASEDIR
$savedRunning = $env:TM_HISTORY_ALLOW_RUNNING
try {
    $env:TM_HISTORY_BASEDIR = $relocated
    $env:TM_HISTORY_ALLOW_RUNNING = '1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'HistoryTrafficTool\UpdateTrafficHistory.ps1')
    Check ($LASTEXITCODE -eq 0) 'Migration launcher did not find relocated latest-format history'
    $after = @(Get-ChildItem -LiteralPath $relocated -Recurse -File | Sort-Object FullName | Get-FileHash).Hash -join ','
    Check ($before -eq $after) 'Latest-format detection rewrote relocated data'
    Write-Fixture (Join-Path $relocated 'app_traffic_state.tsv') "root sentinel`n"
    $before = @(Get-ChildItem -LiteralPath $relocated -Recurse -File | Sort-Object FullName | Get-FileHash).Hash -join ','
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'HistoryTrafficTool\UpdateTrafficHistory.ps1')
    Check ($LASTEXITCODE -ne 0) 'Ambiguous old/new storage locations were silently chosen'
    $after = @(Get-ChildItem -LiteralPath $relocated -Recurse -File | Sort-Object FullName | Get-FileHash).Hash -join ','
    Check ($before -eq $after) 'Ambiguous storage detection changed originals'
}
finally {
    $env:TM_HISTORY_BASEDIR = $savedBase
    $env:TM_HISTORY_ALLOW_RUNNING = $savedRunning
}
Write-Output 'PASS: relocated history detected without changes; conflicting old/new locations refused'
Write-Output 'PASS: published single/daily/old formats, >2 GiB preservation, exact Base62 totals, backup, repeat detection, ambiguous-overlap refusal and packaged source parity'
