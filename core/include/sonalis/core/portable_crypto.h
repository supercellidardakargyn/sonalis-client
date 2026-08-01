#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace sonalis::core {

inline constexpr std::size_t SymmetricKeyBytes = 32;
inline constexpr std::size_t XChaChaNonceBytes = 24;
inline constexpr std::size_t AeadTagBytes = 16;
inline constexpr std::size_t Ed25519PublicBytes = 32;
inline constexpr std::size_t Ed25519SecretBytes = 64;
inline constexpr std::size_t Ed25519SignatureBytes = 64;

bool AeadLock(std::span<std::uint8_t> packedCiphertext,
              std::span<const std::uint8_t, SymmetricKeyBytes> key,
              std::span<const std::uint8_t, XChaChaNonceBytes> nonce,
              std::span<const std::uint8_t> associatedData,
              std::span<const std::uint8_t> plaintext) noexcept;
bool AeadUnlock(std::span<std::uint8_t> plaintext,
                std::span<const std::uint8_t, SymmetricKeyBytes> key,
                std::span<const std::uint8_t, XChaChaNonceBytes> nonce,
                std::span<const std::uint8_t> associatedData,
                std::span<const std::uint8_t> packedCiphertext) noexcept;
void Ed25519KeyPair(std::span<std::uint8_t, Ed25519SecretBytes> secret,
                    std::span<std::uint8_t, Ed25519PublicBytes> publicKey,
                    std::span<const std::uint8_t, 32> seed) noexcept;
void Ed25519Sign(std::span<std::uint8_t, Ed25519SignatureBytes> signature,
                 std::span<const std::uint8_t, Ed25519SecretBytes> secret,
                 std::span<const std::uint8_t> message) noexcept;
bool Ed25519Verify(std::span<const std::uint8_t, Ed25519SignatureBytes> signature,
                   std::span<const std::uint8_t, Ed25519PublicBytes> publicKey,
                   std::span<const std::uint8_t> message) noexcept;
void X25519Public(std::span<std::uint8_t, 32> publicKey,
                  std::span<const std::uint8_t, 32> secret) noexcept;
void X25519(std::span<std::uint8_t, 32> shared,
            std::span<const std::uint8_t, 32> secret,
            std::span<const std::uint8_t, 32> publicKey) noexcept;
void SecureWipe(std::span<std::uint8_t> bytes) noexcept;

}  // namespace sonalis::core
