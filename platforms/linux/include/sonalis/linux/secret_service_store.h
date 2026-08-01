#pragma once

#include "sonalis/core/secure_store.h"

namespace sonalis::linux_platform {

class SecretServiceStore final : public core::SecureStore {
public:
    bool Put(std::string key, std::span<const std::uint8_t> secret, std::string& error) override;
    [[nodiscard]] std::vector<std::uint8_t> Get(std::string key, std::string& error) override;
    bool Erase(std::string key, std::string& error) override;
};

}  // namespace sonalis::linux_platform

