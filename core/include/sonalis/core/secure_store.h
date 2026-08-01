#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sonalis::core {

class SecureStore {
public:
    virtual ~SecureStore() = default;
    virtual bool Put(std::string key, std::span<const std::uint8_t> secret, std::string& error) = 0;
    [[nodiscard]] virtual std::vector<std::uint8_t> Get(std::string key, std::string& error) = 0;
    virtual bool Erase(std::string key, std::string& error) = 0;
};

}  // namespace sonalis::core

