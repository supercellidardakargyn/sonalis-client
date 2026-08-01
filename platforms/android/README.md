# Sonalis Android

Android 10+ (`minSdk 29`) yerel Kotlin View arayüzü, JNI ortak C++20 çekirdek,
AAudio 48 kHz ses hattı ve Android Keystore kullanır. WebView yoktur.

Uygulananlar:

- HTTPS hesap, oda, kanal, mesaj ve voice grant API’si.
- Generation güvenli WebSocket reconnect ve 25 saniye heartbeat.
- Keystore AES-256-GCM ile refresh token/anahtar zarfı saklama.
- AAudio düşük gecikmeli capture/render ve mikrofon yoksa listen-only.
- Opus 20 ms encode/decode, VAD, TalkStart, FEC/PLC çekirdeği.
- Leaf sertifika pini, ChaCha20-Poly1305 UDP, replay penceresi ve heartbeat.
- Etkin görüşmede zorunlu foreground-service bildirimi.

P2P ICE adaptörü henüz bulunmadığı için Android voice grant’i güvenli biçimde relay
ister; başarısız P2P deneyi çalışan sesi kesemez. CI unsigned debug APK üretir.
Release keystore, Play imzası ve fiziksel cihaz kabulü bu teslimin dışındadır.
