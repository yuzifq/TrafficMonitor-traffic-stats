param([Parameter(Mandatory = $true)][string]$BackupRoot, [Parameter(Mandatory = $true)][string]$Executable)
$ErrorActionPreference = 'Stop'
$fixture = Join-Path $PSScriptRoot ('..\build\reset-day-checks-' + [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())
$null = New-Item -ItemType Directory -Path $fixture
foreach ($file in (Get-ChildItem -LiteralPath $BackupRoot)) { Copy-Item -LiteralPath $file.FullName -Destination $fixture -Recurse }
Copy-Item -LiteralPath $Executable -Destination (Join-Path $fixture 'TrafficMonitor.exe')
$before = Get-Content -LiteralPath (Join-Path $fixture 'manifest.json') -Raw | ConvertFrom-Json
& (Join-Path $PSScriptRoot '..\tools\ResetTrafficDay.ps1') -Root $fixture -Date '2026-10-17' -Apply -ResetOfficial
if (-not $?) { throw 'Reset fixture failed' }
$changed = @('app_traffic_history\2026\10\2026-10-17.tsv', 'app_traffic_history\current.tmp',
    'app_traffic_history\current.commit', 'network_traffic_history\2026\10\2026-10-17.tsv',
    'network_traffic_history\2026\10\2026-10-17.tsv.sum', 'network_traffic_history\pending.tsv',
    'app_traffic_state.tsv', 'history_traffic.dat', 'history_traffic.dat.bak',
    'app_traffic_history\2026\10\2026-10-17.quality.tsv')
foreach ($item in $before) {
    if ($changed -contains $item.Path) { continue }
    $path = Join-Path $fixture $item.Path
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $item.Sha256) { throw ('Older file changed: ' + $item.Path) }
}
$state = Get-Content -LiteralPath (Join-Path $fixture 'app_traffic_state.tsv') -Raw
if ($state -notmatch "(?m)^all_time`t2963210484269`t2381640650024`r?$") { throw 'Cumulative reset was not exact' }
foreach ($name in @('history_traffic.dat', 'history_traffic.dat.bak')) {
    $path = Join-Path $fixture $name
    $text = Get-Content -LiteralPath $path -Raw
    if ($text -notmatch '(?m)^2026/10/17 0/0\r?$') { throw 'Official reset failed' }
    $olderBefore = @((Get-Content -LiteralPath (Join-Path $BackupRoot $name)) | Where-Object { $_ -notlike '2026/10/17 *' })
    $olderAfter = @((Get-Content -LiteralPath $path) | Where-Object { $_ -notlike '2026/10/17 *' })
    if (Compare-Object $olderBefore $olderAfter) { throw 'Older official records changed' }
}
foreach ($relative in ($changed | Where-Object { $_ -notin @('app_traffic_state.tsv','history_traffic.dat','history_traffic.dat.bak') })) {
    if (Test-Path -LiteralPath (Join-Path $fixture $relative)) { throw ('Reset record remained: ' + $relative) }
}
Write-Output ('PASS: reset preserved all older SHA256 hashes and official rows; exact cumulative subtraction; fixture=' + $fixture)
