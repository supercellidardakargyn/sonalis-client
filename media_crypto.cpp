#include "media_crypto.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include <monocypher.h>
#include <nlohmann/json.hpp>

#include "settings.h"

namespace ss {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{'S', 'N', 'L', 'M', 'E', '0', '1', 0};
constexpr std::uint32_t kChunkBytes = 256U * 1024U;
constexpr std::size_t kHeaderBytes = 8U + 8U + 4U + 16U;

bool RandomBytes(const std::span<std::uint8_t> bytes) {
    return BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

void PutU32(std::span<std::uint8_t> bytes, const std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

void PutU64(std::span<std::uint8_t> bytes, const std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

std::uint32_t GetU32(const std::span<const std::uint8_t> bytes) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8U);
    return value;
}

std::uint64_t GetU64(const std::span<const std::uint8_t> bytes) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    return value;
}

std::array<std::uint8_t, 24> ChunkNonce(const std::span<const std::uint8_t, 16> prefix,
                                        const std::uint64_t chunkIndex) {
    std::array<std::uint8_t, 24> nonce{};
    std::copy(prefix.begin(), prefix.end(), nonce.begin());
    PutU64(std::span(nonce).subspan(16, 8), chunkIndex);
    return nonce;
}

std::string AssociatedData(const std::string& attachmentId, const std::uint64_t chunkIndex) {
    return "SonalisMediaChunkV1\n" + attachmentId + "\n" + std::to_string(chunkIndex);
}

class Sha256Stream final {
public:
    Sha256Stream() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
            throw std::runtime_error("SHA-256 saglayicisi acilamadi");
        }
        DWORD objectBytes = 0;
        DWORD copied = 0;
        if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes),
                              sizeof(objectBytes), &copied, 0) != 0) {
            throw std::runtime_error("SHA-256 nesne boyutu alinamadi");
        }
        object_.resize(objectBytes);
        if (BCryptCreateHash(algorithm_, &hash_, object_.data(), static_cast<ULONG>(object_.size()),
                             nullptr, 0, 0) != 0) {
            throw std::runtime_error("SHA-256 olusturulamadi");
        }
    }
    ~Sha256Stream() {
        if (hash_ != nullptr) BCryptDestroyHash(hash_);
        if (algorithm_ != nullptr) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    Sha256Stream(const Sha256Stream&) = delete;
    Sha256Stream& operator=(const Sha256Stream&) = delete;
    void Update(const std::span<const std::uint8_t> bytes) {
        if (!bytes.empty() && BCryptHashData(hash_, const_cast<PUCHAR>(bytes.data()),
                                             static_cast<ULONG>(bytes.size()), 0) != 0) {
            throw std::runtime_error("SHA-256 guncellenemedi");
        }
    }
    std::string Finish() {
        std::array<std::uint8_t, 32> digest{};
        if (BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) != 0) {
            throw std::runtime_error("SHA-256 tamamlanamadi");
        }
        constexpr char alphabet[] = "0123456789abcdef";
        std::string result(digest.size() * 2U, '0');
        for (std::size_t index = 0; index < digest.size(); ++index) {
            result[index * 2U] = alphabet[digest[index] >> 4U];
            result[index * 2U + 1U] = alphabet[digest[index] & 0x0fU];
        }
        return result;
    }
private:
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_HASH_HANDLE hash_{};
    std::vector<std::uint8_t> object_;
};

std::string HashFile(const std::wstring& path) {
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    if (!stream) throw std::runtime_error("Sifreli medya hash icin acilamadi");
    Sha256Stream hash;
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    while (stream) {
        stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto read = stream.gcount();
        if (read > 0) hash.Update(std::span(buffer).first(static_cast<std::size_t>(read)));
    }
    return hash.Finish();
}

}  // namespace

std::array<std::uint8_t, 32> MediaCrypto::FileKey(
    const std::span<const std::uint8_t, 32> conversationKey,
    const std::string& attachmentId) {
    std::array<std::uint8_t, 32> output{};
    const std::string context = "SonalisMediaFileKeyV1\n" + attachmentId;
    crypto_blake2b_keyed(output.data(), output.size(),
                         conversationKey.data(), conversationKey.size(),
                         reinterpret_cast<const std::uint8_t*>(context.data()), context.size());
    return output;
}

std::string MediaCrypto::MimeHint(const std::wstring& path) {
    std::wstring extension = std::filesystem::path(path).extension().wstring();
    std::ranges::transform(extension, extension.begin(), [](const wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    if (extension == L".png") return "image/png";
    if (extension == L".jpg" || extension == L".jpeg") return "image/jpeg";
    if (extension == L".webp") return "image/webp";
    if (extension == L".gif") return "image/gif";
    if (extension == L".pdf") return "application/pdf";
    if (extension == L".txt") return "text/plain";
    if (extension == L".zip") return "application/zip";
    return "application/octet-stream";
}

bool MediaCrypto::PrepareFile(const MessageCrypto& crypto,
                              const std::span<const std::uint8_t, 32> conversationKey,
                              const std::string& attachmentId,
                              const std::wstring& sourcePath,
                              const std::wstring& encryptedPath,
                              PreparedMediaFile& output,
                              std::string& error) {
    std::error_code ignored;
    try {
        const std::filesystem::path source(sourcePath);
        const std::uint64_t originalSize = std::filesystem::file_size(source);
        if (originalSize == 0 || originalSize > 100U * 1024U * 1024U) {
            error = "Medya dosya boyutu gecersiz";
            return false;
        }
        std::ifstream input(source, std::ios::binary);
        if (!input) throw std::runtime_error("Medya dosyasi acilamadi");
        const std::filesystem::path target(encryptedPath);
        std::filesystem::create_directories(target.parent_path());
        std::ofstream encrypted(target, std::ios::binary | std::ios::trunc);
        if (!encrypted) throw std::runtime_error("Sifreli medya dosyasi olusturulamadi");

        std::array<std::uint8_t, kHeaderBytes> header{};
        std::copy(kMagic.begin(), kMagic.end(), header.begin());
        PutU64(std::span(header).subspan(8, 8), originalSize);
        PutU32(std::span(header).subspan(16, 4), kChunkBytes);
        if (!RandomBytes(std::span(header).subspan(20, 16))) {
            throw std::runtime_error("Medya nonce uretilemedi");
        }
        encrypted.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));

        auto fileKey = FileKey(conversationKey, attachmentId);
        std::vector<std::uint8_t> clear(kChunkBytes);
        std::vector<std::uint8_t> cipher(kChunkBytes);
        std::array<std::uint8_t, 16> mac{};
        std::uint64_t chunkIndex = 0;
        while (input) {
            input.read(reinterpret_cast<char*>(clear.data()), static_cast<std::streamsize>(clear.size()));
            const auto read = input.gcount();
            if (read <= 0) break;
            const std::size_t bytes = static_cast<std::size_t>(read);
            const auto nonce = ChunkNonce(std::span<const std::uint8_t, 16>(header.data() + 20, 16), chunkIndex);
            const std::string associated = AssociatedData(attachmentId, chunkIndex);
            crypto_aead_lock(cipher.data(), mac.data(), fileKey.data(), nonce.data(),
                             reinterpret_cast<const std::uint8_t*>(associated.data()), associated.size(),
                             clear.data(), bytes);
            encrypted.write(reinterpret_cast<const char*>(mac.data()), static_cast<std::streamsize>(mac.size()));
            encrypted.write(reinterpret_cast<const char*>(cipher.data()), static_cast<std::streamsize>(bytes));
            if (!encrypted) throw std::runtime_error("Sifreli medya diske yazilamadi");
            ++chunkIndex;
        }
        encrypted.flush();
        if (!encrypted) throw std::runtime_error("Sifreli medya tamamlanamadi");
        encrypted.close();

        const std::string mime = MimeHint(sourcePath);
        const std::string metadata = nlohmann::json{
            {"version", 1},
            {"name", WideToUtf8(source.filename().wstring())},
            {"originalSize", originalSize},
            {"mime", mime},
            {"chunkBytes", kChunkBytes},
        }.dump();
        EncryptedMessagePayload encryptedMetadata;
        const std::string metadataAssociated = "SonalisMediaMetadataV1\n" + attachmentId;
        if (!crypto.EncryptMessage(fileKey, metadataAssociated, metadata, encryptedMetadata, error)) {
            std::filesystem::remove(target, ignored);
            return false;
        }
        output = PreparedMediaFile{
            attachmentId,
            encryptedPath,
            std::filesystem::file_size(target),
            HashFile(encryptedPath),
            encryptedMetadata.ciphertext,
            encryptedMetadata.nonce,
            mime,
        };
        crypto_wipe(fileKey.data(), fileKey.size());
        return true;
    } catch (const std::exception& exception) {
        std::filesystem::remove(std::filesystem::path(encryptedPath), ignored);
        error = exception.what();
        return false;
    }
}

bool MediaCrypto::DecryptPreparedFile(const MessageCrypto&,
                                      const std::span<const std::uint8_t, 32> conversationKey,
                                      const std::string& attachmentId,
                                      const std::wstring& encryptedPath,
                                      const std::wstring& outputPath,
                                      const std::string& expectedCiphertextSha256,
                                      std::string& error) {
    std::error_code ignored;
    try {
        if (HashFile(encryptedPath) != expectedCiphertextSha256) {
            error = "Sifreli medya SHA-256 dogrulamasi basarisiz";
            return false;
        }
        std::ifstream input(std::filesystem::path(encryptedPath), std::ios::binary);
        if (!input) throw std::runtime_error("Sifreli medya acilamadi");
        std::array<std::uint8_t, kHeaderBytes> header{};
        input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
        if (input.gcount() != static_cast<std::streamsize>(header.size())
            || !std::equal(kMagic.begin(), kMagic.end(), header.begin())) {
            throw std::runtime_error("Sifreli medya formati gecersiz");
        }
        const std::uint64_t originalSize = GetU64(std::span<const std::uint8_t>(header).subspan(8, 8));
        const std::uint32_t chunkBytes = GetU32(std::span<const std::uint8_t>(header).subspan(16, 4));
        if (originalSize == 0 || originalSize > 100U * 1024U * 1024U || chunkBytes == 0
            || chunkBytes > 1024U * 1024U) throw std::runtime_error("Sifreli medya sinirlari gecersiz");
        const std::filesystem::path target(outputPath);
        std::filesystem::create_directories(target.parent_path());
        std::ofstream clearOutput(target, std::ios::binary | std::ios::trunc);
        if (!clearOutput) throw std::runtime_error("Medya cikti dosyasi acilamadi");

        auto fileKey = FileKey(conversationKey, attachmentId);
        std::vector<std::uint8_t> cipher(chunkBytes);
        std::vector<std::uint8_t> clear(chunkBytes);
        std::array<std::uint8_t, 16> mac{};
        std::uint64_t remaining = originalSize;
        std::uint64_t chunkIndex = 0;
        while (remaining > 0) {
            const std::size_t bytes = static_cast<std::size_t>(std::min<std::uint64_t>(chunkBytes, remaining));
            input.read(reinterpret_cast<char*>(mac.data()), static_cast<std::streamsize>(mac.size()));
            input.read(reinterpret_cast<char*>(cipher.data()), static_cast<std::streamsize>(bytes));
            if (!input || input.gcount() != static_cast<std::streamsize>(bytes)) {
                throw std::runtime_error("Sifreli medya eksik");
            }
            const auto nonce = ChunkNonce(std::span<const std::uint8_t, 16>(header.data() + 20, 16), chunkIndex);
            const std::string associated = AssociatedData(attachmentId, chunkIndex);
            if (crypto_aead_unlock(clear.data(), mac.data(), fileKey.data(), nonce.data(),
                                   reinterpret_cast<const std::uint8_t*>(associated.data()), associated.size(),
                                   cipher.data(), bytes) != 0) {
                throw std::runtime_error("Medya parcasi dogrulanamadi");
            }
            clearOutput.write(reinterpret_cast<const char*>(clear.data()), static_cast<std::streamsize>(bytes));
            remaining -= bytes;
            ++chunkIndex;
        }
        char trailing = 0;
        if (input.read(&trailing, 1)) throw std::runtime_error("Sifreli medya sonunda beklenmeyen veri var");
        clearOutput.flush();
        if (!clearOutput) throw std::runtime_error("Medya cikti tamamlanamadi");
        crypto_wipe(fileKey.data(), fileKey.size());
        return true;
    } catch (const std::exception& exception) {
        std::filesystem::remove(std::filesystem::path(outputPath), ignored);
        error = exception.what();
        return false;
    }
}

bool MediaCrypto::DecryptMetadata(const MessageCrypto& crypto,
                                  const std::span<const std::uint8_t, 32> conversationKey,
                                  const std::string& attachmentId,
                                  const std::string& metadataCiphertext,
                                  const std::string& metadataNonce,
                                  MediaFileMetadata& output,
                                  std::string& error) {
    auto fileKey = FileKey(conversationKey, attachmentId);
    const std::string associated = "SonalisMediaMetadataV1\n" + attachmentId;
    std::string clear;
    const bool decrypted = crypto.DecryptMessage(
        fileKey, associated, {metadataCiphertext, metadataNonce}, clear);
    crypto_wipe(fileKey.data(), fileKey.size());
    if (!decrypted) {
        error = "Medya metadata dogrulanamadi";
        return false;
    }
    try {
        const auto json = nlohmann::json::parse(clear);
        const std::filesystem::path safeName = std::filesystem::path(
            Utf8ToWide(json.at("name").get<std::string>())).filename();
        output.name = WideToUtf8(safeName.wstring());
        output.mime = json.at("mime").get<std::string>();
        output.originalSize = json.at("originalSize").get<std::uint64_t>();
        crypto_wipe(clear.data(), clear.size());
        return !output.name.empty() && output.originalSize > 0;
    } catch (const std::exception& exception) {
        crypto_wipe(clear.data(), clear.size());
        error = exception.what();
        return false;
    }
}

}  // namespace ss
