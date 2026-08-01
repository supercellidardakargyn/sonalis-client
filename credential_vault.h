#pragma once

#include <string>

namespace ss {

class CredentialVault final {
public:
    bool SaveRefreshToken(const std::string& token, std::string& error) const;
    [[nodiscard]] std::string LoadRefreshToken() const;
    void ClearRefreshToken() const noexcept;
    bool SaveDeviceLicense(const std::string& certificate, std::string& error) const;
    [[nodiscard]] std::string LoadDeviceLicense() const;
    void ClearDeviceLicense() const noexcept;
};

}  // namespace ss
