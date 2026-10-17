param([Parameter(Mandatory = $true)][string]$Prefix,
    [Parameter(Mandatory = $true)][string]$NodePath, [int]$Component = 13, [ValidateSet(4,6)][int]$Family = 6,
    [switch]$SkipModern)

$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this diagnostic with elevation.' }
$Prefix = [IO.Path]::GetFullPath($Prefix)
if (@(Get-ChildItem -LiteralPath ([IO.Path]::GetDirectoryName($Prefix)) -File -Filter ([IO.Path]::GetFileName($Prefix) + '.*')).Count) {
    throw 'Use a new report prefix; previous evidence is not overwritten.'
}
$shell = Join-Path $PSHOME 'powershell.exe'
if (!(Test-Path -LiteralPath $shell)) { $shell = Join-Path $PSHOME 'pwsh.exe' }
$nativePath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build\tests\HybridTrafficChecks.exe'))
$captures = @()
$node = $null
$native = $null
$modern = $null
$result = [ordered]@{ Prefix=$Prefix; NativeExit=$null; NodeExit=$null; ModernExit=$null; PacketExit=$null }

function Wait-Ready([string]$suffix, $process, [int]$seconds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($seconds)
    while (!(Test-Path -LiteralPath ($Prefix + $suffix))) {
        if ($process.HasExited) { throw "Diagnostic exited before $suffix; code $($process.ExitCode)." }
        if ([DateTime]::UtcNow -ge $deadline) { throw "Diagnostic timed out waiting for $suffix." }
        Start-Sleep -Milliseconds 50
    }
}
try {
    if (!$SkipModern) {
        $modern = Start-Process -FilePath $shell -WindowStyle Hidden -PassThru -ArgumentList @(
            '-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+(Join-Path $PSScriptRoot 'CaptureModernTcpProbe.ps1')+'"'),
            '-Prefix',('"'+$Prefix+'"'),'-ReadySuffix','.modern-ready')
        $captures += $modern
        $null = $modern.Handle
        Wait-Ready '.modern-ready' $modern 10
    }
    $node = Start-Process -FilePath $NodePath -WindowStyle Hidden -PassThru -ArgumentList @(
        ('"'+(Join-Path $PSScriptRoot 'ExternalTcpProbe.cjs')+'"'),('"'+$Prefix+'"'),$Family) `
        -RedirectStandardOutput ($Prefix+'.node-out') -RedirectStandardError ($Prefix+'.node-error')
    $null = $node.Handle
    Wait-Ready '.control' $node 15
    $packet = Start-Process -FilePath $shell -WindowStyle Hidden -PassThru -ArgumentList @(
        '-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+(Join-Path $PSScriptRoot 'CaptureExternalProbe.ps1')+'"'),
        '-Prefix',('"'+$Prefix+'"'),'-Component',$Component,'-ReadySuffix','.packet-ready')
    $captures += $packet
    $null = $packet.Handle
    Wait-Ready '.packet-ready' $packet 15
    [IO.File]::WriteAllText($Prefix+'.capture-ready','requested collectors ready')
    $native = Start-Process -FilePath $nativePath -WindowStyle Hidden -PassThru -ArgumentList @(
        '--external-capture',('"'+$Prefix+'"')) -RedirectStandardOutput ($Prefix+'.native-out') -RedirectStandardError ($Prefix+'.native-error')
    $null = $native.Handle
    if (!$native.WaitForExit(45000)) { throw 'Controlled native traffic check timed out.' }
    $result.NativeExit = $native.ExitCode
}
catch {
    $result.Error = $_.Exception.Message
}
finally {
    # Both owned capture scripts observe this signal and execute their cleanup.
    [IO.File]::WriteAllText($Prefix+'.quit','diagnostic complete')
    foreach ($capture in $captures) {
        if (!$capture.WaitForExit(60000)) { throw 'Owned capture still running; do not start another capture.' }
    }
    if ($modern) { $result.ModernExit = $modern.ExitCode }
    if ($packet) { $result.PacketExit = $packet.ExitCode }
    if ($node) {
        if (!$node.WaitForExit(10000)) { $node.Kill(); $node.WaitForExit(); $result.NodeForcedStop = $true }
        $result.NodeExit = $node.ExitCode
    }
    if ($native -and !$native.HasExited) {
        # This PID/handle belongs only to the helper launched above.
        if (!$native.WaitForExit(35000)) { $native.Kill(); $native.WaitForExit() }
        $result.NativeExit = $native.ExitCode
    }
    $result | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath ($Prefix+'.combined-report.json') -Encoding utf8
}
if ($result.Contains('Error') -or $result.NativeExit -ne 0 -or $result.NodeExit -ne 0 -or
    $result.PacketExit -ne 0 -or (!$SkipModern -and $result.ModernExit -ne 0)) { exit 1 }
exit 0
