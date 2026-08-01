#include "sonalis/core/portable_crypto.h"

#include <monocypher-ed25519.h>
#include <monocypher.h>

#include <array>
#include <algorithm>

namespace sonalis::core {

bool AeadLock(const std::span<std::uint8_t> packedCiphertext,
              const std::span<const std::uint8_t, SymmetricKeyBytes> key,
              const std::span<const std::uint8_t, XChaChaNonceBytes> nonce,
              const std::span<const std::uint8_t> associatedData,
              const std::span<const std::uint8_t> plaintext) noexcept {
    if (packedCiphertext.size() != plaintext.size() + AeadTagBytes) return false;
    crypto_aead_lock(packedCiphertext.data() + AeadTagBytes, packedCiphertext.data(), key.data(), nonce.data(),
                     associatedData.data(), associatedData.size(), plaintext.data(), plaintext.size());
    return true;
}

bool AeadUnlock(const std::span<std::uint8_t> plaintext,
                const std::span<const std::uint8_t, SymmetricKeyBytes> key,
                const std::span<const std::uint8_t, XChaChaNonceBytes> nonce,
                const std::span<const std::uint8_t> associatedData,
                const std::span<const std::uint8_t> packedCiphertext) noexcept {
    if (packedCiphertext.size() < AeadTagBytes || plaintext.size() + AeadTagBytes != packedCiphertext.size()) {
        return false;
    }
    return crypto_aead_unlock(plaintext.data(), packedCiphertext.data(), key.data(), nonce.data(),
                              associatedData.data(), associatedData.size(),
                              packedCiphertext.data() + AeadTagBytes, plaintext.size()) == 0;
}

void Ed25519KeyPair(const std::span<std::uint8_t, Ed25519SecretBytes> secret,
                    const std::span<std::uint8_t, Ed25519PublicBytes> publicKey,
                    const std::span<const std::uint8_t, 32> seed) noexcept {
    std::array<std::uint8_t, 32> mutableSeed{};
    std::copy(seed.begin(), seed.end(), mutableSeed.begin());
    crypto_ed25519_key_pair(secret.data(), publicKey.data(), mutableSeed.data());
    crypto_wipe(mutableSeed.data(), mutableSeed.size());
}

void Ed25519Sign(const std::span<std::uint8_t, Ed25519SignatureBytes> signature,
                 const std::span<const std::uint8_t, Ed25519SecretBytes> secret,
                 const std::span<const std::uint8_t> message) noexcept {
    crypto_ed25519_sign(signature.data(), secret.data(), message.data(), message.size());
}

bool Ed25519Verify(const std::span<const std::uint8_t, Ed25519SignatureBytes> signature,
                   const std::span<const std::uint8_t, Ed25519PublicBytes> publicKey,
                   const std::span<const std::uint8_t> message) noexcept {
    return crypto_ed25519_check(signature.data(), publicKey.data(), message.data(), message.size()) == 0;
}

void X25519Public(const std::span<std::uint8_t, 32> publicKey,
                  const std::span<const std::uint8_t, 32> secret) noexcept {
    crypto_x25519_public_key(publicKey.data(), secret.data());
}

void X25519(const std::span<std::uint8_t, 32> shared,
            const std::span<const std::uint8_t, 32> secret,
            const std::span<const std::uint8_t, 32> publicKey) noexcept {
    crypto_x25519(shared.data(), secret.data(), publicKey.data());
}

void SecureWipe(const std::span<std::uint8_t> bytes) noexcept { crypto_wipe(bytes.data(), bytes.size()); }

}  // namespace sonalis::core
