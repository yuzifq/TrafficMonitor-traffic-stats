[CmdletBinding()]
# Offline reset with checked version-5/6 checkpoints. Without -Apply this is read-only.
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$Date,
    [switch]$Apply,
    [switch]$ResetOfficial
)

$ErrorActionPreference = 'Stop'
$rootPath = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
$day = [datetime]::ParseExact($Date, 'yyyy-MM-dd', [Globalization.CultureInfo]::InvariantCulture)
if ($day.ToString('yyyy-MM-dd') -ne $Date) { throw 'Invalid date' }
if (Get-Process -Name TrafficMonitor -ErrorAction SilentlyContinue) { throw 'Exit TrafficMonitor first' }
if (-not (Test-Path -LiteralPath (Join-Path $rootPath 'TrafficMonitor.exe'))) { throw 'TrafficMonitor.exe not found' }
$utf8 = [Text.UTF8Encoding]::new($false, $true)
$alphabet = '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'
function Read-Base62([string]$Text) {
    if (-not $Text -or $Text -notmatch '^[0-9a-zA-Z]+$') { throw 'Invalid Base62 field' }
    [Numerics.BigInteger]$value = 0
    foreach ($c in $Text.ToCharArray()) { $value = $value * 62 + $alphabet.IndexOf($c) }
    if ($value -gt [uint64]::MaxValue) { throw 'Base62 overflow' }
    return $value
}
function Read-Text([string]$Path) { return [IO.File]::ReadAllText($Path, $utf8) }
function Metadata-Hash([string]$Text) {
    [Numerics.BigInteger]$hash = 14695981039346656037
    [Numerics.BigInteger]$mask = [uint64]::MaxValue
    foreach ($c in $Text.ToCharArray()) { $hash = (($hash -bxor ([int]$c -band 255)) * 1099511628211) -band $mask }
    return $hash
}
function Safe-Path([string]$Relative) {
    $path = [IO.Path]::GetFullPath((Join-Path $rootPath $Relative))
    if (-not $path.StartsWith($rootPath + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Path outside target directory' }
    return $path
}
$relativeMonth = Join-Path $day.ToString('yyyy') $day.ToString('MM')
$relativeDay = Join-Path $relativeMonth $Date
$rawPath = Safe-Path ('app_traffic_history\' + $relativeDay + '.tsv')
if (-not (Test-Path -LiteralPath $rawPath -PathType Leaf)) { throw 'No daily process history; nothing has been changed' }
$ids = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($line in ((Read-Text (Safe-Path 'app_traffic_history\apps.tsv')) -split '\r?\n')) {
    if (-not $line -or $line -eq "version`t4") { continue }
    $fields = $line.Split("`t")
    if ($fields.Length -lt 3 -or $fields[0] -ne 'app' -or -not $ids.Add($fields[1])) { throw 'Invalid application dictionary' }
}
$targets = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
[Numerics.BigInteger]$rx = 0
[Numerics.BigInteger]$tx = 0
$time = ''
foreach ($line in ((Read-Text $rawPath) -split '\r?\n')) {
    if (-not $line) { continue }
    $fields = $line.Split("`t")
    if ($fields.Length -ne 4) { throw 'Invalid daily history row' }
    if ($fields[0]) { $time = $fields[0] }
    if ($time -notmatch '^(?:[01]\d|2[0-3]):[0-5]\d$') { throw 'Invalid compact minute' }
    $null = Read-Base62 $fields[1]
    if (-not $ids.Contains($fields[1])) { throw 'Unknown application ID' }
    $rx += Read-Base62 $fields[2]
    $tx += Read-Base62 $fields[3]
}
foreach ($relative in @('app_traffic_history', 'network_traffic_history')) {
    $directory = Safe-Path (Join-Path $relative $relativeMonth)
    if (Test-Path -LiteralPath $directory) {
        foreach ($file in (Get-ChildItem -LiteralPath $directory -File -Filter ($Date + '.*'))) { $null = $targets.Add($file.FullName) }
    }
}
$statePath = Safe-Path 'app_traffic_state.tsv'
$state = Read-Text $statePath
$allTime = [regex]::Matches($state, '(?m)^all_time\t(\d+)\t(\d+)\r?$')
if ($allTime.Count -ne 1) { throw 'Expected one cumulative state row' }
[Numerics.BigInteger]$allRx = [Numerics.BigInteger]::Parse($allTime[0].Groups[1].Value)
[Numerics.BigInteger]$allTx = [Numerics.BigInteger]::Parse($allTime[0].Groups[2].Value)
$commitPath = Safe-Path 'app_traffic_history\current.commit'
$checkpointPath = Safe-Path 'app_traffic_history\current.tmp'
$committed = if (Test-Path -LiteralPath $commitPath) { (Read-Text $commitPath).Trim() } else { '' }
$commitFields = $committed.Split("`t")
if ($commitFields.Length -gt 1) {
    if ($commitFields.Length -ne 3 -or $commitFields[1] -notmatch '^\d+$' -or $commitFields[2] -notmatch '^\d+$' -or
        (Metadata-Hash ($commitFields[0] + "`t" + $commitFields[1])) -ne [Numerics.BigInteger]::Parse($commitFields[2])) { throw 'Invalid commit marker checksum' }
}
if (Test-Path -LiteralPath $checkpointPath) {
    $checkpoint = Read-Text $checkpointPath
    $version = [regex]::Match($checkpoint, '(?m)^version\t([56])\r?$')
    if (-not $version.Success) { throw 'Unsupported checkpoint version' }
    $committedCheckpoint = $false
    if ($version.Groups[1].Value -eq '6') {
        [Numerics.BigInteger]$checkpointHash = 14695981039346656037
        [Numerics.BigInteger]$mask = [uint64]::MaxValue
        foreach ($line in ($checkpoint -split '\r?\n')) {
            if ($line -match '^(version|transaction|minute|all_time|entry)\t') {
                $checkpointHash = (($checkpointHash -bxor (Metadata-Hash $line)) * 1099511628211) -band $mask
            }
        }
        $checksum = [regex]::Matches($checkpoint, '(?m)^checksum\t(\d+)\r?$')
        $transaction = [regex]::Matches($checkpoint, '(?m)^transaction\t(\d+)\r?$')
        if ($checksum.Count -ne 1 -or $checkpointHash -ne [Numerics.BigInteger]::Parse($checksum[0].Groups[1].Value) -or
            $transaction.Count -ne 1 -or [Numerics.BigInteger]::Parse($transaction[0].Groups[1].Value) -le 0) { throw 'Invalid v6 checkpoint checksum or transaction' }
        if ($committed -and $commitFields.Length -ne 3) { throw 'V6 checkpoint requires a checksummed commit marker' }
        $committedCheckpoint = $commitFields.Length -eq 3 -and $commitFields[1] -eq $transaction[0].Groups[1].Value
    }
    $minuteMatch = [regex]::Match($checkpoint, '(?m)^minute\t([^\r\n]+)\r?$')
    if (-not $minuteMatch.Success -or -not $minuteMatch.Groups[1].Value.StartsWith($Date + ' ')) { throw 'Checkpoint belongs to another day' }
    $checkpointAll = [regex]::Match($checkpoint, '(?m)^all_time\t(\d+)\t(\d+)\r?$')
    if (-not $checkpointAll.Success -or [Numerics.BigInteger]::Parse($checkpointAll.Groups[1].Value) -ne $allRx -or
        [Numerics.BigInteger]::Parse($checkpointAll.Groups[2].Value) -ne $allTx) { throw 'Cumulative state and checkpoint disagree' }
    $alreadyCommitted = if ($version.Groups[1].Value -eq '6') {
        $committedCheckpoint -and $commitFields[0] -eq $minuteMatch.Groups[1].Value
    } else { $committed -eq $minuteMatch.Groups[1].Value }
    if (-not $alreadyCommitted) {
        foreach ($line in ($checkpoint -split '\r?\n')) {
            $fields = $line.Split("`t")
            if ($fields[0] -eq 'entry') {
                if ($fields.Length -ne 4) { throw 'Invalid checkpoint entry' }
                $null = Read-Base62 $fields[1]
                if (-not $ids.Contains($fields[1])) { throw 'Unknown checkpoint application ID' }
                $rx += Read-Base62 $fields[2]; $tx += Read-Base62 $fields[3]
            }
        }
    }
    $null = $targets.Add($checkpointPath)
}
if ($committed.StartsWith($Date + ' ')) { $null = $targets.Add($commitPath) }
$appendPath = Safe-Path 'app_traffic_history\append.pending'
if (Test-Path -LiteralPath $appendPath) { throw 'Recover the pending append before resetting history' }
$networkPending = Safe-Path 'network_traffic_history\pending.tsv'
if (Test-Path -LiteralPath $networkPending) {
    $rows = @((Read-Text $networkPending) -split '\r?\n' | Where-Object { $_ })
    if ($rows[0] -ne "network`t1") { throw 'Invalid network checkpoint' }
    foreach ($line in ($rows | Select-Object -Skip 1)) {
        $fields = $line.Split("`t")
        if ($fields.Length -ne 8) { throw 'Invalid network checkpoint record' }
        $end = [datetime]::FromFileTimeUtc(([long]$fields[2] - 1) * 10000).ToLocalTime()
        if ($end.ToString('yyyy-MM-dd') -ne $Date) { throw 'Network checkpoint belongs to another day' }
    }
    $null = $targets.Add($networkPending)
}
foreach ($file in (Get-ChildItem -LiteralPath (Safe-Path 'app_traffic_history') -Recurse -File)) {
    if ($file.Name -notmatch '\.(day|week|month|quarter)\.tsv$') { continue }
    $summary = Read-Text $file.FullName
    $start = [regex]::Match($summary, '(?m)^start\t(\d{4}-\d{2}-\d{2})\r?$')
    $end = [regex]::Match($summary, '(?m)^end\t(\d{4}-\d{2}-\d{2})\r?$')
    if ($start.Success -and $end.Success -and $start.Groups[1].Value -le $Date -and $end.Groups[1].Value -ge $Date) {
        $null = $targets.Add($file.FullName)
    }
}
if ($rx -gt $allRx -or $tx -gt $allTx) { throw 'Daily traffic exceeds cumulative state' }
$newState = [regex]::Replace($state, '(?m)^all_time\t\d+\t\d+\r?$', ('all_time' + "`t" + ($allRx - $rx) + "`t" + ($allTx - $tx)))
$newState = [regex]::Replace($newState, '(?m)^range_start\t[^\r\n]+\r?$', ('range_start' + "`t" + $Date + ' 00:00'))
$endTime = if ($day.Date -eq (Get-Date).Date) { (Get-Date).ToString('yyyy-MM-dd HH:mm') } else { $Date + ' 23:59' }
$newState = [regex]::Replace($newState, '(?m)^range_end\t[^\r\n]+\r?$', ('range_end' + "`t" + $endTime))
$replacements = @{ $statePath = $newState }
if ($ResetOfficial) {
    foreach ($relative in @('history_traffic.dat', 'history_traffic.dat.bak')) {
        $path = Safe-Path $relative
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $text = Read-Text $path
        $pattern = '(?m)^' + [regex]::Escape($Date.Replace('-', '/')) + ' \d+/\d+\r?$'
        if ([regex]::Matches($text, $pattern).Count -ne 1) { throw 'Expected one official daily record' }
        $replacements[$path] = [regex]::Replace($text, $pattern, ($Date.Replace('-', '/') + ' 0/0'))
    }
}
[pscustomobject]@{ Date = $Date; RemovedRx = "$rx"; RemovedTx = "$tx"; RemainingRx = "$($allRx - $rx)";
    RemainingTx = "$($allTx - $tx)"; ResetOfficial = [bool]$ResetOfficial; FilesToArchive = @($targets); Applied = [bool]$Apply } | ConvertTo-Json -Depth 3
if (-not $Apply) { return }
$backup = Safe-Path ('traffic_day_reset_' + $day.ToString('yyyyMMdd') + '_' + (Get-Date).ToString('yyyyMMdd_HHmmss_fff'))
$null = New-Item -ItemType Directory -Path $backup
$affected = @($targets) + @($replacements.Keys)
$manifest = foreach ($path in $affected) {
    $relative = [IO.Path]::GetRelativePath($rootPath, $path)
    $copy = Join-Path $backup $relative
    $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($copy)) -Force
    Copy-Item -LiteralPath $path -Destination $copy
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ((Get-FileHash -LiteralPath $copy -Algorithm SHA256).Hash -ne $hash) { throw 'Backup verification failed' }
    [pscustomobject]@{ RelativePath = $relative; SHA256 = $hash }
}
[IO.File]::WriteAllText((Join-Path $backup 'manifest.json'), ($manifest | ConvertTo-Json), $utf8)
try {
    if (Get-Process -Name TrafficMonitor -ErrorAction SilentlyContinue) { throw 'TrafficMonitor restarted during reset' }
    foreach ($path in $replacements.Keys) {
        [IO.File]::WriteAllText($path + '.reset.new', $replacements[$path], $utf8)
        [IO.File]::Replace($path + '.reset.new', $path, [NullString]::Value)
    }
    foreach ($path in $targets) {
        $archived = Join-Path $backup ('removed\' + [IO.Path]::GetRelativePath($rootPath, $path))
        $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($archived)) -Force
        Move-Item -LiteralPath $path -Destination $archived
    }
} catch {
    foreach ($item in $manifest) { Copy-Item -LiteralPath (Join-Path $backup $item.RelativePath) -Destination (Safe-Path $item.RelativePath) -Force }
    foreach ($path in $replacements.Keys) { [IO.File]::Delete($path + '.reset.new') }
    throw
}
Write-Output ('Reset completed; verified rollback backup: ' + $backup)
