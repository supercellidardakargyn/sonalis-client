#include "native_device_signer.h"

#include <bcrypt.h>
#include <wincrypt.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ss {
namespace {

constexpr char kAlgorithm[] = "ecdsa-p256-sha256-cng-v1";

std::string Base64(const std::span<const std::uint8_t> value) {
    DWORD bytes = 0;
    if (!CryptBinaryToStringA(value.data(), static_cast<DWORD>(value.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &bytes)) return {};
    std::string result(bytes, '\0');
    if (!CryptBinaryToStringA(value.data(), static_cast<DWORD>(value.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &bytes)) return {};
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

std::wstring KeyName(const std::string& installationId) {
    std::wstring result = L"Sonalis.NativeLogin.";
    result.reserve(result.size() + installationId.size());
    for (const unsigned char character : installationId) {
        if ((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || character == '-')
            result.push_back(static_cast<wchar_t>(character));
        else if (character >= 'A' && character <= 'F')
            result.push_back(static_cast<wchar_t>(std::tolower(character)));
        else
            return {};
    }
    return result;
}

}  // namespace

NativeDeviceSigner::~NativeDeviceSigner() { Reset(); }

void NativeDeviceSigner::Reset() noexcept {
    std::scoped_lock lock(mutex_);
    if (key_ != 0) NCryptFreeObject(key_);
    if (provider_ != 0) NCryptFreeObject(provider_);
    key_ = 0;
    provider_ = 0;
    publicKey_.clear();
}

bool NativeDeviceSigner::Initialize(const std::string& installationId, std::string& error) {
    std::scoped_lock lock(mutex_);
    if (key_ != 0 && !publicKey_.empty()) return true;
    if (installationId.size() != 36) { error = "Native kurulum kimligi gecersiz"; return false; }

    NCRYPT_PROV_HANDLE provider = 0;
    if (NCryptOpenStorageProvider(&provider, MS_PLATFORM_CRYPTO_PROVIDER, 0) != ERROR_SUCCESS) {
        error = "TPM guvenlik saglayicisi kullanilamiyor";
        return false;
    }
    const std::wstring keyName = KeyName(installationId);
    if (keyName.size() != std::wstring_view(L"Sonalis.NativeLogin.").size() + installationId.size()) {
        NCryptFreeObject(provider);
        error = "Native kurulum kimligi gecersiz";
        return false;
    }
    NCRYPT_KEY_HANDLE key = 0;
    SECURITY_STATUS status = NCryptOpenKey(provider, &key, keyName.c_str(), 0, NCRYPT_SILENT_FLAG);
    if (status == NTE_BAD_KEYSET || status == NTE_NOT_FOUND) {
        status = NCryptCreatePersistedKey(provider, &key, NCRYPT_ECDSA_P256_ALGORITHM,
                                          keyName.c_str(), 0, NCRYPT_SILENT_FLAG);
        if (status == ERROR_SUCCESS) {
            DWORD usage = NCRYPT_ALLOW_SIGNING_FLAG;
            status = NCryptSetProperty(key, NCRYPT_KEY_USAGE_PROPERTY,
                                       reinterpret_cast<PBYTE>(&usage), sizeof(usage), 0);
        }
        if (status == ERROR_SUCCESS) status = NCryptFinalizeKey(key, NCRYPT_SILENT_FLAG);
        if (status != ERROR_SUCCESS && key != 0) {
            NCryptDeleteKey(key, NCRYPT_SILENT_FLAG);
            key = 0;
        }
    }
    if (status != ERROR_SUCCESS || key == 0) {
        if (key != 0) NCryptFreeObject(key);
        NCryptFreeObject(provider);
        error = "TPM cihaz anahtari acilamadi";
        return false;
    }

    DWORD blobBytes = 0;
    status = NCryptExportKey(key, 0, BCRYPT_ECCPUBLIC_BLOB, nullptr, nullptr, 0, &blobBytes, 0);
    std::vector<std::uint8_t> blob(blobBytes);
    if (status == ERROR_SUCCESS) {
        status = NCryptExportKey(key, 0, BCRYPT_ECCPUBLIC_BLOB, nullptr, blob.data(), blobBytes, &blobBytes, 0);
    }
    if (status != ERROR_SUCCESS || blobBytes != sizeof(BCRYPT_ECCKEY_BLOB) + 64) {
        NCryptFreeObject(key); NCryptFreeObject(provider);
        error = "TPM public anahtari alinamadi";
        return false;
    }
    const auto* header = reinterpret_cast<const BCRYPT_ECCKEY_BLOB*>(blob.data());
    if (header->dwMagic != BCRYPT_ECDSA_PUBLIC_P256_MAGIC || header->cbKey != 32) {
        NCryptFreeObject(key); NCryptFreeObject(provider);
        error = "TPM public anahtar bicimi gecersiz";
        return false;
    }
    const std::string encoded = Base64(std::span<const std::uint8_t>(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), 64));
    if (encoded.empty()) {
        NCryptFreeObject(key); NCryptFreeObject(provider);
        error = "TPM public anahtari kodlanamadi";
        return false;
    }
    provider_ = provider;
    key_ = key;
    publicKey_ = encoded;
    return true;
}

bool NativeDeviceSigner::IsReady() const noexcept {
    std::scoped_lock lock(mutex_);
    return key_ != 0 && !publicKey_.empty();
}

std::string NativeDeviceSigner::Algorithm() const { return kAlgorithm; }

std::string NativeDeviceSigner::PublicKey() const {
    std::scoped_lock lock(mutex_);
    return publicKey_;
}

std::string NativeDeviceSigner::Sign(const std::string& canonical, std::string& error) const {
    std::scoped_lock lock(mutex_);
    if (key_ == 0) { error = "TPM cihaz anahtari hazir degil"; return {}; }
    BCRYPT_ALG_HANDLE sha256 = nullptr;
    if (BCryptOpenAlgorithmProvider(&sha256, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        error = "SHA-256 saglayicisi acilamadi"; return {};
    }
    std::array<std::uint8_t, 32> digest{};
    const NTSTATUS hashStatus = BCryptHash(sha256, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(canonical.data())), static_cast<ULONG>(canonical.size()),
        digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(sha256, 0);
    if (hashStatus < 0) { error = "Native giris ozeti uretilemedi"; return {}; }
    DWORD signatureBytes = 0;
    SECURITY_STATUS status = NCryptSignHash(key_, nullptr, digest.data(), static_cast<DWORD>(digest.size()),
                                            nullptr, 0, &signatureBytes, NCRYPT_SILENT_FLAG);
    std::vector<std::uint8_t> signature(signatureBytes);
    if (status == ERROR_SUCCESS) {
        status = NCryptSignHash(key_, nullptr, digest.data(), static_cast<DWORD>(digest.size()),
                                signature.data(), signatureBytes, &signatureBytes, NCRYPT_SILENT_FLAG);
    }
    if (status != ERROR_SUCCESS || signatureBytes != 64) { error = "TPM native giris imzasi uretilemedi"; return {}; }
    signature.resize(signatureBytes);
    return Base64(signature);
}

}  // namespace ss
