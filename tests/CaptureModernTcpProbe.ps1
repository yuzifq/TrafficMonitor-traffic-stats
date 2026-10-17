param([Parameter(Mandatory = $true)][string]$Prefix, [string]$ReadySuffix = '.capture-ready')

$ErrorActionPreference = 'Stop'
$name = 'V2Tcp-' + [Guid]::NewGuid().ToString('N').Substring(0, 12)
$started = $false
try {
    & logman.exe start $name -ets -o ($Prefix + '.modern.etl') -p Microsoft-Windows-TCPIP 0x201700000484 16 -bs 64 -nb 8 64 -f bincirc -max 32 2>&1 |
        Out-File -LiteralPath ($Prefix + '.modern-log') -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw 'Cannot start owned modern TCP diagnostic session.' }
    $started = $true
    [IO.File]::WriteAllText($Prefix + $ReadySuffix, 'ready')
    $deadline = [DateTime]::UtcNow.AddSeconds(50)
    while (!(Test-Path -LiteralPath ($Prefix + '.quit'))) {
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Controlled TCP diagnostic timed out.' }
        Start-Sleep -Milliseconds 50
    }
    # The peer closes only after the native helper finishes reading EStats.
    # Leave a short bounded interval for the connection summary event.
    Start-Sleep -Seconds 2
}
catch {
    $_ | Out-File -LiteralPath ($Prefix + '.modern-log') -Append -Encoding utf8
    throw
}
finally {
    if ($started) {
        & logman.exe stop $name -ets 2>&1 | Out-File -LiteralPath ($Prefix + '.modern-log') -Append -Encoding utf8
    }
}
