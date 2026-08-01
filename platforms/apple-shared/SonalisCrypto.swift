import Foundation
import Security

enum SonalisCryptoError: Error { case invalidInput, randomFailure, operationFailed, authenticationFailed }

struct SonalisSigningKeyPair {
    var secret: Data
    let publicKey: Data
    mutating func wipe() { secret.resetBytes(in: 0..<secret.count) }
}

enum SonalisCrypto {
    static func randomBytes(_ count: Int) throws -> Data {
        guard count > 0, count <= 65_536 else { throw SonalisCryptoError.invalidInput }
        var data = Data(count: count)
        let status = data.withUnsafeMutableBytes { buffer in
            SecRandomCopyBytes(kSecRandomDefault, count, buffer.baseAddress!)
        }
        guard status == errSecSuccess else { throw SonalisCryptoError.randomFailure }
        return data
    }

    static func encrypt(key: Data, nonce: Data, associatedData: Data, plaintext: Data) throws -> Data {
        guard key.count == 32, nonce.count == 24, plaintext.count <= 2 * 1024 * 1024 else {
            throw SonalisCryptoError.invalidInput
        }
        var output = Data(count: plaintext.count + 16)
        let result = output.withUnsafeMutableBytes { out in
            key.withUnsafeBytes { keyBytes in nonce.withUnsafeBytes { nonceBytes in
                associatedData.withUnsafeBytes { aad in plaintext.withUnsafeBytes { clear in
                    sonalis_crypto_aead_lock(out.bindMemory(to: UInt8.self).baseAddress, UInt64(out.count),
                        keyBytes.bindMemory(to: UInt8.self).baseAddress,
                        nonceBytes.bindMemory(to: UInt8.self).baseAddress,
                        aad.bindMemory(to: UInt8.self).baseAddress, UInt64(aad.count),
                        clear.bindMemory(to: UInt8.self).baseAddress, UInt64(clear.count))
                }}
            }}
        }
        guard result == 1 else { throw SonalisCryptoError.operationFailed }
        return output
    }

    static func decrypt(key: Data, nonce: Data, associatedData: Data, ciphertext: Data) throws -> Data {
        guard key.count == 32, nonce.count == 24, ciphertext.count >= 16,
              ciphertext.count <= 2 * 1024 * 1024 else { throw SonalisCryptoError.invalidInput }
        var output = Data(count: ciphertext.count - 16)
        let result = output.withUnsafeMutableBytes { out in
            key.withUnsafeBytes { keyBytes in nonce.withUnsafeBytes { nonceBytes in
                associatedData.withUnsafeBytes { aad in ciphertext.withUnsafeBytes { cipher in
                    sonalis_crypto_aead_unlock(out.bindMemory(to: UInt8.self).baseAddress, UInt64(out.count),
                        keyBytes.bindMemory(to: UInt8.self).baseAddress,
                        nonceBytes.bindMemory(to: UInt8.self).baseAddress,
                        aad.bindMemory(to: UInt8.self).baseAddress, UInt64(aad.count),
                        cipher.bindMemory(to: UInt8.self).baseAddress, UInt64(cipher.count))
                }}
            }}
        }
        guard result == 1 else { throw SonalisCryptoError.authenticationFailed }
        return output
    }

    static func signingKeyPair() throws -> SonalisSigningKeyPair {
        var seed = try randomBytes(32)
        defer { seed.resetBytes(in: 0..<seed.count) }
        var secret = Data(count: 64)
        var publicKey = Data(count: 32)
        secret.withUnsafeMutableBytes { secretBytes in
            publicKey.withUnsafeMutableBytes { publicBytes in
                seed.withUnsafeBytes { seedBytes in
                    sonalis_crypto_ed25519_key_pair(
                        secretBytes.bindMemory(to: UInt8.self).baseAddress,
                        publicBytes.bindMemory(to: UInt8.self).baseAddress,
                        seedBytes.bindMemory(to: UInt8.self).baseAddress)
                }
            }
        }
        return SonalisSigningKeyPair(secret: secret, publicKey: publicKey)
    }

    static func sign(secret: Data, message: Data) throws -> Data {
        guard secret.count == 64 else { throw SonalisCryptoError.invalidInput }
        var signature = Data(count: 64)
        signature.withUnsafeMutableBytes { signatureBytes in
            secret.withUnsafeBytes { secretBytes in message.withUnsafeBytes { messageBytes in
                sonalis_crypto_ed25519_sign(signatureBytes.bindMemory(to: UInt8.self).baseAddress,
                    secretBytes.bindMemory(to: UInt8.self).baseAddress,
                    messageBytes.bindMemory(to: UInt8.self).baseAddress, UInt64(messageBytes.count))
            }}
        }
        return signature
    }

    static func verify(publicKey: Data, message: Data, signature: Data) -> Bool {
        guard publicKey.count == 32, signature.count == 64 else { return false }
        return signature.withUnsafeBytes { signatureBytes in
            publicKey.withUnsafeBytes { publicBytes in message.withUnsafeBytes { messageBytes in
                sonalis_crypto_ed25519_verify(signatureBytes.bindMemory(to: UInt8.self).baseAddress,
                    publicBytes.bindMemory(to: UInt8.self).baseAddress,
                    messageBytes.bindMemory(to: UInt8.self).baseAddress, UInt64(messageBytes.count)) == 1
            }}
        }
    }
}
