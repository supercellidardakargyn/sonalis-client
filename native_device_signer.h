#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ncrypt.h>

#include <mutex>
#include <string>

namespace ss {

// Native login proof backed by the Microsoft Platform Crypto Provider. The
// private P-256 key is non-exportable and remains in the TPM. Callers retain
// the existing DPAPI-protected Ed25519 fallback when this provider is absent.
class NativeDeviceSigner final {
public:
    NativeDeviceSigner() = default;
    ~NativeDeviceSigner();
    NativeDeviceSigner(const NativeDeviceSigner&) = delete;
    NativeDeviceSigner& operator=(const NativeDeviceSigner&) = delete;

    bool Initialize(const std::string& installationId, std::string& error);
    [[nodiscard]] bool IsReady() const noexcept;
    [[nodiscard]] std::string Algorithm() const;
    [[nodiscard]] std::string PublicKey() const;
    [[nodiscard]] std::string Sign(const std::string& canonical, std::string& error) const;

private:
    void Reset() noexcept;

    mutable std::mutex mutex_;
    NCRYPT_PROV_HANDLE provider_{};
    NCRYPT_KEY_HANDLE key_{};
    std::string publicKey_;
};

}  // namespace ss
