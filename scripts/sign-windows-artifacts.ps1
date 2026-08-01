[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string[]]$Path,
    [string]$CertificateThumbprint = $env:SONALIS_SIGN_CERT_THUMBPRINT,
    [string]$PfxPath = $env:SONALIS_SIGN_PFX_PATH,
    [string]$ExpectedCertificateSha256 = $env:SONALIS_AUTHENTICODE_CERT_SHA256,
    [string]$TimestampUrl = 'http://timestamp.digicert.com'
)

$ErrorActionPreference = 'Stop'
$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
    Where-Object FullName -Match '\\x64\\' |
    Sort-Object FullName -Descending |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $signtool) { throw 'Windows SDK signtool.exe bulunamadı.' }
if (-not $CertificateThumbprint -and -not $PfxPath) {
    throw 'SONALIS_SIGN_CERT_THUMBPRINT veya SONALIS_SIGN_PFX_PATH zorunludur.'
}
if ($ExpectedCertificateSha256 -notmatch '^[0-9A-Fa-f]{64}$') {
    throw 'SONALIS_AUTHENTICODE_CERT_SHA256 tam 64 hex karakter olmalıdır.'
}

foreach ($item in $Path) {
    $resolved = (Resolve-Path -LiteralPath $item).Path
    $arguments = @('sign', '/fd', 'SHA256', '/tr', $TimestampUrl, '/td', 'SHA256')
    if ($CertificateThumbprint) {
        $arguments += @('/sha1', $CertificateThumbprint)
    } else {
        $arguments += @('/f', (Resolve-Path -LiteralPath $PfxPath).Path)
        if ($env:SONALIS_SIGN_PFX_PASSWORD) {
            $arguments += @('/p', $env:SONALIS_SIGN_PFX_PASSWORD)
        }
    }
    $arguments += $resolved
    & $signtool @arguments
    if ($LASTEXITCODE -ne 0) { throw "Authenticode imzalama başarısız: $resolved" }

    & $signtool verify /pa /all /v $resolved | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Authenticode doğrulaması başarısız: $resolved" }
    $signature = Get-AuthenticodeSignature -LiteralPath $resolved
    if ($signature.Status -ne 'Valid' -or -not $signature.SignerCertificate) {
        throw "Authenticode sertifikası okunamadı: $resolved"
    }

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $actualCertificateSha256 = ([BitConverter]::ToString(
            $sha256.ComputeHash($signature.SignerCertificate.RawData))).Replace('-', '')
    } finally {
        $sha256.Dispose()
    }
    if ($actualCertificateSha256 -ne $ExpectedCertificateSha256.ToUpperInvariant()) {
        throw "İmzalayan sertifika sabitlenmiş Sonalis sertifikası değil: $resolved"
    }
    Write-Output "Authenticode doğrulandı: $resolved"
}
