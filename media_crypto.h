#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

#include "message_crypto.h"

namespace ss {

struct PreparedMediaFile {
    std::string attachmentId;
    std::wstring encryptedPath;
    std::uint64_t encryptedSize{};
    std::string ciphertextSha256;
    std::string metadataCiphertext;
    std::string metadataNonce;
    std::string mimeHint;
};
struct MediaFileMetadata {
    std::string name;
    std::string mime;
    std::uint64_t originalSize{};
};

class MediaCrypto final {
public:
    static bool PrepareFile(const MessageCrypto& crypto,
                            std::span<const std::uint8_t, 32> conversationKey,
                            const std::string& attachmentId,
                            const std::wstring& sourcePath,
                            const std::wstring& encryptedPath,
                            PreparedMediaFile& output,
                            std::string& error);
    static bool DecryptPreparedFile(const MessageCrypto& crypto,
                                    std::span<const std::uint8_t, 32> conversationKey,
                                    const std::string& attachmentId,
                                    const std::wstring& encryptedPath,
                                    const std::wstring& outputPath,
                                    const std::string& expectedCiphertextSha256,
                                    std::string& error);
    static bool DecryptMetadata(const MessageCrypto& crypto,
                                std::span<const std::uint8_t, 32> conversationKey,
                                const std::string& attachmentId,
                                const std::string& metadataCiphertext,
                                const std::string& metadataNonce,
                                MediaFileMetadata& output,
                                std::string& error);
    [[nodiscard]] static std::string MimeHint(const std::wstring& path);

private:
    [[nodiscard]] static std::array<std::uint8_t, 32> FileKey(
        std::span<const std::uint8_t, 32> conversationKey,
        const std::string& attachmentId);
};

}  // namespace ss
