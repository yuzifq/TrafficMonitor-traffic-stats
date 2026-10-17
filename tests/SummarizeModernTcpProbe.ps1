param([Parameter(Mandatory = $true)][string]$Prefix)

$ErrorActionPreference = 'Stop'
$connection = Get-Content -LiteralPath ($Prefix + '.connection.json') -Raw | ConvertFrom-Json
$ownerPid = [int]$connection.pid

function Get-Fields($event) {
    $xml = [xml]$event.ToXml()
    $fields = @{}
    foreach ($field in $xml.Event.EventData.Data) { $fields[$field.Name] = $field.InnerText }
    return $fields
}
function Test-Address([string]$encoded, [string]$address, [int]$port) {
    $ip = [Net.IPAddress]::Parse($address)
    $ipv6 = $ip.AddressFamily -eq [Net.Sockets.AddressFamily]::InterNetworkV6
    $length = if ($ipv6) { 56 } else { 32 }
    $family = if ($ipv6) { '1700' } else { '0200' }
    $offset = if ($ipv6) { 16 } else { 8 }
    $hex = [BitConverter]::ToString($ip.GetAddressBytes()).Replace('-', '')
    return $encoded.Length -eq $length -and $encoded.Substring(0, 4) -eq $family -and
        $encoded.Substring(4, 4) -eq ('{0:X4}' -f $port) -and
        $encoded.Substring($offset, $hex.Length) -eq $hex
}
function Test-Endpoints($fields) {
    return (Test-Address $fields.LocalAddress $connection.localAddress $connection.localPort) -and
        (Test-Address $fields.RemoteAddress $connection.remoteAddress $connection.remotePort)
}

# TCB pointers are reusable. Scope every event to the endpoint-verified lifetime,
# not just to the first matching pointer or the caller PID of asynchronous events.
$lifecycle = @(Get-WinEvent -Path ($Prefix + '.modern.etl') -Oldest -FilterXPath '*[System[EventID=1002 or EventID=1477]]')
$opened = @($lifecycle | Where-Object { $_.Id -eq 1002 -and $_.ProcessId -eq $ownerPid -and (Test-Endpoints (Get-Fields $_)) })
if ($opened.Count -ne 1) { throw "Expected one controlled connect event; found $($opened.Count)." }
$start = $opened[0].TimeCreated
$tcb = $opened[0].Properties[0].Value.ToString()
$closed = @($lifecycle | Where-Object {
    $_.Id -eq 1477 -and $_.TimeCreated -ge $start -and
    $_.Properties[0].Value.ToString() -eq $tcb -and (Test-Endpoints (Get-Fields $_))
})
if ($closed.Count -ne 1) { throw "Expected one endpoint-verified connection summary; found $($closed.Count)." }
$end = $closed[0].TimeCreated
foreach ($event in $lifecycle) {
    if ($event.Id -eq 1002 -and $event.TimeCreated -gt $start -and $event.TimeCreated -lt $end -and
        $event.Properties[0].Value.ToString() -eq $tcb) {
        throw 'TCB reused inside the selected lifetime; accounting is ambiguous.'
    }
}
$send = [Collections.Generic.List[object]]::new()
$receive = [Collections.Generic.List[object]]::new()
$summaries = [Collections.Generic.List[object]]::new()
$xpath = '*[System[EventID=1332 or EventID=1074]]'
[long]$outsideLifetime = 0
foreach ($event in (Get-WinEvent -Path ($Prefix + '.modern.etl') -Oldest -FilterXPath $xpath)) {
    $values = $event.Properties
    if (!$values.Count -or $values[0].Value.ToString() -ne $tcb) { continue }
    if ($event.TimeCreated -lt $start -or $event.TimeCreated -gt $end) { ++$outsideLifetime; continue }
    if ($event.Id -eq 1332) {
        if ($values.Count -lt 5) { throw 'Unsupported modern send schema.' }
        $fields = Get-Fields $event
        if ($fields.LocalAddress -and !(Test-Endpoints $fields)) { throw 'Conflicting send endpoints in selected lifetime.' }
        $send.Add([pscustomobject]@{Time=$event.TimeCreated;Bytes=[long]$values[3].Value;Sequence=[long]$values[4].Value})
    }
    elseif ($event.Id -eq 1074) {
        if ($values.Count -lt 3) { throw 'Unsupported modern receive schema.' }
        $receive.Add([pscustomobject]@{Time=$event.TimeCreated;Bytes=[long]$values[1].Value;Sequence=[long]$values[2].Value})
    }
}
$summaryFields = Get-Fields $closed[0]
$summaries.Add([pscustomobject]@{Id=$closed[0].Id;DataOut=[uint64]$summaryFields.DataBytesOut;
    DataIn=[uint64]$summaryFields.DataBytesIn;BytesRetrans=[uint64]$summaryFields.BytesRetrans})
function Summarize($records) {
    [long]$total = 0
    [long]$repeated = 0
    $ranges = @()
    $origin = $null
    foreach ($record in $records) {
        $total += $record.Bytes
        if (!$record.Bytes) { continue }
        if ($null -eq $origin) { $origin = $record.Sequence }
        $left = ($record.Sequence - $origin + 4294967296L) % 4294967296L
        if ($left -ge 2147483648L) { $left -= 4294967296L }
        $right = $left + $record.Bytes
        foreach ($span in $ranges) { $repeated += [Math]::Max(0L, [Math]::Min($span.End, $right) - [Math]::Max($span.Start, $left)) }
        $ranges = @($ranges) + @([pscustomobject]@{Start=$left;End=$right})
        $merged = [Collections.Generic.List[object]]::new()
        foreach ($span in ($ranges | Sort-Object Start)) {
            if ($merged.Count -and $span.Start -le $merged[$merged.Count-1].End) {
                $merged[$merged.Count-1].End = [Math]::Max($merged[$merged.Count-1].End, $span.End)
            }
            else { $merged.Add($span) }
        }
        $ranges = @($merged.ToArray())
    }
    [pscustomobject]@{Events=$records.Count;Bytes=$total;RepeatedSequenceBytes=$repeated;UniqueSequenceBytes=$total-$repeated}
}
$wire = ([IO.File]::ReadAllText($Prefix + '.wire-total')).Trim().Split(' ')
$result = [pscustomobject]@{TcbCount=1;LifetimeStart=$start.ToString('o');LifetimeEnd=$end.ToString('o');
    ReusedTcbEventsExcluded=$outsideLifetime;SocketRx=[long]$wire[0];SocketTx=[long]$wire[1];
    Send=(Summarize $send);Receive=(Summarize $receive);ConnectionSummaries=@($summaries.ToArray());
    SmallReceiveEvents=@($receive | Where-Object { $_.Bytes -le 1 })}
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath ($Prefix + '.modern-summary.json') -Encoding UTF8
$result | ConvertTo-Json -Depth 5
