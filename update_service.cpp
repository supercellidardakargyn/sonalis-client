#include "update_service.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <softpub.h>
#include <wintrust.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>

#include <monocypher-ed25519.h>
#include <nlohmann/json.hpp>

#include "http_client.h"
#include "device_identity.h"
#include "settings.h"
#include "win_helpers.h"

namespace ss {
namespace {

constexpr std::uint64_t kUpdateCheckIntervalMs = 10U * 60U * 1'000U;

std::uint64_t SteadyNowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool Base64(const std::string& text, std::vector<std::uint8_t>& output) {
    DWORD bytes = 0;
    if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                              nullptr, &bytes, nullptr, nullptr)) return false;
    output.resize(bytes);
    return CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                                output.data(), &bytes, nullptr, nullptr) != FALSE;
}

std::string Sha256File(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0;
    DWORD hashBytes = 0;
    DWORD written = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes),
                      sizeof(objectBytes), &written, 0);
    BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashBytes),
                      sizeof(hashBytes), &written, 0);
    std::vector<std::uint8_t> object(objectBytes);
    std::vector<std::uint8_t> digest(hashBytes);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    std::ifstream stream(path, std::ios::binary);
    std::array<char, 64U * 1024U> buffer{};
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = stream.gcount();
        if (count > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                        static_cast<ULONG>(count), 0) < 0) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return {};
        }
    }
    if (!stream.eof() || BCryptFinishHash(hash, digest.data(), hashBytes, 0) < 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2U);
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 15U]);
    }
    return result;
}

std::wstring UpdateDirectory() {
    const std::filesystem::path result = LocalAppDataPath();
    return result.empty() ? L"." : (result / L"Sonalis" / L"updates").wstring();
}

std::string JsonString(const std::string& value) {
    return nlohmann::json(value).dump();
}

std::string CanonicalManifest(const UpdateInfo& info) {
    return std::string{"{\"product\":"} + JsonString(info.product)
        + ",\"channel\":" + JsonString(info.channel)
        + ",\"version\":" + JsonString(info.version)
        + ",\"minimumVersion\":" + (info.minimumVersion.empty() ? "null" : JsonString(info.minimumVersion))
        + ",\"artifactUrl\":" + JsonString(info.artifactUrl)
        + ",\"artifactSize\":" + std::to_string(info.artifactSize)
        + ",\"sha256\":" + JsonString(info.sha256)
        + ",\"authenticodeThumbprint\":"
        + (info.authenticodeThumbprint.empty() ? "null" : JsonString(info.authenticodeThumbprint))
        + ",\"publishedAt\":" + JsonString(info.publishedAt)
        + ",\"rolloutPercent\":" + std::to_string(info.rolloutPercent)
        + ",\"required\":" + (info.required ? "true" : "false") + "}";
}

bool AuthenticodeSignerMatches(const std::filesystem::path& path, const std::string_view expected) noexcept {
    if (expected.size() != 64U) return false;
    HCERTSTORE store = nullptr;
    HCRYPTMSG message = nullptr;
    DWORD encoding = 0;
    DWORD content = 0;
    DWORD format = 0;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &content, &format,
                          &store, &message, nullptr)) return false;
    bool matched = false;
    DWORD signerBytes = 0;
    if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerBytes) && signerBytes > 0) {
        std::vector<std::uint8_t> storage(signerBytes);
        if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, storage.data(), &signerBytes)) {
            const auto* signer = reinterpret_cast<const CMSG_SIGNER_INFO*>(storage.data());
            CERT_INFO certificateInfo{};
            certificateInfo.Issuer = signer->Issuer;
            certificateInfo.SerialNumber = signer->SerialNumber;
            PCCERT_CONTEXT certificate = CertFindCertificateInStore(store,
                X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &certificateInfo, nullptr);
            if (certificate != nullptr) {
                std::array<std::uint8_t, 32> digest{};
                DWORD digestBytes = static_cast<DWORD>(digest.size());
                if (CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr,
                                          certificate->pbCertEncoded, certificate->cbCertEncoded,
                                          digest.data(), &digestBytes)
                    && digestBytes == static_cast<DWORD>(digest.size())) {
                    auto nibble = [](const char character) noexcept -> int {
                        if (character >= '0' && character <= '9') return character - '0';
                        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
                        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
                        return -1;
                    };
                    matched = true;
                    for (std::size_t index = 0; matched && index < digest.size(); ++index) {
                        const int high = nibble(expected[index * 2U]);
                        const int low = nibble(expected[index * 2U + 1U]);
                        matched = high >= 0 && low >= 0
                            && digest[index] == static_cast<std::uint8_t>((high << 4) | low);
                    }
                }
                CertFreeCertificateContext(certificate);
            }
        }
    }
    if (message != nullptr) CryptMsgClose(message);
    if (store != nullptr) CertCloseStore(store, 0);
    return matched;
}

bool VerifyAuthenticode(const std::filesystem::path& path, const std::string& expectedThumbprint) noexcept {
    if (expectedThumbprint.empty()) return std::string_view(SONALIS_UPDATE_CHANNEL) == "canary";
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = WinVerifyTrust(nullptr, &policy, &trust);
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &trust);
    return result == ERROR_SUCCESS && AuthenticodeSignerMatches(path, expectedThumbprint);
}

bool VerifyDownloadedPackage(const std::filesystem::path& path, const UpdateInfo& info,
                             std::string& error) {
    if (Sha256File(path) != info.sha256) {
        error = "Paket SHA-256 dogrulamasi basarisiz";
        return false;
    }
    std::error_code sizeError;
    if (std::filesystem::file_size(path, sizeError) != info.artifactSize || sizeError) {
        error = "Paket boyutu dogrulamasi basarisiz";
        return false;
    }

    std::vector<std::uint8_t> publicKey;
    std::vector<std::uint8_t> signature;
    if (!Base64(SONALIS_RELEASE_PUBLIC_KEY_BASE64, publicKey) || publicKey.size() != 32
        || !Base64(info.signature, signature) || signature.size() != 64) {
        error = "Ed25519 guncelleme anahtari yapilandirilmamis";
        return false;
    }
    const std::string signedMessage = CanonicalManifest(info);
    if (crypto_ed25519_check(signature.data(), publicKey.data(),
                             reinterpret_cast<const std::uint8_t*>(signedMessage.data()),
                             signedMessage.size()) != 0) {
        error = "Paket imzasi gecersiz";
        return false;
    }
    if (!VerifyAuthenticode(path, info.authenticodeThumbprint)) {
        error = "Paket Authenticode dogrulamasi basarisiz";
        return false;
    }
    return true;
}

}  // namespace

UpdateService::~UpdateService() {
    if (thread_.joinable()) thread_.request_stop();
}

void UpdateService::Set(const UpdateState state, std::string status) {
    std::function<void()> callback;
    {
        std::scoped_lock lock(mutex_);
        state_ = state;
        status_ = std::move(status);
        if (state == UpdateState::Current || state == UpdateState::Error) {
            nextCheckAtMs_ = SteadyNowMs() + kUpdateCheckIntervalMs;
        }
        callback = stateCallback_;
    }
    if (callback) callback();
}

void UpdateService::SetStateCallback(std::function<void()> callback) {
    std::scoped_lock lock(mutex_);
    stateCallback_ = std::move(callback);
}

UpdateState UpdateService::State() const noexcept {
    std::scoped_lock lock(mutex_);
    return state_;
}

std::string UpdateService::Status() const {
    std::scoped_lock lock(mutex_);
    return status_;
}

std::optional<UpdateInfo> UpdateService::Available() const {
    std::scoped_lock lock(mutex_);
    return available_;
}

bool UpdateService::CheckAsync(std::string controlOrigin, const bool force) {
    std::function<void()> callback;
    {
        std::scoped_lock lock(mutex_);
        const std::uint64_t now = SteadyNowMs();
        if (state_ == UpdateState::Checking || state_ == UpdateState::Downloading
            || state_ == UpdateState::Available || state_ == UpdateState::Ready) return false;
        if (!force && nextCheckAtMs_ != 0 && now < nextCheckAtMs_) return false;
        state_ = UpdateState::Checking;
        status_ = "Guncellemeler denetleniyor";
        nextCheckAtMs_ = 0;
        callback = stateCallback_;
    }
    if (callback) callback();
    thread_ = std::jthread([this, origin = std::move(controlOrigin)] { Check(std::move(origin)); });
    return true;
}

bool UpdateService::CheckDue() const noexcept {
    std::scoped_lock lock(mutex_);
    if (state_ == UpdateState::Checking || state_ == UpdateState::Downloading
        || state_ == UpdateState::Available || state_ == UpdateState::Ready) return false;
    return nextCheckAtMs_ == 0 || SteadyNowMs() >= nextCheckAtMs_;
}

bool UpdateService::DownloadAsync() {
    std::function<void()> callback;
    {
        std::scoped_lock lock(mutex_);
        if (state_ != UpdateState::Available) return false;
        state_ = UpdateState::Downloading;
        status_ = "Guncelleme indiriliyor";
        callback = stateCallback_;
    }
    if (callback) callback();
    thread_ = std::jthread([this] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        std::string ignored;
        (void)Download(ignored);
    });
    return true;
}

std::uint32_t UpdateService::MillisecondsUntilNextCheck() const noexcept {
    std::scoped_lock lock(mutex_);
    if (state_ == UpdateState::Checking || state_ == UpdateState::Downloading
        || state_ == UpdateState::Available || state_ == UpdateState::Ready) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    const std::uint64_t now = SteadyNowMs();
    if (nextCheckAtMs_ == 0 || now >= nextCheckAtMs_) return 0;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(nextCheckAtMs_ - now,
                                                              std::numeric_limits<std::uint32_t>::max()));
}

void UpdateService::Check(std::string origin) {
    try {
        while (!origin.empty() && origin.back() == '/') origin.pop_back();
        const std::string cohort = StableDeviceBindingId();
        if (cohort.empty()) throw std::runtime_error("Guncelleme kohortu olusturulamadi");
        const auto response = HttpClient{}.Request(
            L"GET", origin + "/api/v1/releases/latest?product=client&manifestVersion=2&channel=" SONALIS_UPDATE_CHANNEL
                "&cohort=" + cohort);
        if (response.status == 404) {
            Set(UpdateState::Current, "Uygulama guncel");
            return;
        }
        if (response.status != 200) {
            throw std::runtime_error("Guncelleme servisi HTTP " + std::to_string(response.status));
        }
        const auto json = nlohmann::json::parse(response.body);
        UpdateInfo info{};
        info.product = json.value("product", "");
        info.channel = json.value("channel", "");
        info.version = json.value("version", "");
        info.minimumVersion = json.contains("minimumVersion") && json["minimumVersion"].is_string()
            ? json["minimumVersion"].get<std::string>() : std::string{};
        info.artifactUrl = json.value("artifactUrl", "");
        info.artifactSize = json.value("artifactSize", std::uint64_t{});
        info.sha256 = json.value("sha256", "");
        info.signature = json.value("signature", "");
        info.authenticodeThumbprint = json.contains("authenticodeThumbprint") && json["authenticodeThumbprint"].is_string()
            ? json["authenticodeThumbprint"].get<std::string>() : std::string{};
        info.publishedAt = json.value("publishedAt", "");
        info.rolloutPercent = json.value("rolloutPercent", 0);
        info.required = json.value("required", false);
        info.manifestVersion = json.value("manifestVersion", 0);
        if (info.product != "client" || info.channel != SONALIS_UPDATE_CHANNEL || info.manifestVersion != 2
            || info.version.empty() || !info.artifactUrl.starts_with("https://") || info.artifactSize == 0
            || info.sha256.size() != 64 || info.signature.empty() || info.publishedAt.empty()
            || info.rolloutPercent < 1 || info.rolloutPercent > 100
            || (std::string_view(SONALIS_UPDATE_CHANNEL) == "stable" && info.authenticodeThumbprint.size() != 64U)) {
            throw std::runtime_error("Guncelleme manifesti gecersiz");
        }
        if (!ParseReleaseVersion(info.version)) {
            throw std::runtime_error("Guncelleme surumu gecersiz");
        }
        if (!IsNewerReleaseVersion(info.version, SONALIS_VERSION)) {
            Set(UpdateState::Current, "Uygulama guncel");
            return;
        }
        std::function<void()> callback;
        {
            std::scoped_lock lock(mutex_);
            available_ = std::move(info);
            state_ = UpdateState::Available;
            status_ = available_->version + " kullanilabilir";
            callback = stateCallback_;
        }
        if (callback) callback();
    } catch (const std::exception& exception) {
        Set(UpdateState::Error, exception.what());
    }
}

bool UpdateService::Download(std::string& error) {
    const auto info = Available();
    if (!info) {
        error = "Indirilecek guncelleme yok";
        return false;
    }
    std::filesystem::path temporary;
    try {
        Set(UpdateState::Downloading, "Guncelleme indiriliyor");
        const std::filesystem::path directory(UpdateDirectory());
        std::filesystem::create_directories(directory);
        temporary = directory / (Utf8ToWide(info->version) + L".tmp");
        const auto target = directory / (L"Sonalis-" + Utf8ToWide(info->version) + L"-x64.exe");
        const auto status = HttpClient{}.DownloadToFile(info->artifactUrl, temporary.wstring());
        if (status != 200) {
            throw std::runtime_error("Paket indirilemedi (HTTP " + std::to_string(status) + ")");
        }
        std::string verificationError;
        if (!VerifyDownloadedPackage(temporary, *info, verificationError)) {
            throw std::runtime_error(verificationError);
        }
        if (!MoveFileExW(temporary.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Guncelleme paketi tamamlanamadi");
        }
        std::function<void()> callback;
        {
            std::scoped_lock lock(mutex_);
            downloadedPath_ = target.wstring();
            state_ = UpdateState::Ready;
            status_ = "Kurulum icin hazir";
            callback = stateCallback_;
        }
        if (callback) callback();
        return true;
    } catch (const std::exception& exception) {
        if (!temporary.empty()) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
        error = exception.what();
        Set(UpdateState::Error, error);
        return false;
    }
}

bool UpdateService::LaunchInstaller(std::string& error) const {
    std::wstring path;
    std::optional<UpdateInfo> info;
    {
        std::scoped_lock lock(mutex_);
        if (state_ != UpdateState::Ready) {
            error = "Guncelleme kurulum icin hazir degil";
            return false;
        }
        path = downloadedPath_;
        info = available_;
    }
    if (path.empty() || !info) {
        error = "Hazir guncelleme paketi yok";
        return false;
    }
    if (!VerifyDownloadedPackage(std::filesystem::path(path), *info, error)) return false;
    if (!DynamicShellExecute(path, kAutomaticUpdateInstallerArguments)) {
        error = "Kurulum baslatilamadi";
        return false;
    }
    return true;
}

}  // namespace ss
