[CmdletBinding()]
param(
    [string]$ToolchainRoot = "E:\SonalisToolchains",
    [switch]$AcceptAndroidSdkLicenses
)

$ErrorActionPreference = "Stop"
$jdkRoot = Join-Path $ToolchainRoot "jdk-17"
$sdkRoot = Join-Path $ToolchainRoot "android-sdk"
$downloads = Join-Path $ToolchainRoot "downloads"
$commandToolsZip = Join-Path $downloads "commandlinetools-win-15859902_latest.zip"
$commandToolsUrl = "https://dl.google.com/android/repository/commandlinetools-win-15859902_latest.zip"
$commandToolsSha256 = "90ae805d20434428bffcb699c290860f19bb5f66a67e6b330067e3de801fb04a"
$latestTools = Join-Path $sdkRoot "cmdline-tools\latest"

if (-not (Test-Path (Join-Path $jdkRoot "bin\java.exe"))) {
    throw "JDK 17 bulunamadi: $jdkRoot"
}
[IO.Directory]::CreateDirectory($downloads) | Out-Null
[IO.Directory]::CreateDirectory($sdkRoot) | Out-Null

if (-not (Test-Path $commandToolsZip)) {
    & curl.exe --fail --location --retry 3 --output $commandToolsZip $commandToolsUrl
    if ($LASTEXITCODE -ne 0) { throw "Android command-line tools indirilemedi." }
}
$actual = (Get-FileHash -Algorithm SHA256 $commandToolsZip).Hash.ToLowerInvariant()
if ($actual -ne $commandToolsSha256) {
    throw "Android command-line tools SHA-256 dogrulamasi basarisiz: $actual"
}

if (-not (Test-Path (Join-Path $latestTools "bin\sdkmanager.bat"))) {
    $stage = Join-Path $ToolchainRoot "android-command-tools-extract"
    if (Test-Path $stage) {
        throw "Yarim kalmis staging klasoru var; inceleyip kaldirin: $stage"
    }
    [IO.Directory]::CreateDirectory($stage) | Out-Null
    Expand-Archive -LiteralPath $commandToolsZip -DestinationPath $stage
    [IO.Directory]::CreateDirectory((Split-Path $latestTools -Parent)) | Out-Null
    Move-Item -LiteralPath (Join-Path $stage "cmdline-tools") -Destination $latestTools
}

$env:JAVA_HOME = $jdkRoot
$env:ANDROID_HOME = $sdkRoot
$env:ANDROID_SDK_ROOT = $sdkRoot
[Environment]::SetEnvironmentVariable("JAVA_HOME", $jdkRoot, "User")
[Environment]::SetEnvironmentVariable("ANDROID_HOME", $sdkRoot, "User")
[Environment]::SetEnvironmentVariable("ANDROID_SDK_ROOT", $sdkRoot, "User")

$sdkManager = Join-Path $latestTools "bin\sdkmanager.bat"
if (-not $AcceptAndroidSdkLicenses) {
    Write-Host "Android SDK lisanslari kullanici tarafindan kabul edilmelidir." -ForegroundColor Yellow
    Write-Host "Metinleri okuyup kabul ediyorsaniz su komutu calistirin:"
    Write-Host ".\Install-AndroidToolchain.ps1 -AcceptAndroidSdkLicenses" -ForegroundColor Cyan
    exit 2
}

$answers = (1..30 | ForEach-Object { "y" }) -join "`r`n"
$answers | & $sdkManager --licenses
if ($LASTEXITCODE -ne 0) { throw "Android SDK lisans kabul islemi tamamlanmadi." }

$sdkPackages = @(
    "platform-tools",
    "platforms;android-35",
    "build-tools;35.0.0",
    "ndk;27.2.12479018",
    "cmake;3.31.1"
)
& $sdkManager @sdkPackages
if ($LASTEXITCODE -ne 0) { throw "Android SDK/NDK kurulumu basarisiz." }

$localProperties = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\local.properties"))
$escapedSdk = $sdkRoot.Replace("\", "\\").Replace(":", "\:")
[IO.File]::WriteAllText($localProperties, "sdk.dir=$escapedSdk`n", [Text.UTF8Encoding]::new($false))
Write-Host "Android arac zinciri hazir: $sdkRoot" -ForegroundColor Green
