#include "message_crypto.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iterator>
#include <stdexcept>

#include <monocypher.h>
#include <monocypher-ed25519.h>
#include <nlohmann/json.hpp>

#include "win_helpers.h"

namespace ss {
namespace {

constexpr char kEntropy[] = "Sonalis-v3-message-vault";

bool RandomBytes(std::span<std::uint8_t> output) {
    return BCryptGenRandom(nullptr, output.data(), static_cast<ULONG>(output.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

std::string Base64(std::span<const std::uint8_t> value) {
    DWORD size = 0;
    if (!CryptBinaryToStringA(value.data(), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &size)) return {};
    std::string result(size, '\0');
    if (!CryptBinaryToStringA(value.data(), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &size)) return {};
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

bool FromBase64(const std::string& value, std::vector<std::uint8_t>& output) {
    DWORD size = 0;
    if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr)) return false;
    output.resize(size);
    return CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64, output.data(), &size, nullptr, nullptr) != FALSE;
}

std::wstring LocalVaultPath() {
    const std::filesystem::path result = LocalAppDataPath();
    if (result.empty()) return L"message-vault.bin";
    return (result / L"Sonalis" / L"message-vault.bin").wstring();
}

std::string Uuid() {
    std::array<std::uint8_t, 16> bytes{};
    if (!RandomBytes(bytes)) return {};
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);
    constexpr char hex[] = "0123456789abcdef";
    std::string result; result.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) result.push_back('-');
        result.push_back(hex[bytes[index] >> 4U]); result.push_back(hex[bytes[index] & 15U]);
    }
    return result;
}

std::array<std::uint8_t, 32> EnvelopeKey(std::span<const std::uint8_t, 32> shared, const std::string& context) {
    std::array<std::uint8_t, 32> key{};
    crypto_blake2b_keyed(key.data(), key.size(), shared.data(), shared.size(),
                         reinterpret_cast<const std::uint8_t*>(context.data()), context.size());
    return key;
}

std::string Sha256(std::span<const std::uint8_t> bytes) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD objectBytes = 0;
    DWORD resultBytes = 0;
    std::vector<std::uint8_t> object;
    std::array<std::uint8_t, 32> digest{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    const auto close = [&] {
        if (hash != nullptr) BCryptDestroyHash(hash);
        if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    };
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
                          &resultBytes, 0) < 0) {
        close();
        return {};
    }
    object.resize(objectBytes);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) < 0
        || (!bytes.empty() && BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()),
                                             static_cast<ULONG>(bytes.size()), 0) < 0)
        || BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        close();
        return {};
    }
    close();
    constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(digest.size() * 2U);
    for (const std::uint8_t value : digest) {
        output.push_back(hex[value >> 4U]);
        output.push_back(hex[value & 15U]);
    }
    return output;
}

}  // namespace

MessageCrypto::~MessageCrypto() {
    crypto_wipe(encryptionSecret_.data(), encryptionSecret_.size());
    crypto_wipe(signingSecret_.data(), signingSecret_.size());
}

std::wstring MessageCrypto::VaultPath() const { return LocalVaultPath(); }
const std::string& MessageCrypto::DeviceId() const noexcept { return deviceId_; }
bool MessageCrypto::IsReady() const noexcept { return ready_; }
std::string MessageCrypto::NewId() { return Uuid(); }
std::string MessageCrypto::EncryptionPublicKey() const { return Base64(encryptionPublic_); }
std::string MessageCrypto::SigningPublicKey() const { return Base64(signingPublic_); }
std::string MessageCrypto::AccountVaultEnvelope() const { return Base64(encryptionPublic_); }

bool MessageCrypto::Save(std::string& error) const {
    try {
        const nlohmann::json json{{"deviceId", deviceId_}, {"encryptionSecret", Base64(encryptionSecret_)},
            {"encryptionPublic", Base64(encryptionPublic_)}, {"signingSecret", Base64(signingSecret_)}, {"signingPublic", Base64(signingPublic_)}};
        const std::string plaintext = json.dump();
        DATA_BLOB input{static_cast<DWORD>(plaintext.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()))};
        DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropy) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
        DATA_BLOB protectedData{};
        if (!CryptProtectData(&input, L"Sonalis mesaj anahtarlari", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &protectedData)) {
            error = "Mesaj anahtar kasasi DPAPI ile korunamadi"; return false;
        }
        const std::filesystem::path target(VaultPath()); std::filesystem::create_directories(target.parent_path());
        const std::filesystem::path temporary = target.wstring() + L".tmp";
        { std::ofstream stream(temporary, std::ios::binary | std::ios::trunc); stream.write(reinterpret_cast<const char*>(protectedData.pbData), protectedData.cbData); stream.flush(); }
        LocalFree(protectedData.pbData);
        if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::filesystem::remove(temporary); error = "Mesaj anahtar kasasi kaydedilemedi"; return false;
        }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool MessageCrypto::Initialize(std::string& error) {
    try {
        std::ifstream stream(std::filesystem::path(VaultPath()), std::ios::binary);
        if (stream) {
            const std::vector<std::uint8_t> encrypted((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            DATA_BLOB input{static_cast<DWORD>(encrypted.size()), const_cast<BYTE*>(encrypted.data())};
            DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropy) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
            DATA_BLOB plaintext{};
            if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plaintext)) throw std::runtime_error("Mesaj anahtar kasasi acilamadi");
            const auto json = nlohmann::json::parse(reinterpret_cast<const char*>(plaintext.pbData), reinterpret_cast<const char*>(plaintext.pbData) + plaintext.cbData);
            LocalFree(plaintext.pbData); deviceId_ = json.at("deviceId").get<std::string>();
            std::vector<std::uint8_t> value;
            const auto load = [&](const char* name, auto& destination) {
                value.clear(); if (!FromBase64(json.at(name).get<std::string>(), value) || value.size() != destination.size()) throw std::runtime_error("Mesaj anahtar kasasi gecersiz");
                std::copy(value.begin(), value.end(), destination.begin());
            };
            load("encryptionSecret", encryptionSecret_); load("encryptionPublic", encryptionPublic_);
            load("signingSecret", signingSecret_); load("signingPublic", signingPublic_); ready_ = true; return true;
        }
        deviceId_ = Uuid(); std::array<std::uint8_t, 32> signingSeed{};
        if (deviceId_.empty() || !RandomBytes(encryptionSecret_) || !RandomBytes(signingSeed)) throw std::runtime_error("Guvenli rastgele anahtar uretilemedi");
        crypto_x25519_public_key(encryptionPublic_.data(), encryptionSecret_.data());
        crypto_ed25519_key_pair(signingSecret_.data(), signingPublic_.data(), signingSeed.data());
        crypto_wipe(signingSeed.data(), signingSeed.size()); ready_ = true; return Save(error);
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool MessageCrypto::RandomConversationKey(std::array<std::uint8_t, 32>& key, std::string& error) const {
    if (!ready_ || !RandomBytes(key)) { error = "Konusma anahtari uretilemedi"; return false; }
    return true;
}

bool MessageCrypto::SealConversationKey(std::span<const std::uint8_t, 32> key, const std::string& recipientPublicKey,
                                        const std::string& context, std::string& envelope, std::string& error) const {
    std::vector<std::uint8_t> recipient;
    if (!FromBase64(recipientPublicKey, recipient) || recipient.size() != 32) { error = "Alici mesaj anahtari gecersiz"; return false; }
    std::array<std::uint8_t, 32> ephemeralSecret{}, ephemeralPublic{}, shared{}; std::array<std::uint8_t, 24> nonce{};
    if (!RandomBytes(ephemeralSecret) || !RandomBytes(nonce)) { error = "Anahtar zarfi rastgeleligi uretilemedi"; return false; }
    crypto_x25519_public_key(ephemeralPublic.data(), ephemeralSecret.data());
    crypto_x25519(shared.data(), ephemeralSecret.data(), recipient.data()); const auto sealKey = EnvelopeKey(shared, context);
    std::array<std::uint8_t, 16> mac{}; std::array<std::uint8_t, 32> cipher{};
    crypto_aead_lock(cipher.data(), mac.data(), sealKey.data(), nonce.data(), reinterpret_cast<const std::uint8_t*>(context.data()), context.size(), key.data(), key.size());
    std::vector<std::uint8_t> packed; packed.reserve(105); packed.push_back(1); packed.insert(packed.end(), ephemeralPublic.begin(), ephemeralPublic.end());
    packed.insert(packed.end(), nonce.begin(), nonce.end()); packed.insert(packed.end(), mac.begin(), mac.end()); packed.insert(packed.end(), cipher.begin(), cipher.end());
    envelope = Base64(packed); crypto_wipe(ephemeralSecret.data(), ephemeralSecret.size()); crypto_wipe(shared.data(), shared.size()); return !envelope.empty();
}

bool MessageCrypto::OpenConversationKey(const std::string& envelope, const std::string& context,
                                        std::array<std::uint8_t, 32>& key, std::string& error) const {
    std::vector<std::uint8_t> packed; if (!FromBase64(envelope, packed) || packed.size() != 105 || packed[0] != 1) { error = "Mesaj anahtar zarfi gecersiz"; return false; }
    std::array<std::uint8_t, 32> shared{}; crypto_x25519(shared.data(), encryptionSecret_.data(), packed.data() + 1); const auto sealKey = EnvelopeKey(shared, context);
    if (crypto_aead_unlock(key.data(), packed.data() + 57, sealKey.data(), packed.data() + 33,
                           reinterpret_cast<const std::uint8_t*>(context.data()), context.size(), packed.data() + 73, key.size()) != 0) {
        error = "Mesaj anahtar zarfi dogrulanamadi"; crypto_wipe(shared.data(), shared.size()); return false;
    }
    crypto_wipe(shared.data(), shared.size()); return true;
}

bool MessageCrypto::EncryptMessage(std::span<const std::uint8_t, 32> key, const std::string& associatedData,
                                   const std::string& plaintext, EncryptedMessagePayload& output, std::string& error) const {
    std::array<std::uint8_t, 24> nonce{}; if (!RandomBytes(nonce)) { error = "Mesaj nonce uretilemedi"; return false; }
    std::vector<std::uint8_t> cipher(16 + plaintext.size());
    crypto_aead_lock(cipher.data() + 16, cipher.data(), key.data(), nonce.data(), reinterpret_cast<const std::uint8_t*>(associatedData.data()), associatedData.size(),
                     reinterpret_cast<const std::uint8_t*>(plaintext.data()), plaintext.size());
    output.ciphertext = Base64(cipher); output.nonce = Base64(nonce); return !output.ciphertext.empty() && !output.nonce.empty();
}

bool MessageCrypto::DecryptMessage(std::span<const std::uint8_t, 32> key, const std::string& associatedData,
                                   const EncryptedMessagePayload& payload, std::string& plaintext) const {
    std::vector<std::uint8_t> cipher, nonce;
    if (!FromBase64(payload.ciphertext, cipher) || cipher.size() < 16 || !FromBase64(payload.nonce, nonce) || nonce.size() != 24) return false;
    std::vector<std::uint8_t> clear(cipher.size() - 16);
    if (crypto_aead_unlock(clear.data(), cipher.data(), key.data(), nonce.data(), reinterpret_cast<const std::uint8_t*>(associatedData.data()), associatedData.size(),
                           cipher.data() + 16, clear.size()) != 0) return false;
    plaintext.assign(reinterpret_cast<const char*>(clear.data()), clear.size()); return true;
}

bool MessageCrypto::SealModerationEvidence(const std::span<const std::uint8_t> plaintext,
                                           const std::string& moderationPublicKey,
                                           std::vector<std::uint8_t>& ciphertext,
                                           std::string& error) const {
    constexpr std::string_view context = "SonalisGuardianEvidenceV1";
    std::vector<std::uint8_t> recipient;
    if (!ready_) {
        error = "Cihaz imza anahtarı hazır değil";
        return false;
    }
    if (!FromBase64(moderationPublicKey, recipient) || recipient.size() != 32U) {
        error = "Guardian moderasyon anahtarı geçersiz";
        return false;
    }
    if (plaintext.empty() || plaintext.size() > 16U * 1024U * 1024U) {
        error = "Guardian delil boyutu geçersiz";
        return false;
    }
    std::array<std::uint8_t, 32> ephemeralSecret{};
    std::array<std::uint8_t, 32> ephemeralPublic{};
    std::array<std::uint8_t, 32> shared{};
    std::array<std::uint8_t, 24> nonce{};
    std::array<std::uint8_t, 16> mac{};
    if (!RandomBytes(ephemeralSecret) || !RandomBytes(nonce)) {
        error = "Guardian delil anahtarı üretilemedi";
        return false;
    }
    crypto_x25519_public_key(ephemeralPublic.data(), ephemeralSecret.data());
    crypto_x25519(shared.data(), ephemeralSecret.data(), recipient.data());
    const auto sealKey = EnvelopeKey(shared, std::string(context));
    ciphertext.resize(1U + ephemeralPublic.size() + nonce.size() + mac.size() + plaintext.size());
    ciphertext[0] = 1U;
    std::copy(ephemeralPublic.begin(), ephemeralPublic.end(), ciphertext.begin() + 1);
    std::copy(nonce.begin(), nonce.end(), ciphertext.begin() + 33);
    crypto_aead_lock(ciphertext.data() + 73, mac.data(), sealKey.data(), nonce.data(),
                     reinterpret_cast<const std::uint8_t*>(context.data()), context.size(),
                     plaintext.data(), plaintext.size());
    std::copy(mac.begin(), mac.end(), ciphertext.begin() + 57);
    crypto_wipe(ephemeralSecret.data(), ephemeralSecret.size());
    crypto_wipe(shared.data(), shared.size());
    return true;
}

std::string MessageCrypto::Sha256Hex(const std::span<const std::uint8_t> bytes) {
    return Sha256(bytes);
}

std::string MessageCrypto::Sign(const std::string& canonical) const {
    if (!ready_) return {};
    std::array<std::uint8_t, 64> signature{};
    crypto_ed25519_sign(signature.data(), signingSecret_.data(), reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size());
    return Base64(signature);
}

bool MessageCrypto::Verify(const std::string& publicKey, const std::string& canonical, const std::string& signature) {
    std::vector<std::uint8_t> publicBytes, signatureBytes;
    if (!FromBase64(publicKey, publicBytes) || publicBytes.size() != 32 || !FromBase64(signature, signatureBytes) || signatureBytes.size() != 64) return false;
    return crypto_ed25519_check(signatureBytes.data(), publicBytes.data(), reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()) == 0;
}

}  // namespace ss
