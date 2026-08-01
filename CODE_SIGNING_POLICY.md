# Sonalis Client code-signing policy

Free code signing provided by SignPath.io, certificate by SignPath Foundation

## Kapsam

Bu politika yalnız `sonalis-client` açık kaynak deposundan üretilen Sonalis
istemci artifact'lerini kapsar. Merkezi sunucu, voice node, ödeme, yönetim
paneli veya herhangi bir kapalı kaynak bileşen imzalı pakete dahil edilmez.

İmzalanabilecek Windows artifact'leri:

- `Sonalis.exe`
- `SonalisGuardianScanner.exe`
- `Sonalis-Kurulum-x64-<version>.exe`

## Kaynak ve derleme

- İmzalı artifact'in bütün kaynak kodu ve derleme betikleri public depoda yer alır.
- Bağımlılık sürümleri ve indirilen arşiv hash'leri `CMakeLists.txt` içinde sabittir.
- CI derlemesi temiz Git commit'inden yapılır ve commit SHA, kanal, sürüm ve SHA-256
  değerleri release kaydında tutulur.
- Secret, signing private key, PFX veya kişisel veri depoya ve artifact'e eklenmez.
- İmzalama yalnız CI testleri geçtikten sonra ve release onayıyla yapılır.

## Roller ve değişiklik kontrolü

- Committer/reviewer: `supercellidardakargyn`
- Release approver: `supercellidardakargyn`
- Depo ve imzalama hizmetinde çok faktörlü kimlik doğrulama zorunludur.
- Release onayı commit oluşturma işleminden ayrı bir adımdır.
- Proje büyüdüğünde aynı kişinin kendi değişikliğini tek başına onaylamaması için
  ikinci maintainer ve zorunlu pull-request review kuralı etkinleştirilecektir.

## Sürüm ve iptal

- Her release semantik sürüm etiketi taşır.
- İmza, SHA-256, artifact boyutu ve kaynak commit'i yayımlanır.
- Şüpheli veya hatalı release geri çekilir; aynı sürüm numarası yeniden kullanılmaz.
- Signing hesabı ya da anahtarı tehlikeye girerse SignPath Foundation ile iptal
  süreci başlatılır ve güvenlik duyurusu yayımlanır.

## Doğrulama

Windows paketleri Authenticode ve RFC 3161 zaman damgasıyla doğrulanır. İstemci
güncelleyicisi ayrıca Ed25519 manifest imzasını, SHA-256 değerini, dosya boyutunu,
ürünü, kanalı ve sürümü doğrular.
