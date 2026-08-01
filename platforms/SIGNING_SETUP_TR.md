# Sonalis platform imzalama ve tek tik paket rehberi

Bu belge private anahtarlarin kaynak koda, release ZIP'ine veya sohbet mesajina
yazilmadan dort platform paketinin uretilmesini tanimlar. Imzali workflow:
`.github/workflows/release-platform-clients.yml`.

## Cikti dosyalari

| Platform | Kullaniciya verilecek dosya | Not |
|---|---|---|
| Linux | `Sonalis-<surum>-*.deb` / `.rpm` | Paket yoneticisiyle tek tik kurulur; `SHA256SUMS.asc` GPG imzasidir. |
| Android | `Sonalis-<surum>-android.apk` | Dogrudan kurulum; ayni anahtar gelecekteki guncellemelerde korunmalidir. |
| Android Play | `Sonalis-<surum>-play.aab` | Google Play'e yuklenir; telefona dogrudan kurulmaz. |
| macOS | `Sonalis-<surum>-macOS.dmg` | Developer ID imzali ve Apple tarafindan notarize edilir. |
| iOS | `Sonalis-<surum>-iOS.ipa` | TestFlight/App Store'a yuklenir; rastgele cihaza dogrudan kurulamaz. |

## Android yerel kurulum

1. Android SDK lisansini okuyun ve kabul ediyorsaniz:

```powershell
cd client\platforms\android\scripts
.\Install-AndroidToolchain.ps1 -AcceptAndroidSdkLicenses
```

2. Uygulama Google Play'de daha once yayinlanmadiysa tek seferlik upload anahtari:

```powershell
.\New-AndroidSigningKey.ps1
```

3. Imzali APK ve AAB:

```powershell
.\Build-AndroidRelease.ps1 -VersionName 5.2.0 -VersionCode 50200
```

Keystore `client/platforms/signing/private/` altindadir ve Git tarafindan engellenir.
P12 ile konsolda bir defa gosterilen kurtarma parolasini iki ayri sifreli, cevrimdisi
yedekte tutun. Uygulama Play'de zaten yayinlandiysa yeni key uretmeyin; mevcut upload
key'i kullanin veya Play Console'da resmi key reset akisini tamamlayin.

## GitHub `platform-signing` ortami

Private repoda `Settings > Environments > New environment` ile `platform-signing`
olusturun, gerekli reviewer ekleyin ve asagidaki environment secret'larini tanimlayin.
Dosyalar PowerShell'de `[Convert]::ToBase64String([IO.File]::ReadAllBytes(...))`
ile Base64'e cevrilebilir. Base64 sifreleme degildir; yalniz GitHub secret alanina
yapistirilmalidir.

### Linux

- `LINUX_GPG_PRIVATE_KEY_BASE64`
- `LINUX_GPG_PASSPHRASE`

### Android

- `ANDROID_KEYSTORE_BASE64`
- `ANDROID_KEYSTORE_PASSWORD`
- `ANDROID_KEY_ALIAS` (`sonalis-upload`)
- `ANDROID_KEY_PASSWORD`

### macOS dogrudan dagitim

- `APPLE_TEAM_ID`
- `MACOS_DEVELOPER_ID_P12_BASE64` (Developer ID Application sertifikasi)
- `MACOS_DEVELOPER_ID_P12_PASSWORD`
- `APPLE_NOTARY_P8_BASE64` (App Store Connect API key)
- `APPLE_NOTARY_KEY_ID`
- `APPLE_NOTARY_ISSUER_ID`

### iOS TestFlight/App Store

- `APPLE_TEAM_ID`
- `IOS_DISTRIBUTION_P12_BASE64` (Apple Distribution sertifikasi)
- `IOS_DISTRIBUTION_P12_PASSWORD`
- `IOS_PROVISIONING_PROFILE_BASE64` (`tr.sonalis.mobile` App Store profili)

## Apple hesabinda olusturulacaklar

Apple Developer Program uyeligi olmadan fiziksel cihaz IPA'si, TestFlight paketi,
Developer ID DMG imzasi veya notarizasyon yapilamaz.

1. Identifiers'da `tr.sonalis.mobile` ve `tr.sonalis.desktop` kayitli olmali.
2. Certificates'ta `Apple Distribution` ve `Developer ID Application` olusturulmali.
3. iOS icin `tr.sonalis.mobile` App Store provisioning profile indirilmelidir.
4. App Store Connect > Users and Access > Integrations altinda API key olusturulup
   `.p8`, Key ID ve Issuer ID alinmalidir. `.p8` tekrar indirilemeyebilir.
5. App Store Connect'te Sonalis uygulama kaydi olusturulup bundle ID baglanmalidir.

Workflow iOS IPA'sini uretir. TestFlight'a yukleme, hesap sahibinin App Store Connect
sozlesmeleri ve uygulama gizlilik bilgileri tamamlandiktan sonra yapilir.

## Surum calistirma

GitHub Actions > `Signed platform client release` > Run workflow ekraninda surum ve
artan build numarasini girin. Anahtarlar yalniz korumali environment job'u basladiginda
acilir. Artifact'ler workflow bitince indirilebilir. Stable olarak yayinlamadan once
fiziksel ses, mikrofon, hoparlor, NAT, guncelleme ve uzun sure testleri zorunludur.
