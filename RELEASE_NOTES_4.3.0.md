# Sonalis Client 4.3.0 Canary

- Aktif bağlantıda entitlement bitrate değişikliği Opus encoder'a atomik uygulanır.
- Çoklu konuşmacı mixer'ı sert kırpma yerine allocation-free blok sınırlayıcı kullanır.
- Sınırlayıcı ani taşmayı anında bastırır ve sesi yaklaşık 800 ms içinde doğal seviyeye döndürür.
- 4.2 kaynak profilleri ve çalışma belleği telemetrisi korunur.

Eşlik eden Voice Node 3.4.7, sunucu RNNoise yeniden kodlamasında 48 kbit/s kalite tabanı,
adaptif kuru/filtreli karışım ve seviye koruma kullanır.
