[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$clientRoot = Split-Path -Parent $PSScriptRoot
$platformRoot = Join-Path $clientRoot 'platforms'

$required = @(
    'CMakeLists.txt',
    'PLATFORM_READINESS.md',
    'linux/CMakeLists.txt',
    'linux/main.cpp',
    'linux/src/curl_http_client.cpp',
    'linux/src/linux_api.cpp',
    'linux/src/linux_voice_transport.cpp',
    'linux/src/linux_voice_call.cpp',
    'linux/src/pipewire_audio_backend.cpp',
    'linux/src/secret_service_store.cpp',
    'macos/CMakeLists.txt',
    'macos/main.mm',
    'macos/SonalisKeychainStore.swift',
    'macos/project.yml',
    'macos/App/RootViewController.swift',
    'android/settings.gradle.kts',
    'android/app/build.gradle.kts',
    'android/app/src/main/AndroidManifest.xml',
    'android/app/src/main/cpp/native_voice_session.cpp',
    'android/app/src/main/cpp/native_audio_engine.cpp',
    'android/app/src/main/cpp/native_crypto.cpp',
    'android/app/src/main/cpp/native_voice_codec.cpp',
    'android/app/src/main/java/tr/sonalis/core/NativeCore.kt',
    'android/app/src/main/java/tr/sonalis/core/SonalisSecureStore.kt',
    'android/app/src/main/java/tr/sonalis/mobile/MainActivity.kt',
    'android/app/src/main/java/tr/sonalis/mobile/SonalisApi.kt',
    'android/app/src/main/java/tr/sonalis/mobile/SonalisRealtime.kt',
    'android/app/src/main/java/tr/sonalis/mobile/SonalisVoiceTransport.kt',
    'android/app/src/main/java/tr/sonalis/mobile/AndroidVoiceCall.kt',
    'android/app/src/main/java/tr/sonalis/mobile/VoiceForegroundService.kt',
    'ios/project.yml',
    'ios/App/Info.plist',
    'ios/App/AppDelegate.swift',
    'ios/App/SceneDelegate.swift',
    'ios/App/RootViewController.swift',
    'ios/SonalisKeychainStore.swift',
    'ios/SonalisVoiceSession.swift',
    'apple-shared/SonalisAPI.swift',
    'apple-shared/SonalisRealtime.swift',
    'apple-shared/SonalisAudioEngine.swift',
    'apple-shared/SonalisCrypto.swift',
    'apple-shared/SonalisVoiceCodec.swift',
    'apple-shared/SonalisVoiceTransport.swift',
    'apple-shared/SonalisVoiceCall.swift',
    'apple-shared/build-apple-opus.sh',
    'apple-shared/prepare-apple-dependencies.sh'
)

$missing = @($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $platformRoot $_)) })
if ($missing.Count -gt 0) {
    throw "Eksik platform dosyaları: $($missing -join ', ')"
}

$sourceFiles = Get-ChildItem -LiteralPath $platformRoot -Recurse -File |
    Where-Object { $_.Extension -in @('.cpp', '.h', '.kt', '.swift', '.m', '.mm', '.xml', '.kts') }
$forbidden = Select-String -Path $sourceFiles.FullName -Pattern '\b(?:WebView|Electron|WKWebView)\b'
if ($forbidden) {
    throw "Yerel istemci kuralını ihlal eden tarayıcı motoru bulundu: $($forbidden.Path -join ', ')"
}

$androidNative = Get-Content -LiteralPath (Join-Path $platformRoot 'android/app/src/main/cpp/native_voice_session.cpp') -Raw
$iosScene = Get-Content -LiteralPath (Join-Path $platformRoot 'ios/App/SceneDelegate.swift') -Raw
if ($androidNative -notmatch 'sonalis_core_abi_version' -or $iosScene -notmatch 'sonalis_core_abi_version') {
    throw 'Mobil platform başlangıçlarında ortak ABI doğrulaması eksik.'
}

$coreAbi = Get-Content -LiteralPath (Join-Path $clientRoot 'core/include/sonalis/core/c_api.h') -Raw
if ($coreAbi -notmatch 'SONALIS_CORE_ABI_VERSION\s+2u') { throw 'Ortak C ABI sürümü beklenen 2 değil.' }

if ($coreAbi -notmatch 'sonalis_voice_encoder_encode' -or
    -not (Test-Path -LiteralPath (Join-Path $clientRoot 'core/src/c_voice_codec.cpp'))) {
    throw 'Common Opus C ABI bridge is missing.'
}

$androidVoice = Get-Content -LiteralPath (Join-Path $platformRoot 'android/app/src/main/java/tr/sonalis/mobile/SonalisVoiceTransport.kt') -Raw
if ($androidVoice -notmatch 'ChaCha20-Poly1305' -or $androidVoice -notmatch 'certificate_pin_mismatch' -or
    $androidVoice -notmatch 'replayMask') {
    throw 'Android şifreli relay, TLS pin veya replay koruması eksik.'
}
$appleAudio = Get-Content -LiteralPath (Join-Path $platformRoot 'apple-shared/SonalisAudioEngine.swift') -Raw
if ($appleAudio -notmatch 'AVAudioEngine' -or $appleAudio -match 'Array\(UnsafeBufferPointer') {
    throw 'Apple AVAudioEngine hattı eksik veya callback içinde PCM dizi tahsisi yapıyor.'
}
$linuxAudio = Get-Content -LiteralPath (Join-Path $platformRoot 'linux/src/pipewire_audio_backend.cpp') -Raw
if ($linuxAudio -notmatch 'info\.rate\s*=\s*48''000' -or $linuxAudio -notmatch 'PW_STREAM_FLAG_RT_PROCESS') {
    throw 'Linux PipeWire 48 kHz ses hattı doğrulanamadı.'
}
$linuxVoice = Get-Content -LiteralPath (Join-Path $platformRoot 'linux/src/linux_voice_transport.cpp') -Raw
$appleVoice = Get-Content -LiteralPath (Join-Path $platformRoot 'apple-shared/SonalisVoiceTransport.swift') -Raw
if ($linuxVoice -notmatch 'EVP_chacha20_poly1305' -or $linuxVoice -notmatch 'certificate_pin_mismatch') {
    throw 'Linux encrypted relay or certificate pin is missing.'
}
if ($appleVoice -notmatch 'ChaChaPoly' -or $appleVoice -notmatch 'certificateMismatch') {
    throw 'Apple encrypted relay or certificate pin is missing.'
}

$manifest = [xml](Get-Content -LiteralPath (Join-Path $platformRoot 'android/app/src/main/AndroidManifest.xml') -Raw)
$cleartext = $manifest.manifest.application.usesCleartextTraffic
if ($cleartext -ne 'false') { throw 'Android cleartext ağ trafiği kapalı değil.' }

Write-Output "Sonalis platform kaynakları doğrulandı: $($required.Count) zorunlu dosya, yerel UI, ABI, ses ve güvenli ağ kontrolleri hazır."
