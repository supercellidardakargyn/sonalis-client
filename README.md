# Sonalis 5.2.0 — Windows istemcisi

Windows 10/11 x64 için saf C++20, Win32, Dear ImGui/DX11 ve WASAPI istemcisidir. Electron, WebView, PWA, tarayıcı motoru ve .NET çalışma zamanı içermez.

Bu depo Sonalis istemcisinin bağımsız açık kaynak kodudur. Kod
[GNU Affero General Public License v3.0](LICENSE) ile yayımlanır. Sonalis merkezi
sunucusu, voice node, ödeme ve yönetim altyapısı bu deponun ve lisansın kapsamında
değildir. Kaynak kodu: https://github.com/supercellidardakargyn/sonalis-client

Güvenlik açığı bildirmek için [SECURITY.md](SECURITY.md), katkıda bulunmak için
[CONTRIBUTING.md](CONTRIBUTING.md), veri işleme ayrıntıları için
[PRIVACY.md](PRIVACY.md) ve release imzalama zinciri için
[CODE_SIGNING_POLICY.md](CODE_SIGNING_POLICY.md) belgelerine bakın.

5.2 serisi; Aurora kaynak profilleri, Guardian şifreli medya raporlama, gerçek
zamanlı mesaj retry/idempotency, cihaz ve oturum yönetimi, ortak platform C ABI'si
ve event-generation tabanlı kayıpsız senkronizasyon içerir. Linux, macOS, Android
ve iOS kaynak temelleri `platforms/` altında ayrı platform kabul kapılarıyla tutulur.

## Teslim dosyaları

- `Sonalis.exe`: taşınabilir native istemci
- `Sonalis-Kurulum-x64.exe`: kullanıcı düzeyinde Inno Setup x64 kurulum paketi

Kurulum varsayılan olarak `%LocalAppData%\Programs\Sonalis` yolunu kullanır ve yönetici izni istemez. Kurulum yeri değiştirilebilir; Başlat menüsü/Windows araması, isteğe bağlı masaüstü kısayolu, kaldırma kaydı ve `sonalis://join/<kod>` deep-link protokolü eklenir.

Depodaki eski binary'ler Authenticode ile imzalanmamış olabilir ve genel dağıtım
için kullanılamaz. Üretim yapılandırması `SONALIS_REQUIRE_AUTHENTICODE=ON` ve
imzalayan sertifikanın SHA-256 izi olan `SONALIS_AUTHENTICODE_CERT_SHA256`
olmadan oluşturulmaz. Başlangıç doğrulaması yalnız herhangi bir geçerli imzayı
değil, bu sabitlenmiş Sonalis yayıncı sertifikasını kabul eder. EXE ve kurulum
paketi aynı güvenilir kod imzalama sertifikasıyla imzalanmadan stable yayın
yapılmamalıdır.

## Arayüz ve responsive düzen

İstemci dört açıkça etiketlenmiş sayfa kullanır: **Ses**, **Odalar**, **Mesajlar** ve **Ayarlar**. Üst çubuk seçili oda, bağlantı durumu ve hesap menüsünü; sağ alt durum düğmesi güncelleme durumunu gösterir.

- DPI uyumlu minimum pencere boyutu: `960×640`
- Başlangıç boyutu: yaklaşık `1240×780`
- `1280 px` ve üzeri: liste, ana içerik ve üye paneli birlikte gösterilebilir
- `960–1279 px`: ikincil listeler etiketli yan panel düğmeleriyle açılır
- Sayfalar bağımsız kaydırılır; alt eylemler pencere dışında kalmaz
- Pencere boyutu, konumu ve büyütülmüş durumu ayarlarda saklanır
- Ekran dışına taşmış kayıtlar güvenli bir monitör konumuna alınır

Segoe UI ve gerekli Türkçe karakterler tek font atlasında yüklenir. İkonlar ayrı font kullanmadan ImGui çizim komutlarıyla üretilir.

## Güvenli açılış ve renderer kurtarma

Uygulama ilk pencereyi ses cihazı, oturum ve ağ hazırlığını beklemeden çizer. DX11
donanım aygıtı kurulamazsa WARP kurtarma renderer'ına geçer; font atlası veya aygıt
kaybı yarım kaynak bırakmadan yeniden hazırlanır. Tepsiye küçültüldüğünde renderer
kapatılmaz, yalnız frame üretimi durur.

Kurtarma komutları:

```text
Sonalis.exe --safe-ui
Sonalis.exe --reset-ui
Sonalis.exe --disable-gpu
```

`--reset-ui` hesap ve kriptografi anahtarlarını silmeden yalnız pencere/UI
yerleşimini sıfırlar. Başlangıç ve renderer tanılamaları
`%LocalAppData%\Sonalis\logs\` altında iki gün ve dosya başına 512 KB sınırıyla
tutulur; secret, token, mesaj plaintext'i ve tam IP adresi kaydedilmez.

## Topluluklar, kanallar ve mesajlaşma

İstemci merkezi HTTPS API’ye kullanıcı hesabıyla giriş yapar; parola saklanmaz. Yenileme tokenı Windows Credential Manager ile, mesaj cihaz anahtarları DPAPI ile korunur. Native giriş imzası desteklenen bilgisayarda Microsoft Platform Crypto Provider içindeki dışa aktarılamaz TPM ECDSA P-256 anahtarını kullanır; TPM yoksa DPAPI korumalı Ed25519 geriye uyumlu yolu seçilir. Kullanıcı yalnız üyesi, yöneticisi veya sahibi olduğu odaları görür ve `SS-XXXX-XXXX` davet koduyla katılabilir.

Bir Sonalis odası bir topluluktur. Topluluklar kategori, metin kanalı ve birbirinden
bağımsız ses kanalları içerir. Owner/Admin kanal ve kategori yönetebilir; Mod
mesaj/ses moderasyonu yapabilir. Kanal bildirimleri tüm mesajlar, mention veya
süreli/süresiz sessiz seçeneklerini destekler.

Oda ve arkadaş DM mesajları WebSocket üzerinden gerçek zamanlıdır. Yanıtlama, 15 dakikalık düzenleme, silme, altı tepki, sabitleme, okunmamış sayısı, mention, yazıyor bilgisi, kararlı cursor senkronizasyonu ve bağlantı kesilince kontrollü HTTP fallback bulunur. Açık konuşmada en fazla 300 çözülmüş mesaj ve 1 MB geçici bellek tutulur; konuşma değişince topluca bırakılır.

Mesaj içeriği XChaCha20-Poly1305 ile şifrelenir ve cihaz Ed25519 anahtarıyla imzalanır. Merkezi sunucu yalnız ciphertext saklar. Yeni üyeye eski epoch ciphertext’i API seviyesinde gönderilmez. Bu sürüm görsel/dosya mesajı veya kalıcı yerel mesaj arşivi içermez.

## Ses hattı

- 48 kHz mono WASAPI event-driven capture/render
- 20 ms Opus VoIP paketleri, VBR, DTX, FEC ve PLC
- Adaptif gürültü tabanlı VAD; sessizlikte ses upload’u yok, yalnız heartbeat
- Kullanıcı başına jitter buffer, düşük gecikmeli mixer ve paket kaybı göstergesi
- RMS: `sqrt(sum(x²)/N)`, arayüzde dBFS bara dönüştürülür
- Mikrofon kapatma, bas-konuş ve otomatik konuşma algılama
- Kullanıcı başına yerel ses seviyesi ve susturma
- Bağlantı kopmadan mikrofon ve çıkış cihazı hot-swap

RNNoise seçenekleri **Kapalı**, **Yerel** ve hak/node kapasitesi varsa **Sunucu** modudur. Deneysel GPU seçeneği yalnız uyumlu bir backend algılanıp başlangıç benchmark’ını geçerse gösterilecek şekilde tasarlanmıştır; bu paket GPU RNNoise backend’i içermez.

## Ağ güvenliği

Merkezi API HTTPS kullanır. Oda düğümüne kısa ömürlü imzalı join biletiyle TLS üzerinden katılınır. Join grant node leaf sertifikasının SHA-256 pinini taşır; yalnız bu tek voice `/join` çağrısı kendinden imzalı sertifikaya izin verir ve WinHTTP'nin sunduğu leaf sertifikayı kesin pinle doğrular. Merkezi API, realtime ve güncelleme çağrıları Windows güven deposunu normal biçimde doğrulamaya devam eder. UDP ses paketleri ChaCha20-Poly1305 ile korunur; paket sıra numarası, replay penceresi ve kimliksiz paketi erken bırakma bulunur. Varsayılan düğüm portu TCP ve UDP için `25565`’tir.

## Ayarlar ve güncelleme

Ayarlar `%LocalAppData%\Sonalis\settings.json` altında saklanır. Güncelleme servisi:

- Uygulama açılışında bir kez,
- Uygulama açıkken her 10 dakikada,
- Kullanıcı sağ alttaki durum düğmesine bastığında

`/api/v1/releases/latest?product=client&manifestVersion=2&channel=<stable|canary>` endpoint’ini kontrol eder.
Manifest v1 yalnız geçiş uyumluluğu içindir; güncel istemci her zaman imzalı v2 manifestini ister.

Yeni sürüm yalnız kullanıcı **Güncelle** düğmesine bastığında indirilir. Paket geçici dosyaya akış halinde yazılır, artımlı SHA-256 hesaplanır ve Ed25519 imzası doğrulanır. Kurulum ayrıca son kullanıcı onayı ister. Hatalı hash/imza, yarım indirme ve downgrade reddedilir; güncelleme hatası devam eden ses görüşmesini kesmez.

Release public key CMake yapılandırmasına verilmelidir:

```powershell
cmake -S . -B build -A x64 -DSONALIS_RELEASE_PUBLIC_KEY_BASE64="PUBLIC_KEY_BASE64"
cmake --build build --config Release --target Sonalis
```

## Kaynaktan Release derleme

Visual Studio 2022 Build Tools, Windows 10/11 SDK, CMake ve Inno Setup gerekir:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --target Sonalis
iscc installer.iss
```

CMake bağımlılık sürümlerini ve indirme hash’lerini sabitler; libopus, RNNoise, Monocypher ve MSVC runtime statik bağlanır. Release yapılandırması `/W4 /WX /GL /LTCG /OPT:REF /OPT:ICF` kullanır.

## Windows stable imzalama

Genel dağıtıma uygun stable paket, private key içeren güvenilir bir Authenticode
sertifikası Windows kullanıcı sertifika deposuna kurulduktan sonra tek komutla
üretilir:

```powershell
$env:SONALIS_SIGN_CERT_THUMBPRINT = '<sertifikanın SHA-1 thumbprint değeri>'
.\scripts\build-signed-stable.ps1
```

PFX kullanılıyorsa dosya ve parola yalnız süreç ortamında verilir; PFX depoya
eklenmez. Betik sertifikanın SHA-256 izini istemciye sabitler, x64 stable EXE ile
Guardian tarayıcısını derleyip zaman damgalı imzalar, kurulum paketini oluşturur,
onu da imzalar ve bütün imzaları tekrar doğrular.

## Düşük kaynak profili

Arayüz olay odaklıdır: kullanıcı girdisi ve durum değişikliği anında çizilir; aktif ses barları en çok 15 FPS, odak dışı pencere 4 FPS, minimize pencere 0 FPS kullanır. DXGI frame-latency wait handle ile en fazla bir frame kuyruğa alınır. HTTP, JSON ve mesaj işleri sınırlı bir worker kuyruğunda UI thread’inden ayrılır. Güncelleme zamanlayıcısı busy-loop oluşturmaz; çizim döngüsünü yalnız sıradaki kontrol zamanı geldiğinde uyandırır.

## Doğrulama notu

3.5.2 teslimi x64 Release MSVC `/W4 /WX /GL /LTCG` derlemesi, istemci birim testleri ve statik PE/import incelemesiyle doğrulanmalıdır. Makine-geneli mutex bütün Windows oturumlarında ikinci Sonalis sürecini reddeder; merkezi cihaz bağı aynı Windows kurulumunun ikinci hesaba bağlanmasını engeller. Tek kullanıcı kalan ses kanalı 60 saniye sonunda node tarafından kapatıldığında istemci açık bir durum mesajı gösterir. Gerçek çok kullanıcılı ses kalitesi, paket kaybı/failover ve uzun süreli yük ölçümleri staging ortamında ayrıca yapılmalıdır. “Neredeyse sıfır kaynak” mutlak bir garanti değildir; gerçek kullanım donanıma, sürücüye ve aktif kullanıcı sayısına bağlıdır.
## 3.5.0 kısa not

Istemci on iki dil secimi destekler; dil adlari secim kutusunda kendi dillerinde gosterilir. Iki kisilik odalarda sabitlenmis libjuice 1.7.1 ile STUN/ICE dogrudan yolu denenir; STUN hazir degilse mevcut gozlemlenmis UDP aday yolu kullanilir ve her iki durumda da basarisizlikta gorusme otomatik Voice1 relay fallback ile devam eder. TURN uzerinden medya tasinmaz. Native API istekleri server-imzali cihaz lisansi tasir; kopyalanmis binary lisanssiz API kullanamaz.
