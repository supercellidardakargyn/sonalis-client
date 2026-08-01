package tr.sonalis.core

import java.security.SecureRandom

object NativeCrypto {
    private val random = SecureRandom()

    init { System.loadLibrary("sonalis_android") }

    data class SigningKeyPair(val secret: ByteArray, val publicKey: ByteArray) : AutoCloseable {
        override fun close() { secret.fill(0) }
    }

    fun randomBytes(size: Int): ByteArray {
        require(size in 1..65_536)
        return ByteArray(size).also(random::nextBytes)
    }

    fun encrypt(key: ByteArray, nonce: ByteArray, associatedData: ByteArray,
                plaintext: ByteArray): ByteArray {
        require(key.size == 32 && nonce.size == 24 && plaintext.size <= 2 * 1024 * 1024)
        return nativeEncrypt(key, nonce, associatedData, plaintext.copyOf())
            ?: throw SecurityException("encryption_failed")
    }

    fun decrypt(key: ByteArray, nonce: ByteArray, associatedData: ByteArray,
                ciphertext: ByteArray): ByteArray {
        require(key.size == 32 && nonce.size == 24 && ciphertext.size in 16..2 * 1024 * 1024)
        return nativeDecrypt(key, nonce, associatedData, ciphertext)
            ?: throw SecurityException("authentication_failed")
    }

    fun signingKeyPair(seed: ByteArray = randomBytes(32)): SigningKeyPair {
        require(seed.size == 32)
        val localSeed = seed.copyOf()
        val pair = nativeEd25519KeyPair(localSeed) ?: throw SecurityException("key_generation_failed")
        localSeed.fill(0)
        return SigningKeyPair(pair.copyOfRange(0, 64), pair.copyOfRange(64, 96)).also { pair.fill(0) }
    }

    fun sign(secret: ByteArray, message: ByteArray): ByteArray {
        require(secret.size == 64)
        return nativeEd25519Sign(secret, message) ?: throw SecurityException("signing_failed")
    }

    fun verify(publicKey: ByteArray, message: ByteArray, signature: ByteArray): Boolean =
        publicKey.size == 32 && signature.size == 64 && nativeEd25519Verify(publicKey, message, signature)

    @JvmStatic private external fun nativeEncrypt(key: ByteArray, nonce: ByteArray, aad: ByteArray,
                                                  plaintext: ByteArray): ByteArray?
    @JvmStatic private external fun nativeDecrypt(key: ByteArray, nonce: ByteArray, aad: ByteArray,
                                                  ciphertext: ByteArray): ByteArray?
    @JvmStatic private external fun nativeEd25519KeyPair(seed: ByteArray): ByteArray?
    @JvmStatic private external fun nativeEd25519Sign(secret: ByteArray, message: ByteArray): ByteArray?
    @JvmStatic private external fun nativeEd25519Verify(publicKey: ByteArray, message: ByteArray,
                                                        signature: ByteArray): Boolean
}
