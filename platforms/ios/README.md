# Sonalis iOS

iOS 17+ için UIKit, AVAudioEngine/AVAudioSession, Network, Keychain ve ortak C++20
çekirdek kullanan native uygulamadır. WebView yoktur.

Hesap/oda/kanal/mesaj API’si, refresh token yenileme, ThisDeviceOnly Keychain,
realtime heartbeat, aktif görüşmeye bağlı background-audio yapılandırması, deep-link
48 kHz ses capture/render, Opus ve leaf sertifika pinli ChaCha20-Poly1305 relay
uygulanmıştır. CI XcodeGen ile unsigned simulator app üretir.

CallKit son kabulü ve fiziksel cihaz testi iOS ortamında tamamlanmadan stable
sayılmaz. Apple imzası bu teslimin dışındadır.
