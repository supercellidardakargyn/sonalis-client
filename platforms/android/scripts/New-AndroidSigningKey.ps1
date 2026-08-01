[CmdletBinding()]
param(
    [string]$Alias = "sonalis-upload",
    [string]$Company = "Sonalis Iletisim Teknolojileri A.S.",
    [string]$CountryCode = "TR",
    [int]$ValidityDays = 10000
)

$ErrorActionPreference = "Stop"

function Find-KeyTool {
    $candidates = @()
    if ($env:JAVA_HOME) { $candidates += (Join-Path $env:JAVA_HOME "bin\keytool.exe") }
    $command = Get-Command keytool.exe -ErrorAction SilentlyContinue
    if ($command) { $candidates += $command.Source }
    $candidates += Get-ChildItem "E:\SonalisToolchains" -Filter keytool.exe -Recurse -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty FullName
    $found = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $found) { throw "keytool bulunamadi. Once JDK 17 kurun veya JAVA_HOME ayarlayin." }
    return $found
}

function New-RandomSecret {
    $bytes = [byte[]]::new(32)
    [Security.Cryptography.RandomNumberGenerator]::Fill($bytes)
    return [Convert]::ToBase64String($bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_')
}

$keyTool = Find-KeyTool
$privateDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\..\signing\private"))
$publicDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\..\signing\public"))
[IO.Directory]::CreateDirectory($privateDir) | Out-Null
[IO.Directory]::CreateDirectory($publicDir) | Out-Null

$keyStore = Join-Path $privateDir "sonalis-android-upload.p12"
$credentialFile = Join-Path $privateDir "android-signing.dpapi"
$certificateFile = Join-Path $publicDir "sonalis-android-upload-certificate.pem"
if (Test-Path $keyStore) { throw "Mevcut anahtar ezilmeyecek: $keyStore" }

$password = New-RandomSecret
$env:SONALIS_KEYSTORE_PASSWORD_TEMP = $password
try {
    & $keyTool -genkeypair -v -storetype PKCS12 -keystore $keyStore `
        -storepass:env SONALIS_KEYSTORE_PASSWORD_TEMP -keypass:env SONALIS_KEYSTORE_PASSWORD_TEMP `
        -alias $Alias -keyalg RSA -keysize 4096 -sigalg SHA256withRSA `
        -validity $ValidityDays -dname "CN=Sonalis Android, OU=Software, O=$Company, C=$CountryCode"
    if ($LASTEXITCODE -ne 0) { throw "Android upload anahtari uretilemedi." }

    & $keyTool -exportcert -rfc -keystore $keyStore -alias $Alias `
        -storepass:env SONALIS_KEYSTORE_PASSWORD_TEMP -file $certificateFile
    if ($LASTEXITCODE -ne 0) { throw "Android public sertifikasi disari aktarilamadi." }

    $payload = @{
        keyStorePath = $keyStore
        alias = $Alias
        storePassword = $password
        keyPassword = $password
        createdAt = [DateTimeOffset]::UtcNow.ToString("O")
    } | ConvertTo-Json -Compress
    $plainBytes = [Text.Encoding]::UTF8.GetBytes($payload)
    $protected = [Security.Cryptography.ProtectedData]::Protect(
        $plainBytes,
        $null,
        [Security.Cryptography.DataProtectionScope]::CurrentUser)
    [IO.File]::WriteAllBytes($credentialFile, $protected)
}
finally {
    Remove-Item Env:\SONALIS_KEYSTORE_PASSWORD_TEMP -ErrorAction SilentlyContinue
}

Write-Host "Android upload anahtari olusturuldu." -ForegroundColor Green
Write-Host "Private keystore : $keyStore"
Write-Host "DPAPI kimlik     : $credentialFile"
Write-Host "Public sertifika : $certificateFile"
Write-Host ""
Write-Host "OFFLINE YEDEK PAROLASI (yalniz simdi gosterilir):" -ForegroundColor Yellow
Write-Host $password -ForegroundColor Yellow
Write-Host "Bu parolayi ve P12 dosyasini iki ayri sifreli yedekte saklayin." -ForegroundColor Yellow
