# Sonalis 5.x platform hazırlık matrisi

| Platform | Yerel kabuk | Güvenli saklama | Hesap/oda/mesaj | Yerel ses I/O | Şifreli relay | İmza dışı durum |
|---|---|---|---|---|---|---|
| Windows | Win32 + ImGui/DX11 | Credential Manager + DPAPI | Hazır | WASAPI + Opus + RNNoise | Hazır | Release derleme ve 12/12 test geçti |
| Linux | GTK4 | libsecret | Hazır | PipeWire 48 kHz + Opus | TLS pinli ChaCha20-Poly1305 relay | CI derleme/paket hattı hazır |
| Android | Kotlin View + JNI | Android Keystore AES-GCM | Hazır | AAudio 48 kHz | TLS pinli ChaCha20-Poly1305 relay hazır | CI unsigned APK hattı hazır |
| macOS | AppKit | ThisDeviceOnly Keychain | Hazır | AVAudioEngine + Opus | TLS pinli ChaCha20-Poly1305 relay | CI unsigned app hattı hazır |
| iOS | UIKit | ThisDeviceOnly Keychain | Hazır | AVAudioEngine/AVAudioSession + Opus | TLS pinli ChaCha20-Poly1305 relay | CI simulator app hattı hazır |

“Hazır” kaynak ve otomatik doğrulama seviyesini ifade eder. Windows dışındaki gerçek
donanım, mikrofon/hoparlör hot-swap, NAT, mağaza ve uzun süreli kabul testleri ilgili
işletim sistemi runner’ında tamamlanmadan platform stable olarak yayımlanmaz.

Ortak güvenlik ve kaynak kararları:

- Production merkezi adresi yalnız HTTPS/WSS olur.
- Uzun ömürlü token düz metin dosyada tutulmaz.
- P2P/relay/uyutma ve event-generation kararları ortak C++ çekirdektedir.
- Ortak Opus köprüsü 48 kHz mono, 20 ms, FEC ve PLC davranışını sağlar.
- Mobil mikrofon yalnız görünür aktif görüşme yaşam döngüsünde çalışır.
- İmzalar bu teslimin dışında tutulmuştur; CI yalnız unsigned artifact üretir.
