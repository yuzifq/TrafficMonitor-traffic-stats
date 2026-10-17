$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('TrafficHistoryLayout-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $fixture
$utf8 = [Text.UTF8Encoding]::new($false)
$source = [IO.File]::ReadAllText((Join-Path $root 'HistoryTrafficTool\UpdateTrafficHistory.cs'))
Add-Type -TypeDefinition $source

function Check([bool]$condition, [string]$message) { if (!$condition) { throw $message } }
function Write-Fixture([string]$path, [string]$text) {
    $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($path)) -Force
    [IO.File]::WriteAllText($path, $text, $utf8)
}
function Snapshot-Once([string]$directory) {
    $prefix = [IO.Path]::GetFullPath($directory).TrimEnd('\') + '\'
    $entries = [Collections.Generic.List[string]]::new()
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($directory)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        while ($pending.Count) {
            foreach ($path in [IO.Directory]::GetFileSystemEntries($pending.Pop())) {
                $relative = $path.Substring($prefix.Length)
                if ([IO.Directory]::Exists($path)) {
                    $entries.Add("D|$relative")
                    $pending.Push($path)
                }
                else {
                    $input = [IO.File]::OpenRead($path)
                    try {
                        $hash = [Convert]::ToBase64String($sha.ComputeHash($input))
                        $entries.Add("F|$relative|$($input.Length)|$([IO.File]::GetLastWriteTimeUtc($path).Ticks)|$hash")
                    }
                    finally { $input.Dispose() }
                }
            }
        }
    }
    finally { $sha.Dispose() }
    return (@($entries | Sort-Object) -join "`n")
}
function Snapshot([string]$directory) {
    $previous = $null
    for ($attempt = 0; $attempt -lt 50; ++$attempt) {
        try {
            $current = Snapshot-Once $directory
            if ($null -ne $previous -and $current -eq $previous) { return $current }
            $previous = $current
        }
        catch {
            $failure = $_.Exception
            while ($null -ne $failure.InnerException) { $failure = $failure.InnerException }
            if (($failure.HResult -band 0xffff) -notin @(2, 3, 32, 33)) { throw }
            $previous = $null
        }
        Start-Sleep -Milliseconds 20
    }
    throw 'Fixture files are still changing or locked; cannot obtain an exact stable snapshot'
}
function Expect-Failure([string]$directory, [bool]$allowRunning = $true) {
    try { [void][TrafficHistoryMigration]::Upgrade($directory, $allowRunning) }
    catch { return $_.Exception.ToString() }
    throw 'Unsafe upgrade unexpectedly succeeded'
}
function Copy-Latest([string]$directory) {
    Copy-Item -LiteralPath $template -Destination $directory -Recurse
}
function Write-NativeHistory([string]$directory, [string]$migrationRevision = '') {
    $state = "history_format`t8`nhistory_archive_format`t2`nall_time_recovery_format`t1`nlanguage`t1`n" +
        "range_start`t2026-10-17 00:00`nrange_end`t2026-10-17 18:43`nall_time`t125816352`t1479986`n" +
        "path`tsvchost.exe`tC:\Windows\System32\svchost.exe`n"
    if ($migrationRevision) { $state += "history_migration_format`t$migrationRevision`n" }
    Write-Fixture (Join-Path $directory 'app_traffic_state.tsv') $state
    Write-Fixture (Join-Path $directory 'app_traffic_history\apps.tsv') "version`t4`napp`t1`tsvchost.exe`n"
    Write-Fixture (Join-Path $directory 'app_traffic_history\2026\10\2026-10-17.tsv') "18:43`t1`t3L`t4d`n"
}

$template = Join-Path $fixture 'template'
Write-Fixture (Join-Path $template 'app_traffic_history.tsv') "2026-10-16 14:49`tlarge.exe`t4294967296`t17`n"
Check ([TrafficHistoryMigration]::Run($template, $true) -eq 0) 'Cannot prepare latest-format fixture'
Write-Fixture (Join-Path $template 'network_traffic_history\2026\10\2026-10-16.tsv') "network sentinel`n"
Write-Fixture (Join-Path $template 'network_traffic_history\adapter.txt') "adapter sentinel`n"
Write-Fixture (Join-Path $template 'network_traffic_history\pending.tsv') "pending sentinel`n"
Write-Fixture (Join-Path $template 'app_traffic_history\current.tmp') "checkpoint sentinel`n"
Write-Fixture (Join-Path $template 'app_traffic_history\current.commit') "commit sentinel`n"
Write-Fixture (Join-Path $template 'app_traffic_state.tsv.tmp') "state cache sentinel`n"
Write-Fixture (Join-Path $template 'app_traffic_state.tsv.new') "state temporary sentinel`n"
Write-Fixture (Join-Path $template '.traffic_history_failed_saved\original.bin') "failed migration sentinel`n"
Write-Fixture (Join-Path $template '.traffic_history_staging_saved\unfinished.bin') "staging sentinel`n"
[IO.File]::WriteAllBytes((Join-Path $template 'network_traffic_history\2026\10\2026-10-16.tsv.sum'), [byte[]](0..255))
$null = New-Item -ItemType Directory -Path (Join-Path $template 'app_traffic_history\empty directory')

$latest = Join-Path $fixture ('latest ' + [char]0x4e2d + [char]0x6587)
Copy-Latest $latest
$expected = Snapshot $latest
foreach ($name in @('history_traffic.dat', 'history_traffic.dat.bak', 'config.ini', 'global_cfg.ini', 'keep.txt')) {
    Write-Fixture (Join-Path $latest $name) "official/unrelated sentinel $name`n"
}
Check ([TrafficHistoryMigration]::Upgrade($latest, $true) -eq 0) 'Latest-format relocation failed'
Check ((Snapshot (Join-Path $latest 'history')) -eq $expected) 'Relocation changed bytes, names, directories or last-write times'
foreach ($name in @('history_traffic.dat', 'history_traffic.dat.bak', 'config.ini', 'global_cfg.ini', 'keep.txt')) {
    Check ([IO.File]::ReadAllText((Join-Path $latest $name)) -eq "official/unrelated sentinel $name`n") 'Official/unrelated file changed'
}
$before = Snapshot $latest
Check ([TrafficHistoryMigration]::Upgrade($latest, $false) -eq 0) 'Latest relocated layout should be a read-only no-op even with the host running'
Check ((Snapshot $latest) -eq $before) 'Repeat upgrade changed data or nested history/history'
Check (!(Test-Path -LiteralPath (Join-Path $latest 'app_traffic_state.tsv'))) 'Old state location was regenerated'
Write-Output 'PASS: new history directory, unchanged latest-format bytes/timestamps/empty directories, all sidecars/backups, official-file preservation and repeat no-op'

foreach ($revision in @('', '0', '7', '9', '999')) {
    $native = Join-Path $fixture ('native-' + $(if ($revision) { $revision } else { 'no-migration-marker' }))
    Write-NativeHistory $native $revision
    $expected = Snapshot $native
    Check ([TrafficHistoryMigration]::Run($native, $false) -eq 0) 'Native dictionary data without a current migration-tool marker was rejected'
    Check ((Snapshot $native) -eq $expected) 'Native format detection rewrote state or added a tool marker'
    Check ([TrafficHistoryMigration]::Upgrade($native, $true) -eq 0) 'Native history relocation failed'
    Check ((Snapshot (Join-Path $native 'history')) -eq $expected) 'Native relocation changed state, totals or raw bytes'
    Check (!(Test-Path -LiteralPath (Join-Path $native 'history\network_traffic_history'))) 'Missing adapter history was treated as required or fabricated'
    $before = Snapshot $native
    Check ([TrafficHistoryMigration]::Upgrade($native, $false) -eq 0) 'Native relocated history was not repeatable'
    Check ((Snapshot $native) -eq $before) 'Repeated native upgrade rewrote history'
}
Write-Output 'PASS: native format 8/archive 2/dictionary 4, absent/zero/older/current/newer migration markers, no adapter folder, exact state/totals preservation and repeat no-op'

foreach ($kind in @('future-history', 'future-archive', 'future-dictionary', 'missing-state', 'missing-dictionary')) {
    $unsupported = Join-Path $fixture $kind
    Write-NativeHistory $unsupported
    if ($kind -eq 'future-history') {
        Write-Fixture (Join-Path $unsupported 'app_traffic_state.tsv') "history_format`t9`nhistory_archive_format`t2`n"
    }
    elseif ($kind -eq 'future-archive') {
        Write-Fixture (Join-Path $unsupported 'app_traffic_state.tsv') "history_format`t8`nhistory_archive_format`t3`n"
    }
    elseif ($kind -eq 'future-dictionary') {
        Write-Fixture (Join-Path $unsupported 'app_traffic_history\apps.tsv') "version`t5`napp`t1`tsvchost.exe`n"
    }
    elseif ($kind -eq 'missing-state') {
        [IO.File]::Delete((Join-Path $unsupported 'app_traffic_state.tsv'))
    }
    else { [IO.File]::Delete((Join-Path $unsupported 'app_traffic_history\apps.tsv')) }
    $before = Snapshot $unsupported
    $failure = Expect-Failure $unsupported
    Check ($failure.Contains('format metadata')) 'Unsupported dictionary history did not explain the missing/unsupported metadata'
    Check ((Snapshot $unsupported) -eq $before) 'Unsupported dictionary history was converted or moved'
}
Write-Output 'PASS: unsupported/missing format metadata is explained and preserved, not decoded as legacy named-app data'

foreach ($kind in @('single', 'daily-old')) {
    $legacy = Join-Path $fixture ('automatic-' + $kind)
    $row = "14:49`tlegacy.exe`t4294967296`t17`n"
    if ($kind -eq 'single') {
        Write-Fixture (Join-Path $legacy 'app_traffic_history.tsv') ("2026-10-16 " + $row)
    }
    else {
        Write-Fixture (Join-Path $legacy 'app_traffic_history\2026-10-16.tsv') $row
        Write-Fixture (Join-Path $legacy 'app_traffic_history\old\2026-09-16.tsv') "14:49`tolder.exe`t7`t11`n"
    }
    Write-Fixture (Join-Path $legacy 'network_traffic_history\pending.tsv') "adapter original`n"
    Check ([TrafficHistoryMigration]::Upgrade($legacy, $true) -eq 0) 'Automatic legacy conversion plus relocation failed'
    $storage = Join-Path $legacy 'history'
    $state = [IO.File]::ReadAllLines((Join-Path $storage 'app_traffic_state.tsv'))
    $total = if ($kind -eq 'single') { "all_time`t4294967296`t17" } else { "all_time`t4294967303`t28" }
    Check ($state -contains $total) 'Automatic upgrade changed legacy totals'
    Check ([IO.File]::ReadAllText((Join-Path $storage 'network_traffic_history\pending.tsv')) -eq "adapter original`n") 'Legacy upgrade changed adapter data'
    $backup = @(Get-ChildItem -LiteralPath $storage -Directory -Filter '.traffic_history_migration_*')
    Check ($backup.Count -eq 1) 'Original-format safety backup was not moved with the data'
    if ($kind -eq 'single') {
        Check ([IO.File]::ReadAllText((Join-Path $backup[0].FullName 'app_traffic_history.tsv')) -eq ("2026-10-16 " + $row)) 'Original single-file backup changed'
    }
    $before = Snapshot $legacy
    Check ([TrafficHistoryMigration]::Upgrade($legacy, $true) -eq 0) 'Automatic upgrade repeat failed'
    Check ((Snapshot $legacy) -eq $before) 'Automatic upgrade repeat changed converted data'
}
Write-Output 'PASS: one-step single/daily/old conversion plus relocation, >2 GiB totals, exact original backup and adapter preservation'

$adapterOnly = Join-Path $fixture 'adapter-only'
Write-Fixture (Join-Path $adapterOnly 'network_traffic_history\pending.tsv') "adapter-only sentinel`n"
Write-Fixture (Join-Path $adapterOnly 'app_traffic_state.tsv') "empty-process-state sentinel`n"
$expected = Snapshot $adapterOnly
Write-Fixture (Join-Path $adapterOnly 'history\keep.txt') "unrelated destination file`n"
Check ([TrafficHistoryMigration]::Upgrade($adapterOnly, $true) -eq 0) 'Adapter/state-only relocation failed'
Check ([IO.File]::ReadAllText((Join-Path $adapterOnly 'history\keep.txt')) -eq "unrelated destination file`n") 'Unrelated destination content was overwritten'
Check ([IO.File]::ReadAllText((Join-Path $adapterOnly 'history\app_traffic_state.tsv')) -eq "empty-process-state sentinel`n") 'State-only data was needlessly converted'
$empty = Join-Path $fixture 'empty'
$null = New-Item -ItemType Directory -Path $empty
Check ([TrafficHistoryMigration]::Upgrade($empty, $true) -eq 0) 'Empty install should not fail'
Check (@(Get-ChildItem -LiteralPath $empty -Force).Count -eq 0) 'Empty install gained unnecessary data'
Write-Output 'PASS: adapter/state-only data, unrelated destination files and empty-install no-op'

$inside = Join-Path $fixture 'legacy-already-under-history'
Write-Fixture (Join-Path $inside 'history\app_traffic_history.tsv') "2026-10-16 14:49`tinside.exe`t19`t23`n"
Check ([TrafficHistoryMigration]::Upgrade($inside, $true) -eq 0) 'Legacy data already under history did not convert in place'
Check ([IO.File]::ReadAllLines((Join-Path $inside 'history\app_traffic_state.tsv')) -contains "all_time`t19`t23") 'In-place legacy conversion changed totals'
Check (!(Test-Path -LiteralPath (Join-Path $inside 'history\history'))) 'In-place conversion created nested history/history'
Write-Output 'PASS: legacy format already under history converts in place without double nesting'

foreach ($kind in @('both-locations', 'destination-file', 'invalid-format')) {
    $blocked = Join-Path $fixture $kind
    if ($kind -eq 'invalid-format') {
        Write-Fixture (Join-Path $blocked 'app_traffic_history.tsv') "invalid original`n"
    }
    else {
        Copy-Latest $blocked
        if ($kind -eq 'both-locations') { Write-Fixture (Join-Path $blocked 'history\app_traffic_state.tsv') "conflict sentinel`n" }
        else { Write-Fixture (Join-Path $blocked 'history') "not a directory`n" }
    }
    $before = Snapshot $blocked
    $failure = Expect-Failure $blocked
    Check ((Snapshot $blocked) -eq $before) 'Refused upgrade changed files'
}
if ([Diagnostics.Process]::GetProcessesByName('TrafficMonitor').Length -ne 0) {
    $running = Join-Path $fixture 'running-host'
    Copy-Latest $running
    $before = Snapshot $running
    $failure = Expect-Failure $running $false
    Check ($failure.Contains('TrafficMonitor is running')) 'Running-host refusal did not explain how to proceed'
    Check ((Snapshot $running) -eq $before) 'Running-host refusal changed files'
    Write-Output 'PASS: running-host protection before conversion or relocation'
}
else { Write-Output 'SKIP: running-host refusal (no TrafficMonitor process is running)' }
Write-Output 'PASS: ambiguous locations, destination file and invalid-format refusal without changes'

$concurrent = Join-Path $fixture 'concurrent-run'
Copy-Latest $concurrent
$before = Snapshot $concurrent
$gate = [IO.FileStream]::new((Join-Path $concurrent '.traffic_history_update.lock'), [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None, 1, [IO.FileOptions]::DeleteOnClose)
try { $failure = Expect-Failure $concurrent }
finally { $gate.Dispose() }
Check ((Snapshot $concurrent) -eq $before) 'Concurrent-run refusal changed data'
Write-Output 'PASS: second migration instance cannot change a locked installation'

foreach ($preexisting in @($false, $true)) {
    $rollback = Join-Path $fixture ('rollback-' + $preexisting)
    Copy-Latest $rollback
    if ($preexisting) { $null = New-Item -ItemType Directory -Path (Join-Path $rollback 'history') }
    $before = Snapshot $rollback
    $handle = [IO.FileStream]::new((Join-Path $rollback 'app_traffic_state.tsv.tmp'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try { $failure = Expect-Failure $rollback }
    finally { $handle.Dispose() }
    Check ($failure.Contains('moved entries were restored')) ("Locked late entry did not exercise partial relocation rollback: " + $failure)
    $after = Snapshot $rollback
    $expectedData = @($before.Split([char]10) | Where-Object { $_ -ne 'D|history' }) -join "`n"
    $actualData = @($after.Split([char]10) | Where-Object { $_ -ne 'D|history' }) -join "`n"
    if ($actualData -ne $expectedData) { Compare-Object ($expectedData.Split([char]10)) ($actualData.Split([char]10)) | Format-List | Out-Host }
    Check ($actualData -eq $expectedData) 'Partial-move rollback lost, changed or left data at the wrong location'
    Check ([TrafficHistoryMigration]::Upgrade($rollback, $true) -eq 0) 'Retry after releasing the locked file failed'
    Check ((Snapshot (Join-Path $rollback 'history')) -eq $expectedData) 'Retry lost or duplicated restored data'
}
Write-Output 'PASS: real Windows file locks fail after earlier moves; original data is restored, any empty destination is harmless and unlocked retry preserves exact data'

$junctionTarget = Join-Path $fixture 'junction-target'
Write-Fixture (Join-Path $junctionTarget 'sentinel.txt') "outside sentinel`n"
foreach ($kind in @('destination-junction', 'source-junction')) {
    $linked = Join-Path $fixture $kind
    Copy-Latest $linked
    $link = if ($kind -eq 'destination-junction') { Join-Path $linked 'history' } else { Join-Path $linked 'app_traffic_history\linked-directory' }
    $null = New-Item -ItemType Junction -Path $link -Target $junctionTarget
    $failure = Expect-Failure $linked
    Check ($failure.Contains('junction') -or $failure.Contains('Unsafe plugin data path')) 'Junction was not refused'
    Check ([IO.File]::ReadAllText((Join-Path $junctionTarget 'sentinel.txt')) -eq "outside sentinel`n") 'Junction target was modified'
    Check (Test-Path -LiteralPath (Join-Path $linked 'app_traffic_state.tsv')) 'Junction refusal moved original state'
}
Write-Output 'PASS: source/destination junction refusal without traversing or moving outside data'

$saved = @{}
foreach ($name in @('TM_HISTORY_BASEDIR', 'TM_HISTORY_ALLOW_RUNNING', 'TM_HISTORY_ASSUME_YES', 'TM_HISTORY_NO_PAUSE')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name)
}
try {
    $env:TM_HISTORY_BASEDIR = $null
    $env:TM_HISTORY_ALLOW_RUNNING = '1'
    $env:TM_HISTORY_ASSUME_YES = '1'
    $env:TM_HISTORY_NO_PAUSE = '1'
    $packaged = Join-Path $fixture ('packaged ' + [char]0x4e2d + [char]0x6587)
    Write-Fixture (Join-Path $packaged 'app_traffic_history.tsv') "2026-10-16 14:49`tstandalone.exe`t233`t261`n"
    Write-Fixture (Join-Path $packaged 'TrafficMonitor.exe') "discovery fixture, not executed`n"
    Write-Fixture (Join-Path $packaged 'history_traffic.dat') "official packaged sentinel`n"
    $toolDirectory = Join-Path $packaged 'plugins\nested tools'
    $null = New-Item -ItemType Directory -Path $toolDirectory
    $launcher = Join-Path $toolDirectory 'UpdateTrafficHistory.cmd'
    Copy-Item -LiteralPath (Join-Path $root 'HistoryTrafficTool\UpdateTrafficHistory.cmd') -Destination $launcher
    & $launcher
    Check ($LASTEXITCODE -eq 0) 'Standalone packaged CMD failed'
    Check ([IO.File]::ReadAllLines((Join-Path $packaged 'history\app_traffic_state.tsv')) -contains "all_time`t233`t261") 'Packaged CMD did not discover the host and relocate converted data'
    Check ([IO.File]::ReadAllText((Join-Path $packaged 'history_traffic.dat')) -eq "official packaged sentinel`n") 'Packaged CMD changed official data'
    $before = Snapshot $packaged
    & $launcher
    Check ($LASTEXITCODE -eq 0 -and (Snapshot $packaged) -eq $before) 'Packaged CMD repeat was not a no-op'

    $nativePackaged = Join-Path $fixture 'packaged-native-no-migration-marker'
    Write-NativeHistory $nativePackaged
    $expected = Snapshot $nativePackaged
    Write-Fixture (Join-Path $nativePackaged 'TrafficMonitor.exe') "discovery fixture, not executed`n"
    $nativeToolDirectory = Join-Path $nativePackaged 'plugins\HistoryTrafficTool'
    $null = New-Item -ItemType Directory -Path $nativeToolDirectory
    $nativeLauncher = Join-Path $nativeToolDirectory 'UpdateTrafficHistory.cmd'
    Copy-Item -LiteralPath (Join-Path $root 'HistoryTrafficTool\UpdateTrafficHistory.cmd') -Destination $nativeLauncher
    & $nativeLauncher
    Check ($LASTEXITCODE -eq 0) 'Standalone CMD rejected native history without a migration marker or adapter folder'
    Check ((Snapshot (Join-Path $nativePackaged 'history')) -eq $expected) 'Standalone CMD rewrote native state/totals or raw history'
}
finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
}
Check ([IO.File]::ReadAllText((Join-Path $root 'HistoryTrafficTool\UpdateTrafficHistory.cmd')).Contains($source)) 'Packaged C# source differs from the tested implementation'
Check ([IO.File]::ReadAllText((Join-Path $root 'HistoryTrafficTool\UpdateTrafficHistory.cmd')).Contains([IO.File]::ReadAllText((Join-Path $root 'HistoryTrafficTool\UpdateTrafficHistory.ps1')))) 'Packaged PowerShell differs from the tested launcher'
Write-Output 'PASS: standalone CMD, native data without tool marker/adapter folder, upward discovery, spaces/Chinese paths, embedded-source parity and repeat no-op'
Write-Output "Fixtures retained: $fixture"
