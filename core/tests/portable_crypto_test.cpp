#include "sonalis/core/portable_crypto.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <string_view>

int main() {
    using namespace sonalis::core;
    std::array<std::uint8_t, 32> key{};
    std::array<std::uint8_t, 24> nonce{};
    for (std::size_t index = 0; index < key.size(); ++index) key[index] = static_cast<std::uint8_t>(index + 1);
    for (std::size_t index = 0; index < nonce.size(); ++index) nonce[index] = static_cast<std::uint8_t>(index + 31);
    constexpr std::string_view aad = "channel:epoch:message";
    constexpr std::string_view clear = "Sonalis portable encrypted message";
    std::array<std::uint8_t, clear.size() + AeadTagBytes> cipher{};
    std::array<std::uint8_t, clear.size()> output{};
    assert(AeadLock(cipher, key, nonce,
                    {reinterpret_cast<const std::uint8_t*>(aad.data()), aad.size()},
                    {reinterpret_cast<const std::uint8_t*>(clear.data()), clear.size()}));
    assert(AeadUnlock(output, key, nonce,
                      {reinterpret_cast<const std::uint8_t*>(aad.data()), aad.size()}, cipher));
    assert(std::string_view(reinterpret_cast<const char*>(output.data()), output.size()) == clear);
    cipher[5] ^= 1;
    assert(!AeadUnlock(output, key, nonce,
                       {reinterpret_cast<const std::uint8_t*>(aad.data()), aad.size()}, cipher));

    std::array<std::uint8_t, 32> seed{};
    seed[0] = 42;
    std::array<std::uint8_t, 64> secret{};
    std::array<std::uint8_t, 32> publicKey{};
    std::array<std::uint8_t, 64> signature{};
    Ed25519KeyPair(secret, publicKey, seed);
    Ed25519Sign(signature, secret,
                {reinterpret_cast<const std::uint8_t*>(clear.data()), clear.size()});
    assert(Ed25519Verify(signature, publicKey,
                         {reinterpret_cast<const std::uint8_t*>(clear.data()), clear.size()}));
    signature[0] ^= 1;
    assert(!Ed25519Verify(signature, publicKey,
                          {reinterpret_cast<const std::uint8_t*>(clear.data()), clear.size()}));
    SecureWipe(secret);
    for (const auto value : secret) assert(value == 0);
    return 0;
}
