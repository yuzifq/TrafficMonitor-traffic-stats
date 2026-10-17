param([Parameter(Mandatory = $true)][string]$ReportPath)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $ReportPath) { throw 'Use a new report path.' }
$report = [ordered]@{
    WindowsVersion=[Environment]::OSVersion.Version.ToString()
    Adapters=@(Get-NetAdapter | Select-Object Name, InterfaceDescription, Status, ifIndex, HardwareInterface)
    Lso=@(Get-NetAdapterLso | Select-Object Name, IPv4Enabled, IPv6Enabled)
    Rsc=@(Get-NetAdapterRsc | Select-Object Name, IPv4Enabled, IPv6Enabled)
    PacketComponents=((& pktmon comp list 2>&1) -join "`n")
}
$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ReportPath -Encoding utf8
