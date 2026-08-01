#include <jni.h>

#include <cstdint>
#include <vector>

#include "sonalis/core/c_api.h"

namespace {

std::vector<std::uint8_t> Bytes(JNIEnv* environment, jbyteArray value, const jsize expected = -1) {
    if (value == nullptr) return {};
    const jsize size = environment->GetArrayLength(value);
    if ((expected >= 0 && size != expected) || size < 0 || size > 2 * 1024 * 1024) return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0) environment->GetByteArrayRegion(value, 0, size, reinterpret_cast<jbyte*>(bytes.data()));
    return bytes;
}

jbyteArray Array(JNIEnv* environment, const std::vector<std::uint8_t>& bytes) {
    jbyteArray output = environment->NewByteArray(static_cast<jsize>(bytes.size()));
    if (output != nullptr && !bytes.empty()) {
        environment->SetByteArrayRegion(output, 0, static_cast<jsize>(bytes.size()),
                                        reinterpret_cast<const jbyte*>(bytes.data()));
    }
    return output;
}

}  // namespace

extern "C" JNIEXPORT jbyteArray JNICALL
Java_tr_sonalis_core_NativeCrypto_nativeEncrypt(JNIEnv* environment, jclass, jbyteArray keyValue,
                                                 jbyteArray nonceValue, jbyteArray aadValue,
                                                 jbyteArray plaintextValue) {
    auto key = Bytes(environment, keyValue, 32);
    auto nonce = Bytes(environment, nonceValue, 24);
    auto aad = Bytes(environment, aadValue);
    auto plaintext = Bytes(environment, plaintextValue);
    if (key.size() != 32 || nonce.size() != 24 || plaintextValue == nullptr) return nullptr;
    std::vector<std::uint8_t> cipher(plaintext.size() + 16);
    if (!sonalis_crypto_aead_lock(cipher.data(), cipher.size(), key.data(), nonce.data(),
                                  aad.data(), aad.size(), plaintext.data(), plaintext.size())) return nullptr;
    sonalis_crypto_wipe(plaintext.data(), plaintext.size());
    return Array(environment, cipher);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_tr_sonalis_core_NativeCrypto_nativeDecrypt(JNIEnv* environment, jclass, jbyteArray keyValue,
                                                 jbyteArray nonceValue, jbyteArray aadValue,
                                                 jbyteArray cipherValue) {
    auto key = Bytes(environment, keyValue, 32);
    auto nonce = Bytes(environment, nonceValue, 24);
    auto aad = Bytes(environment, aadValue);
    auto cipher = Bytes(environment, cipherValue);
    if (key.size() != 32 || nonce.size() != 24 || cipher.size() < 16) return nullptr;
    std::vector<std::uint8_t> clear(cipher.size() - 16);
    if (!sonalis_crypto_aead_unlock(clear.data(), clear.size(), key.data(), nonce.data(),
                                    aad.data(), aad.size(), cipher.data(), cipher.size())) return nullptr;
    return Array(environment, clear);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_tr_sonalis_core_NativeCrypto_nativeEd25519KeyPair(JNIEnv* environment, jclass, jbyteArray seedValue) {
    auto seed = Bytes(environment, seedValue, 32);
    if (seed.size() != 32) return nullptr;
    std::vector<std::uint8_t> pair(96);
    sonalis_crypto_ed25519_key_pair(pair.data(), pair.data() + 64, seed.data());
    sonalis_crypto_wipe(seed.data(), seed.size());
    return Array(environment, pair);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_tr_sonalis_core_NativeCrypto_nativeEd25519Sign(JNIEnv* environment, jclass, jbyteArray secretValue,
                                                    jbyteArray messageValue) {
    auto secret = Bytes(environment, secretValue, 64);
    auto message = Bytes(environment, messageValue);
    if (secret.size() != 64 || messageValue == nullptr) return nullptr;
    std::vector<std::uint8_t> signature(64);
    sonalis_crypto_ed25519_sign(signature.data(), secret.data(), message.data(), message.size());
    sonalis_crypto_wipe(secret.data(), secret.size());
    return Array(environment, signature);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_tr_sonalis_core_NativeCrypto_nativeEd25519Verify(JNIEnv* environment, jclass,
                                                      jbyteArray publicValue, jbyteArray messageValue,
                                                      jbyteArray signatureValue) {
    const auto publicKey = Bytes(environment, publicValue, 32);
    const auto message = Bytes(environment, messageValue);
    const auto signature = Bytes(environment, signatureValue, 64);
    return publicKey.size() == 32 && signature.size() == 64 && messageValue != nullptr
        && sonalis_crypto_ed25519_verify(signature.data(), publicKey.data(), message.data(), message.size())
        ? JNI_TRUE : JNI_FALSE;
}
