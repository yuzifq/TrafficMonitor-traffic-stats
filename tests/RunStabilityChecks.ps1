param([Parameter(Mandatory = $true)][string]$ReportDirectory,
    [ValidateSet('windows', 'sources', 'hybrid-short', 'udp', 'mixed', 'endpoints', 'native', 'steps', 'load', 'accounting-window', 'ui-checkpoint', 'quic')]
    [string[]]$Tests = @('windows', 'sources', 'hybrid-short', 'udp', 'mixed', 'endpoints', 'native', 'steps', 'load', 'accounting-window', 'ui-checkpoint'),
    [ValidateRange(1, 10)][int]$Repeat = 1)

$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run the native stability suite with elevation.'
}
$ReportDirectory = [IO.Path]::GetFullPath($ReportDirectory)
if (Test-Path -LiteralPath $ReportDirectory) { throw 'Choose a new report directory; existing reports are not overwritten.' }
$null = New-Item -ItemType Directory -Path $ReportDirectory
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$executable = Join-Path $root 'build\tests\HybridTrafficChecks.exe'
$results = @()
if ($Tests.Count -ne @($Tests | Select-Object -Unique).Count) { throw 'Duplicate test names would overwrite reports; use -Repeat instead.' }
for ($iteration = 1; $iteration -le $Repeat; $iteration++) {
foreach ($mode in $Tests) {
    $name = if ($Repeat -gt 1) { $mode + '-' + $iteration } else { $mode }
    $report = Join-Path $ReportDirectory ($name + '.tsv')
    $parameters = @{
        FilePath=$executable; WindowStyle='Hidden'; PassThru=$true; WorkingDirectory=$root
        RedirectStandardOutput=($report + '.stdout'); RedirectStandardError=($report + '.stderr')
    }
    if ($mode -eq 'ui-checkpoint') { $parameters.FilePath = Join-Path $root 'build\tests\HistoryViewChecks.exe' }
    else { $parameters.ArgumentList = @('--' + $mode, ('"' + $report + '"')) }
    $process = Start-Process @parameters
    $null = $process.Handle
    $timeout = if ($mode -eq 'windows') { 600000 } else { 180000 }
    if (!$process.WaitForExit($timeout)) {
        $process.Kill()
        $process.WaitForExit()
        $results += [pscustomobject]@{ Test=$mode; Iteration=$iteration; ExitCode=$process.ExitCode; TimedOut=$true }
    }
    else { $results += [pscustomobject]@{ Test=$mode; Iteration=$iteration; ExitCode=$process.ExitCode; TimedOut=$false } }
    $results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $ReportDirectory 'results.json') -Encoding utf8
}
}
if (@($results | Where-Object { $_.ExitCode -ne 0 -or $_.TimedOut }).Count) { exit 1 }
exit 0
