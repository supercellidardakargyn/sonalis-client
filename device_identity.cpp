#include "device_identity.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "win_helpers.h"

namespace ss {
namespace {

bool Sha256(const std::string_view input, std::array<std::uint8_t, 32>& digest) noexcept {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0;
    DWORD written = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &written, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    std::vector<std::uint8_t> object(objectBytes);
    const NTSTATUS created = BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0);
    if (created < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    const NTSTATUS updated = BCryptHashData(
        hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
        static_cast<ULONG>(input.size()), 0);
    const NTSTATUS finished = updated < 0
        ? updated : BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return finished >= 0;
}

}  // namespace

std::string DeviceBindingIdFromMachineGuid(const std::string_view machineGuid) {
    if (machineGuid.empty() || machineGuid.size() > 256U) return {};
    std::string normalized(machineGuid);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    constexpr std::string_view productNamespace = "Sonalis.DeviceBinding.v1\n";
    std::string material;
    material.reserve(productNamespace.size() + normalized.size());
    material.append(productNamespace);
    material.append(normalized);
    std::array<std::uint8_t, 32> digest{};
    if (!Sha256(material, digest)) return {};
    digest[6] = static_cast<std::uint8_t>((digest[6] & 0x0FU) | 0x50U);
    digest[8] = static_cast<std::uint8_t>((digest[8] & 0x3FU) | 0x80U);
    constexpr char digits[] = "0123456789abcdef";
    std::string uuid;
    uuid.reserve(36U);
    for (std::size_t index = 0; index < 16U; ++index) {
        if (index == 4U || index == 6U || index == 8U || index == 10U) uuid.push_back('-');
        uuid.push_back(digits[digest[index] >> 4U]);
        uuid.push_back(digits[digest[index] & 0x0FU]);
    }
    return uuid;
}

std::string StableDeviceBindingId() {
    try {
        const std::filesystem::path root = LocalAppDataPath();
        if (root.empty()) return {};
        const std::filesystem::path path = root / L"Sonalis" / L"installation-id";
        {
            std::ifstream input(path, std::ios::binary);
            std::string existing;
            if (input && std::getline(input, existing) && existing.size() == 36U) return existing;
        }
        std::array<std::uint8_t, 16> random{};
        if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
        random[6] = static_cast<std::uint8_t>((random[6] & 0x0FU) | 0x40U);
        random[8] = static_cast<std::uint8_t>((random[8] & 0x3FU) | 0x80U);
        constexpr char digits[] = "0123456789abcdef";
        std::string id;
        id.reserve(36U);
        for (std::size_t index = 0; index < random.size(); ++index) {
            if (index == 4U || index == 6U || index == 8U || index == 10U) id.push_back('-');
            id.push_back(digits[random[index] >> 4U]);
            id.push_back(digits[random[index] & 0x0FU]);
        }
        std::filesystem::create_directories(path.parent_path());
        const std::filesystem::path temporary = path.wstring() + L".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << id << '\n';
            output.flush();
            if (!output) return {};
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::filesystem::remove(temporary);
            return {};
        }
        return id;
    } catch (...) {
        return {};
    }
}

}  // namespace ss
