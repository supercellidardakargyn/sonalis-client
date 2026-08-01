package tr.sonalis.core

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

class SonalisSecureStore(context: Context) {
    private val preferences = context.getSharedPreferences("sonalis_secure_store", Context.MODE_PRIVATE)

    fun put(key: String, value: ByteArray) {
        validate(key, value)
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.ENCRYPT_MODE, encryptionKey())
        val encrypted = cipher.doFinal(value)
        val record = byteArrayOf(FORMAT_VERSION, cipher.iv.size.toByte()) + cipher.iv + encrypted
        check(preferences.edit().putString(key, Base64.encodeToString(record, Base64.NO_WRAP)).commit()) {
            "secure_store_write_failed"
        }
        record.fill(0)
        encrypted.fill(0)
    }

    fun get(key: String): ByteArray? {
        validateKey(key)
        val encoded = preferences.getString(key, null) ?: return null
        val record = Base64.decode(encoded, Base64.NO_WRAP)
        try {
            require(record.size >= 2 + 12 + 16 && record[0] == FORMAT_VERSION) { "secure_store_record_invalid" }
            val ivSize = record[1].toInt() and 0xff
            require(ivSize in 12..32 && 2 + ivSize < record.size) { "secure_store_iv_invalid" }
            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(Cipher.DECRYPT_MODE, encryptionKey(), GCMParameterSpec(128, record, 2, ivSize))
            return cipher.doFinal(record, 2 + ivSize, record.size - 2 - ivSize)
        } finally {
            record.fill(0)
        }
    }

    fun erase(key: String) {
        validateKey(key)
        check(preferences.edit().remove(key).commit()) { "secure_store_delete_failed" }
    }

    private fun encryptionKey(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(KEY_ALIAS, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").run {
            init(KeyGenParameterSpec.Builder(KEY_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .setRandomizedEncryptionRequired(true)
                .setUserAuthenticationRequired(false)
                .build())
            generateKey()
        }
    }

    private fun validate(key: String, value: ByteArray) {
        validateKey(key)
        require(value.isNotEmpty() && value.size <= 64 * 1024) { "secure_store_value_invalid" }
    }

    private fun validateKey(key: String) {
        require(key.length in 1..128 && key.all { it.isLetterOrDigit() || it in "._-" }) {
            "secure_store_key_invalid"
        }
    }

    private companion object {
        const val KEY_ALIAS = "tr.sonalis.mobile.secure-store.v1"
        const val TRANSFORMATION = "AES/GCM/NoPadding"
        const val FORMAT_VERSION: Byte = 1
    }
}

