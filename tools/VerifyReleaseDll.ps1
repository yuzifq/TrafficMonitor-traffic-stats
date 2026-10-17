param(
    [Parameter(Mandatory = $true)]
    [string]$Path
)

$ErrorActionPreference = 'Stop'
$bytes = (Get-Item -LiteralPath $Path).Length
if ($bytes -ge 600000) {
    throw "Release DLL is $bytes bytes; it must be strictly smaller than 600000 bytes."
}
Write-Output "Release DLL size: $bytes / 600000 bytes (strict limit)."
