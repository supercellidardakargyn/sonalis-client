param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$ScannerExecutable,
    [Parameter(Mandatory = $true)][string]$BuildMetadata,
    [Parameter(Mandatory = $true)][string]$Destination,
    [Parameter(Mandatory = $true)][string]$ScannerDestination
)

$ErrorActionPreference = 'Stop'
$metadata = Get-Content -LiteralPath $BuildMetadata -Raw | ConvertFrom-Json
if ($metadata.configuration -ne 'Release') { throw 'Only a Release build may be staged.' }
if ($metadata.channel -notin @('stable', 'canary')) { throw 'Invalid embedded update channel.' }

function Assert-VerifiedExecutable([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 512 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { throw "$Path is not a valid PE executable." }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -lt 0 -or $peOffset + 6 -gt $bytes.Length) { throw "$Path has an invalid PE header offset." }
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    if ($machine -ne 0x8664) { throw "$Path is not an x64 PE executable." }
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($metadata.channel -eq 'stable' -and $signature.Status -ne 'Valid') {
        throw "Stable artifacts require valid Authenticode on $Path (status: $($signature.Status))."
    }
}

Assert-VerifiedExecutable $Executable
Assert-VerifiedExecutable $ScannerExecutable

$destinationDirectory = Split-Path -Parent $Destination
if ($destinationDirectory) { New-Item -ItemType Directory -Force -Path $destinationDirectory | Out-Null }
Copy-Item -LiteralPath $Executable -Destination $Destination -Force
$scannerDirectory = Split-Path -Parent $ScannerDestination
if ($scannerDirectory) { New-Item -ItemType Directory -Force -Path $scannerDirectory | Out-Null }
Copy-Item -LiteralPath $ScannerExecutable -Destination $ScannerDestination -Force
Write-Output "Verified $($metadata.channel) $($metadata.version) x64 client and Guardian scanner staged."
