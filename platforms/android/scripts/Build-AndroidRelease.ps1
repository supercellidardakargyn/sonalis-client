[CmdletBinding()]
param(
    [string]$VersionName = "5.2.0",
    [int]$VersionCode = 50200,
    [string]$ArtifactDirectory
)

$ErrorActionPreference = "Stop"
$androidRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$privateDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\..\signing\private"))
$credentialFile = Join-Path $privateDir "android-signing.dpapi"
if (-not $ArtifactDirectory) {
    $ArtifactDirectory = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\..\artifacts\android"))
}
if (-not (Test-Path $credentialFile)) {
    throw "Imza kimligi yok. Once New-AndroidSigningKey.ps1 calistirin."
}

$protected = [IO.File]::ReadAllBytes($credentialFile)
$plainBytes = [Security.Cryptography.ProtectedData]::Unprotect(
    $protected,
    $null,
    [Security.Cryptography.DataProtectionScope]::CurrentUser)
$credentials = [Text.Encoding]::UTF8.GetString($plainBytes) | ConvertFrom-Json

$gradle = $null
if (Test-Path (Join-Path $androidRoot "gradlew.bat")) {
    $gradle = Join-Path $androidRoot "gradlew.bat"
} else {
    $gradleCommand = Get-Command gradle.bat -ErrorAction SilentlyContinue
    if ($gradleCommand) { $gradle = $gradleCommand.Source }
}
if (-not $gradle) { throw "Gradle bulunamadi. Android toolchain kurulumunu tamamlayin." }

$env:SONALIS_ANDROID_KEYSTORE_PATH = $credentials.keyStorePath
$env:SONALIS_ANDROID_KEYSTORE_PASSWORD = $credentials.storePassword
$env:SONALIS_ANDROID_KEY_ALIAS = $credentials.alias
$env:SONALIS_ANDROID_KEY_PASSWORD = $credentials.keyPassword
$env:SONALIS_VERSION_NAME = $VersionName
$env:SONALIS_VERSION_CODE = $VersionCode.ToString()
try {
    Push-Location $androidRoot
    & $gradle --no-daemon --stacktrace :app:assembleRelease :app:bundleRelease
    if ($LASTEXITCODE -ne 0) { throw "Android release derlemesi basarisiz." }
} finally {
    Pop-Location
    @(
        "SONALIS_ANDROID_KEYSTORE_PATH", "SONALIS_ANDROID_KEYSTORE_PASSWORD",
        "SONALIS_ANDROID_KEY_ALIAS", "SONALIS_ANDROID_KEY_PASSWORD",
        "SONALIS_VERSION_NAME", "SONALIS_VERSION_CODE"
    ) | ForEach-Object { Remove-Item "Env:\$_" -ErrorAction SilentlyContinue }
}

[IO.Directory]::CreateDirectory($ArtifactDirectory) | Out-Null
$apk = Get-ChildItem (Join-Path $androidRoot "app\build\outputs\apk\release") -Filter "*.apk" |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
$aab = Get-ChildItem (Join-Path $androidRoot "app\build\outputs\bundle\release") -Filter "*.aab" |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if (-not $apk -or -not $aab) { throw "APK veya AAB cikti dosyasi bulunamadi." }

$apkTarget = Join-Path $ArtifactDirectory "Sonalis-$VersionName-android.apk"
$aabTarget = Join-Path $ArtifactDirectory "Sonalis-$VersionName-play.aab"
Copy-Item -LiteralPath $apk.FullName -Destination $apkTarget -Force
Copy-Item -LiteralPath $aab.FullName -Destination $aabTarget -Force
$hashes = Get-FileHash -Algorithm SHA256 $apkTarget, $aabTarget
$hashes | ForEach-Object { "$($_.Hash)  $([IO.Path]::GetFileName($_.Path))" } |
    Set-Content -Encoding ascii (Join-Path $ArtifactDirectory "SHA256SUMS.txt")
Write-Host "Android paketleri hazir: $ArtifactDirectory" -ForegroundColor Green
