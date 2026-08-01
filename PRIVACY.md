# Sonalis Client privacy notice

Son güncelleme: 1 Ağustos 2026

Bu belge açık kaynak Sonalis istemcisinin cihazda ve ağ üzerinde yaptığı işlemleri
açıklar. Sonalis hizmetinin tam gizlilik ve KVKK metinleri https://sonalis.tr
üzerinde yayımlanır; bu belge onların yerine geçmez.

## Cihazda tutulan veriler

- Pencere, dil, ses cihazı ve kullanıcı tercihleri
  `%LocalAppData%\Sonalis\settings.json` altında tutulur.
- Refresh token Windows Credential Manager ile; mesaj cihaz anahtarları DPAPI veya
  desteklenen cihazlarda dışa aktarılamaz TPM anahtarıyla korunur.
- Tanılama günlükleri `%LocalAppData%\Sonalis\logs\` altında en fazla iki gün ve
  dosya başına 512 KB sınırıyla tutulur.
- Parola saklanmaz. Mesaj plaintext'i kalıcı yerel arşive yazılmaz.

## Hizmete gönderilen veriler

İstemci, kullanıcının yapılandırdığı merkezi Sonalis HTTPS adresine hesap ve oturum
istekleri, oda/arkadaş metadata'sı, şifreli mesajlar, cihaz public key'i ve gerekli
işletimsel sayaçları gönderir. Ses, seçili voice node'a şifreli UDP paketleriyle
gider. İki kişilik odada P2P seçeneği açıksa katılımcılar bağlantı kurabilmek için
birbirlerinin public ağ adaylarını görebilir.

Uygulama açılışta ve açıkken her on dakikada imzalı güncelleme manifestini kontrol
eder. Paket yalnız güncelleme akışı başlatıldığında indirilir.

## Hata tanılaması

Kimliği doğrulanmış istemci, hata oluştuğunda bileşen, güvenli hata kodu, önem,
istemci sürümü, işletim sistemi ailesi, tekrar sayısı ve kimliği ayıklanmış kısa
bağlamı `/api/v1/diagnostics/errors` adresine gönderebilir. Parola, token, API key,
mesaj plaintext'i, kriptografi anahtarı, e-posta benzeri değer ve tam IP bağlama
eklenmemek üzere kod seviyesinde ayıklanır. Tanılama gönderimi ses veya arayüz
çalışmasını engellemez.

Kullanıcı Tanılama ekranından yerel rapor dışa aktarabilir; bu işlem kullanıcı
eylemi olmadan dosya oluşturmaz.

## Guardian medya incelemesi

Guardian özelliği etkinleştirildiğinde seçilen görsel yerel modelle incelenebilir.
Özel medya raporu yalnız kullanıcının açık raporlama eylemiyle gönderilir. Şifreli
kanıt, rapor kimliği ve doğrulama metadata'sı bu akışta hizmete aktarılabilir.

## Kontrol ve silme

Hesap/oturumlar web panelinden yönetilebilir. Yerel ayar ve günlükler uygulama
kaldırıldıktan sonra `%LocalAppData%\Sonalis` klasörü silinerek temizlenebilir.
Hesap verisi erişim/silme talepleri için https://sonalis.tr/support kullanılabilir.

## Kaynak doğrulaması

Bu davranışların uygulanışı açık kaynakta `diagnostics.cpp`, `platform_api.cpp`,
`update_service.cpp`, `credential_vault.cpp` ve mesaj/ses hattı dosyalarında
incelenebilir.
