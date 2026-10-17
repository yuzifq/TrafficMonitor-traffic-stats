param([Parameter(Mandatory = $true)][string]$Report)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding
& chcp.com 65001 > $null
& pktmon status 2>&1 | Out-File -LiteralPath $Report -Encoding utf8
& pktmon filter list 2>&1 | Out-File -LiteralPath $Report -Append -Encoding utf8
& pktmon list 2>&1 | Out-File -LiteralPath $Report -Append -Encoding utf8
& logman query -ets 2>&1 | Out-File -LiteralPath $Report -Append -Encoding utf8
