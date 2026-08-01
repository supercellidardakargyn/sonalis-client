# Sonalis macOS

macOS 13+ için AppKit, AVAudioEngine, Network, Security ve ortak C++20 çekirdek
kullanan yerel uygulamadır. API, Keychain ThisDeviceOnly saklama, realtime heartbeat,
48 kHz mono ses dönüştürme, Opus, leaf sertifika pinli ChaCha20-Poly1305 relay ve
allocation-free capture callback’i uygulanmıştır.

`../apple-shared/prepare-apple-dependencies.sh` Monocypher 4.0.2 kaynağını sabit
SHA-256 ile doğrular. CI XcodeGen projesini üretir, kod imzasını kapatarak Release
uygulamasını derler ve zip artifact oluşturur.

Cihaz hot-swap ve gerçek donanım kabulü macOS runner’ında tamamlanmadan stable
sayılmaz. Developer ID/notarizasyon kapsam dışıdır.
