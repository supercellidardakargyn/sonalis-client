import Foundation
import Security

enum SonalisKeychainError: Error { case invalidKey, status(OSStatus) }

struct SonalisKeychainStore: SecureTokenStore {
    private let service = "tr.sonalis.mobile"

    func put(_ value: Data, key: String) throws {
        try validate(key)
        var query = baseQuery(key)
        SecItemDelete(query as CFDictionary)
        query[kSecValueData as String] = value
        query[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        let status = SecItemAdd(query as CFDictionary, nil)
        guard status == errSecSuccess else { throw SonalisKeychainError.status(status) }
    }

    func get(_ key: String) throws -> Data? {
        try validate(key)
        var query = baseQuery(key)
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess, let data = result as? Data else {
            throw SonalisKeychainError.status(status)
        }
        return data
    }

    func erase(_ key: String) throws {
        try validate(key)
        let status = SecItemDelete(baseQuery(key) as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else {
            throw SonalisKeychainError.status(status)
        }
    }

    private func baseQuery(_ key: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service,
         kSecAttrAccount as String: key,
         kSecAttrSynchronizable as String: kCFBooleanFalse as Any]
    }

    private func validate(_ key: String) throws {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "._-"))
        guard !key.isEmpty, key.utf8.count <= 128,
              key.unicodeScalars.allSatisfy({ allowed.contains($0) }) else {
            throw SonalisKeychainError.invalidKey
        }
    }
}
