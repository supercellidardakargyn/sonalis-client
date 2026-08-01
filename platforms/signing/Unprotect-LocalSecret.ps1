[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
$source = (Resolve-Path -LiteralPath $InputPath).Path
$target = [System.IO.Path]::GetFullPath($OutputPath)
$parent = Split-Path -Parent $target
if (-not (Test-Path -LiteralPath $parent)) {
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
}

$protectedBytes = [System.IO.File]::ReadAllBytes($source)
try {
    $plainBytes = [System.Security.Cryptography.ProtectedData]::Unprotect(
        $protectedBytes,
        $null,
        [System.Security.Cryptography.DataProtectionScope]::CurrentUser
    )
    try {
        [System.IO.File]::WriteAllBytes($target, $plainBytes)
    } finally {
        [System.Array]::Clear($plainBytes, 0, $plainBytes.Length)
    }
} finally {
    [System.Array]::Clear($protectedBytes, 0, $protectedBytes.Length)
}

Write-Output $target
