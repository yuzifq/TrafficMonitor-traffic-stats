param([Parameter(Mandatory = $true)][string]$Prefix, [switch]$QueryOnly, [int]$Component = 0,
    [string]$ReadySuffix = '.capture-ready')

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding
& chcp.com 65001 > $null
$log = $Prefix + '.capture-log'
$started = $false
$filterAdded = $false
$filterSnapshot = ''
try {
    $status = & pktmon status 2>&1
    $status | Out-File -LiteralPath $log -Encoding utf8
    if ($LASTEXITCODE -ne 0 -or ($status -join "`n") -notmatch 'not running|\u6ca1\u6709\u8fd0\u884c') {
        throw 'Another Packet Monitor capture is active or status is unavailable; leaving it unchanged.'
    }
    $filters = & pktmon filter list 2>&1
    $filterResult = $LASTEXITCODE
    $filters | Out-File -LiteralPath $log -Append -Encoding utf8
    if ($QueryOnly) { return }
    $filterLines = @($filters | ForEach-Object { "$_".Trim() } | Where-Object { $_ })
    $emptyFilters = ($filterLines.Count -eq 1 -and $filterLines[0] -match '^(None|\u65e0)$') -or
        ($filterLines.Count -eq 2 -and $filterLines[0] -match '^(Packet filters:|\u6570\u636e\u5305\u7b5b\u9009\u5668:)$' -and
            $filterLines[1] -match '^(None|\u65e0)$')
    if ($filterResult -ne 0 -or !$emptyFilters) {
        throw 'Existing packet filters found; leaving them unchanged.'
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while (!(Test-Path -LiteralPath ($Prefix + '.control'))) {
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Controlled TLS connection did not start.' }
        Start-Sleep -Milliseconds 50
    }
    $identity = [IO.File]::ReadAllText($Prefix + '.control').Trim().Split(' ')
    $peer = Get-NetTCPConnection -OwningProcess ([int]$identity[0]) -LocalPort ([int]$identity[1]) -State Established |
        Select-Object -First 1
    if (!$peer) { throw 'Controlled TLS endpoint no longer exists.' }
    & pktmon filter add V2ExternalProbe -t TCP -p $peer.LocalPort 443 -i $peer.RemoteAddress 2>&1 |
        Out-File -LiteralPath $log -Append -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw 'Cannot add the isolated test-connection filter.' }
    $filterAdded = $true
    $filterSnapshot = (& pktmon filter list 2>&1) -join "`n"
    $selector = if ($Component -gt 0) { "$Component" } else { 'nics' }
    # This machine uses 16 MiB ETW buffers; the file must hold more than its header buffer.
    & pktmon start --capture --comp $selector --pkt-size 96 --file-name ($Prefix + '.etl') --file-size 64 2>&1 |
        Out-File -LiteralPath $log -Append -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw 'Cannot start isolated test capture.' }
    $started = $true
    [IO.File]::WriteAllText($Prefix + $ReadySuffix, 'ready')
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    while (!(Test-Path -LiteralPath ($Prefix + '.quit'))) {
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Controlled traffic probe timed out.' }
        Start-Sleep -Milliseconds 50
    }
}
catch {
    $_ | Out-File -LiteralPath $log -Append -Encoding utf8
    throw
}
finally {
    if ($started) {
        & pktmon counters 2>&1 | Out-File -LiteralPath $log -Append -Encoding utf8
        & pktmon stop 2>&1 | Out-File -LiteralPath $log -Append -Encoding utf8
    }
    if ($filterAdded) {
        $current = (& pktmon filter list 2>&1) -join "`n"
        if ($current -eq $filterSnapshot) {
            & pktmon filter remove 2>&1 | Out-File -LiteralPath $log -Append -Encoding utf8
        }
        else {
            'Filters changed concurrently; no filters were removed.' | Out-File -LiteralPath $log -Append -Encoding utf8
        }
    }
}
if ($started) {
    & pktmon etl2pcap ($Prefix + '.etl') --out ($Prefix + '.pcapng') 2>&1 |
        Out-File -LiteralPath $log -Append -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw 'Cannot convert the isolated capture.' }
}
