#include "credential_vault.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincred.h>

#include <vector>

namespace ss {
namespace {
constexpr wchar_t kRefreshTarget[] = L"Sonalis:v3:refresh-token";
constexpr wchar_t kLicenseTarget[] = L"Sonalis:v4:device-license";

bool SaveSecret(const wchar_t* target, const std::string& value, std::string& error) {
    if (value.empty() || value.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) { error = "Gecersiz kimlik bilgisi"; return false; }
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC; credential.TargetName = const_cast<wchar_t*>(target);
    credential.CredentialBlobSize = static_cast<DWORD>(value.size());
    credential.CredentialBlob = reinterpret_cast<BYTE*>(const_cast<char*>(value.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE; credential.UserName = const_cast<wchar_t*>(L"Sonalis");
    if (!CredWriteW(&credential, 0)) { error = "Windows kimlik kasasina yazilamadi: " + std::to_string(GetLastError()); return false; }
    return true;
}

std::string LoadSecret(const wchar_t* target) {
    PCREDENTIALW raw = nullptr;
    if (!CredReadW(target, CRED_TYPE_GENERIC, 0, &raw) || raw == nullptr) return {};
    std::string value(reinterpret_cast<const char*>(raw->CredentialBlob), raw->CredentialBlobSize);
    CredFree(raw); return value;
}
}

bool CredentialVault::SaveRefreshToken(const std::string& token, std::string& error) const {
    return SaveSecret(kRefreshTarget, token, error);
}

std::string CredentialVault::LoadRefreshToken() const { return LoadSecret(kRefreshTarget); }

void CredentialVault::ClearRefreshToken() const noexcept { CredDeleteW(kRefreshTarget, CRED_TYPE_GENERIC, 0); }
bool CredentialVault::SaveDeviceLicense(const std::string& certificate, std::string& error) const {
    return SaveSecret(kLicenseTarget, certificate, error);
}
std::string CredentialVault::LoadDeviceLicense() const { return LoadSecret(kLicenseTarget); }
void CredentialVault::ClearDeviceLicense() const noexcept { CredDeleteW(kLicenseTarget, CRED_TYPE_GENERIC, 0); }

}  // namespace ss
