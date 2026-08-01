# Contributing to Sonalis Client

Katkılar issue veya pull request ile yapılabilir. Katkıda bulunan kişi gönderdiği
kod için gerekli haklara sahip olduğunu ve kodun AGPL-3.0 kapsamında
yayımlanabileceğini kabul eder.

## Geliştirme akışı

1. Ayrı bir branch oluşturun.
2. Secret, kişisel veri, signing key, PFX, `.env` veya gerçek kullanıcı içeriği
   eklemeyin.
3. Windows değişikliklerinde `canary-release` preset'ini derleyin ve testleri
   çalıştırın.
4. Kullanıcıya gösterilen yeni metinleri yerelleştirme tablosuna ekleyin.
5. Ses callback'i ve UDP sıcak hattında paket başına heap allocation eklemeyin.
6. Davranış değişikliği için test ve kısa release notu ekleyin.
7. Pull request'te güvenlik, gizlilik ve kaynak tüketimi etkisini açıklayın.

```powershell
cmake --preset canary-release
cmake --build --preset canary-release
ctest --preset canary-release
```

## İnceleme

CI başarılı olmadan merge veya signing yapılmaz. Maintainer kendi release
onayından önce build logunu, testleri, üretilen hash'leri ve bağımlılık değişimini
gözden geçirir. Güvenlik açıkları için public issue yerine [SECURITY.md](SECURITY.md)
akışı kullanılır.
