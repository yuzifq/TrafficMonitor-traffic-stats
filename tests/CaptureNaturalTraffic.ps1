param([Parameter(Mandatory = $true)][string]$Prefix, [Parameter(Mandatory = $true)][int]$Component)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding
& chcp.com 65001 > $null
$log = $Prefix + '.capture-log'
$started = $false
try {
    $status = & pktmon status 2>&1
    $status | Out-File -LiteralPath $log -Encoding utf8
    if ($LASTEXITCODE -ne 0 -or ($status -join "`n") -notmatch 'not running|\u6ca1\u6709\u8fd0\u884c') {
        throw 'An existing capture is active or status is unavailable; leaving it unchanged.'
    }
    $filters = & pktmon filter list 2>&1
    $filterResult = $LASTEXITCODE
    $filters | Out-File -LiteralPath $log -Append -Encoding utf8
    $filterLines = @($filters | ForEach-Object { "$_".Trim() } | Where-Object { $_ })
    if ($filterResult -ne 0 -or $filterLines.Count -ne 2 -or
        $filterLines[0] -notmatch '^(Packet filters:|\u6570\u636e\u5305\u7b5b\u9009\u5668:)$' -or
        $filterLines[1] -notmatch '^(None|\u65e0)$') {
        throw 'Existing packet filters found; leaving them unchanged.'
    }
    & pktmon start --capture --comp $Component --pkt-size 96 --flags 0x03f --file-name ($Prefix + '.etl') --file-size 128 2>&1 |
        Out-File -LiteralPath $log -Append -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw 'Cannot start isolated physical NIC capture.' }
    $started = $true
    [IO.File]::WriteAllText($Prefix + '.capture-ready', 'ready')
    $deadline = [DateTime]::UtcNow.AddSeconds(210)
    while (!(Test-Path -LiteralPath ($Prefix + '.quit'))) {
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Natural traffic audit timed out.' }
        Start-Sleep -Milliseconds 100
    }
}
finally {
    if ($started) {
        & pktmon counters 2>&1 | Out-File -LiteralPath $log -Append -Encoding utf8
        & pktmon stop 2>&1 | Out-File -LiteralPath $log -Append -Encoding utf8
    }
}
if ($started) {
    & pktmon etl2pcap ($Prefix + '.etl') --out ($Prefix + '.pcapng') 2>&1 |
        Out-File -LiteralPath $log -Append -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw 'Cannot convert physical NIC capture.' }
}
