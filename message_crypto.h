#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ss {

struct EncryptedMessagePayload {
    std::string ciphertext;
    std::string nonce;
};

class MessageCrypto final {
public:
    MessageCrypto() = default;
    ~MessageCrypto();
    MessageCrypto(const MessageCrypto&) = delete;
    MessageCrypto& operator=(const MessageCrypto&) = delete;

    bool Initialize(std::string& error);
    [[nodiscard]] bool IsReady() const noexcept;
    [[nodiscard]] const std::string& DeviceId() const noexcept;
    [[nodiscard]] static std::string NewId();
    [[nodiscard]] std::string EncryptionPublicKey() const;
    [[nodiscard]] std::string SigningPublicKey() const;
    [[nodiscard]] std::string AccountVaultEnvelope() const;

    bool RandomConversationKey(std::array<std::uint8_t, 32>& key, std::string& error) const;
    bool SealConversationKey(std::span<const std::uint8_t, 32> key,
                             const std::string& recipientPublicKey,
                             const std::string& context,
                             std::string& envelope,
                             std::string& error) const;
    bool OpenConversationKey(const std::string& envelope,
                             const std::string& context,
                             std::array<std::uint8_t, 32>& key,
                             std::string& error) const;
    bool EncryptMessage(std::span<const std::uint8_t, 32> key,
                        const std::string& associatedData,
                        const std::string& plaintext,
                        EncryptedMessagePayload& output,
                        std::string& error) const;
    bool DecryptMessage(std::span<const std::uint8_t, 32> key,
                        const std::string& associatedData,
                        const EncryptedMessagePayload& payload,
                        std::string& plaintext) const;
    bool SealModerationEvidence(std::span<const std::uint8_t> plaintext,
                                const std::string& moderationPublicKey,
                                std::vector<std::uint8_t>& ciphertext,
                                std::string& error) const;
    [[nodiscard]] static std::string Sha256Hex(std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::string Sign(const std::string& canonical) const;
    [[nodiscard]] static bool Verify(const std::string& publicKey, const std::string& canonical, const std::string& signature);

private:
    bool Save(std::string& error) const;
    [[nodiscard]] std::wstring VaultPath() const;

    std::string deviceId_;
    std::array<std::uint8_t, 32> encryptionSecret_{};
    std::array<std::uint8_t, 32> encryptionPublic_{};
    std::array<std::uint8_t, 64> signingSecret_{};
    std::array<std::uint8_t, 32> signingPublic_{};
    bool ready_{false};
};

}  // namespace ss
