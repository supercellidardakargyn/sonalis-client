# Sonalis çoklu platform istemcileri

Bu dizindeki istemciler web motoru kullanmaz. Hepsi `client/core` içindeki ABI-kararlı
C sınırını, aynı oturum/event-generation durum makinelerini, taşınabilir kriptografiyi
ve Opus codec sözleşmesini kullanır.

- `linux/`: GTK4, libsecret ve PipeWire kullanan yerel masaüstü istemcisi.
- `macos/`: AppKit, AVAudioEngine, Network ve Keychain kullanan uygulama.
- `android/`: Kotlin View + JNI, AAudio ve Android Keystore kullanan uygulama.
- `ios/`: UIKit, AVAudioEngine/AVAudioSession, Network ve Keychain kullanan uygulama.

`.github/workflows/native-platform-clients.yml` Linux paketini, unsigned Android APK’yi,
unsigned macOS uygulamasını ve unsigned iOS simulator uygulamasını platformun kendi
runner’ında derler. Secret veya production parolası kaynakta bulunmaz.
