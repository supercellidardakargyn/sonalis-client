[CmdletBinding()]
param(
    [string]$CertificateThumbprint = $env:SONALIS_SIGN_CERT_THUMBPRINT,
    [string]$PfxPath = $env:SONALIS_SIGN_PFX_PATH,
    [string]$TimestampUrl = 'http://timestamp.digicert.com'
)

$ErrorActionPreference = 'Stop'
$clientRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$cmakeLists = Get-Content -LiteralPath (Join-Path $clientRoot 'CMakeLists.txt') -Raw
$versionMatch = [regex]::Match($cmakeLists, 'project\(Sonalis VERSION ([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $versionMatch.Success) { throw 'CMakeLists.txt içinden Sonalis sürümü okunamadı.' }
$version = $versionMatch.Groups[1].Value
$buildDirectory = Join-Path $clientRoot "build-stable-$version"

if (-not $CertificateThumbprint -and -not $PfxPath) {
    throw 'SONALIS_SIGN_CERT_THUMBPRINT veya SONALIS_SIGN_PFX_PATH zorunludur.'
}

if ($CertificateThumbprint) {
    $normalizedThumbprint = ($CertificateThumbprint -replace '\s', '').ToUpperInvariant()
    $certificate = Get-Item -LiteralPath "Cert:\CurrentUser\My\$normalizedThumbprint" -ErrorAction Stop
    if (-not $certificate.HasPrivateKey) { throw 'Seçilen sertifikanın private key erişimi yok.' }
} else {
    $resolvedPfx = (Resolve-Path -LiteralPath $PfxPath).Path
    $certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
        $resolvedPfx,
        $env:SONALIS_SIGN_PFX_PASSWORD,
        [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
    if (-not $certificate.HasPrivateKey) { throw 'PFX private key içermiyor.' }
}

$sha256 = [System.Security.Cryptography.SHA256]::Create()
try {
    $certificateSha256 = ([BitConverter]::ToString($sha256.ComputeHash($certificate.RawData))).Replace('-', '')
} finally {
    $sha256.Dispose()
}
$env:SONALIS_AUTHENTICODE_CERT_SHA256 = $certificateSha256

$vsDevCmd = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $vsDevCmd)) { throw 'Visual Studio Build Tools bulunamadı.' }
if (-not (Test-Path -LiteralPath $cmake)) { throw 'Visual Studio CMake bulunamadı.' }

$configureAndBuild = 'call "{0}" -arch=x64 -host_arch=x64 >nul && cd /d "{1}" && set "SONALIS_AUTHENTICODE_CERT_SHA256={2}" && "{3}" --preset stable-release && "{3}" --build "{4}" --target Sonalis SonalisGuardianScanner' -f `
    $vsDevCmd, $clientRoot, $certificateSha256, $cmake, $buildDirectory
cmd.exe /d /s /c $configureAndBuild
if ($LASTEXITCODE -ne 0) { throw "Stable $version derlemesi başarısız oldu." }

$clientExecutable = Join-Path $buildDirectory 'Sonalis.exe'
$scannerExecutable = Join-Path $buildDirectory 'SonalisGuardianScanner.exe'
$signScript = Join-Path $clientRoot 'scripts\sign-windows-artifacts.ps1'
$signParameters = @{
    Path = @($clientExecutable, $scannerExecutable)
    ExpectedCertificateSha256 = $certificateSha256
    TimestampUrl = $TimestampUrl
}
if ($CertificateThumbprint) { $signParameters.CertificateThumbprint = $CertificateThumbprint }
else { $signParameters.PfxPath = $PfxPath }
& $signScript @signParameters

& (Join-Path $clientRoot 'scripts\stage-verified-client.ps1') `
    -Executable $clientExecutable `
    -ScannerExecutable $scannerExecutable `
    -BuildMetadata (Join-Path $buildDirectory 'sonalis-build-Release.json') `
    -Destination (Join-Path $clientRoot 'Sonalis.exe') `
    -ScannerDestination (Join-Path $clientRoot 'SonalisGuardianScanner.exe')

$isccCandidates = @(
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
    'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
    'C:\Program Files\Inno Setup 6\ISCC.exe'
)
$iscc = $isccCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 ISCC.exe bulunamadı.' }

$installerName = "Sonalis-Kurulum-x64-$version"
Push-Location $clientRoot
try {
    & $iscc "/DMyAppVersion=$version" "/DMyAppVersionNumeric=$version.0" `
        '/DMyAppSourceExe=Sonalis.exe' '/DMyGuardianScannerSource=SonalisGuardianScanner.exe' `
        "/DMyOutputBaseFilename=$installerName" 'installer.iss'
    if ($LASTEXITCODE -ne 0) { throw 'Inno Setup derlemesi başarısız oldu.' }
} finally {
    Pop-Location
}

$installer = Join-Path $clientRoot "$installerName.exe"
$installerSignParameters = @{
    Path = @($installer)
    ExpectedCertificateSha256 = $certificateSha256
    TimestampUrl = $TimestampUrl
}
if ($CertificateThumbprint) { $installerSignParameters.CertificateThumbprint = $CertificateThumbprint }
else { $installerSignParameters.PfxPath = $PfxPath }
& $signScript @installerSignParameters

$artifacts = @($clientExecutable, $scannerExecutable, $installer)
foreach ($artifact in $artifacts) {
    $signature = Get-AuthenticodeSignature -LiteralPath $artifact
    if ($signature.Status -ne 'Valid') { throw "İmza doğrulanamadı: $artifact" }
    $hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash
    Write-Output "$hash  $artifact"
}

Write-Output "Sonalis Windows $version stable paketi derlendi, zaman damgalı imzalandı ve doğrulandı."
