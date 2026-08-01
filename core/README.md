# Sonalis ortak istemci çekirdeği

Bu dizin platform UI ve ses cihazı API'lerinden bağımsız C++20 sözleşmelerini içerir. Sabit ses biçimi 48 kHz, mono ve 20 ms/960 örnektir. Windows WASAPI, Linux PipeWire, macOS/iOS AVAudioEngine ve Android Oboe katmanları aynı `AudioBackend`, `VoiceTransport` ve `SecureStore` sınırlarını uygular.

Gerçek zamanlı callback'lerde sınırsız kuyruk, disk erişimi veya paket başına heap tahsisi kullanılmaz. Platform UI'ları hesap, oda ve mesaj API'sini paylaşır; mobilde arka plan çalışması yalnız etkin görüşme sırasında işletim sistemi izinleri içinde sürer.

`VoiceSession` durum makinesi iki katılımcıda on saniyelik kararlılıktan sonra
P2P probe başlatır, probe başarısızlığında relay'e döner, server RNNoise açıkken
P2P'yi reddeder ve tek katılımcıda altmış saniye sonra yalnız medya kaynağını
uyutur. Durum makinesi saat veya socket sahibi değildir; böylece dört platformda
aynı kararlar deterministik olarak test edilebilir.

`c_api.h`, durum makinesini ABI-kararlı ve istisna geçirmeyen bir C sınırıyla
açar. Android JNI ile macOS/iOS Swift köprüleri bu arayüzü kullanır; platform
katmanları C++ sınıf düzenine veya STL ABI'sine bağlanmaz. Opaque handle yalnız
onu oluşturan platform worker'ında kullanılmalıdır.

`EventGeneration`, bir yenileme sürerken gelen ikinci realtime olayını tek bir
sonraki çalışmaya birleştirir; olayları sessizce düşürmez ve sınırsız iş kuyruğu
oluşturmaz. Aynı davranış C ABI üzerinden mobil kabuklara da açıktır.

`PlatformHttpClient`, `RealtimeBackend`, `PlatformNotifier` ve `SecureStore`
sözleşmeleri işletim sistemi servislerini çekirdek durum makinelerinden ayırır.
Platform adaptörleri uzun ömürlü tokenı düz metin dosyada saklayamaz ve production
trafiğinde cleartext merkezi origin kullanamaz.
