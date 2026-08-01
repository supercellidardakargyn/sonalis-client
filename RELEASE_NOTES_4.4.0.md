# Sonalis Client 4.4.0 Canary

- Gönderilen mesajlar anında `Gönderiliyor` durumuyla konuşmada görünür.
- Geçici ağ hatasında aynı şifreli mesaj kimliği bir kez daha kullanılarak güvenli ve idempotent gönderim denenir.
- Başarısız mesaj taslağı korunur ve satır `Gönderilemedi` olarak işaretlenir.
- Kanal listesinde okunmamış mesaj ile doğrudan bahsetme sayaçları ayrı rozetlerdir.
- Aktif kanal okunduğunda yerel sayaçlar beklemeden temizlenir; arka plandaki gerçek zamanlı olaylar sayaçları anında artırır.
- Mesaj geçmişi, cursor senkronizasyonu, şifreli ekler, reaksiyonlar, sabitleme, yazıyor bilgisi ve kanal bildirim tercihleri korunur.

Bu canary paketi Authenticode imzası ve gerçek iki-hesaplı uçtan uca kabul testi tamamlanmadan stable kanalına yayımlanmaz.
