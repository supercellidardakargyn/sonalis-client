# Sonalis Client 4.5.0 Canary

- Şifreli mesaj ekleri artık doğrudan mesaj menüsünden Guardian'a bildirilebilir.
- İstemci kanıt görselini yalnız bellekte ve geçici dosyada açar, moderasyon public key'i ile yeniden mühürler ve cihaz Ed25519 anahtarıyla imzalar.
- Merkezi sistem mesaj plaintext'ini veya konuşma anahtarını almaz; yalnız ayrı moderasyon anahtarıyla açılabilen kanıt paketi gönderilir.
- Rapor nedeni ve isteğe bağlı açıklama native arayüzden seçilebilir.
- Kullanıcı son gönderdiği raporu geri çekebilir. Tek bağımsız rapora bağlı otomatik kısıtlama, manuel moderatör onayı yoksa sunucu tarafından geri alınır.
- Geçici kanıt dosyaları işlem sonunda kaldırılır; plaintext ve mühürlü kanıt tamponları bellekten silinir.

4.4 mesaj teslim durumu, idempotent ağ tekrarı ve mention rozetleri bu sürüme dahildir.
