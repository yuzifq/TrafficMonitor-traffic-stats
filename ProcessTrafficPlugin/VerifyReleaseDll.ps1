param(
    [Parameter(Mandatory = $true)]
    [string]$Path
)

$ErrorActionPreference = 'Stop'
$bytes = (Get-Item -LiteralPath $Path).Length
if ($bytes -ge 610000) {
    throw "Release DLL is $bytes bytes; it must be strictly smaller than 610000 bytes."
}
Write-Output "Release DLL size: $bytes / 610000 bytes (strict limit)."
