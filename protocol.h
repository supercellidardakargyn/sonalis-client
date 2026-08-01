#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ss::protocol {

inline constexpr std::uint8_t kMagic0 = 0x53;
inline constexpr std::uint8_t kMagic1 = 0x53;
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::uint16_t kDefaultPort = 25565;
inline constexpr std::size_t kTokenBytes = 16;
inline constexpr std::size_t kRegisterBytes = 20;
inline constexpr std::size_t kAudioUpHeaderBytes = 27;
inline constexpr std::size_t kAudioDownHeaderBytes = 15;
inline constexpr std::size_t kMaxDatagramBytes = 1400;
inline constexpr std::size_t kMaxOpusBytes = 1275;
inline constexpr std::uint8_t kAudioFlagTalkStart = 0x01;

enum class DatagramType : std::uint8_t {
    Register = 1,
    RegisterAck = 2,
    AudioUp = 3,
    AudioDown = 4,
    KeepAliveUp = 5,
    KeepAliveAck = 6,
};

struct ServerAddress {
    std::string host;
    std::uint16_t port{kDefaultPort};
};

struct DownlinkAudioPacket {
    std::uint32_t peerId{};
    std::uint16_t sequence{};
    std::uint32_t timestamp{};
    std::uint8_t flags{};
    std::span<const std::uint8_t> opus;
};

bool ParseServerAddress(const std::string& value, ServerAddress& result, std::string& error);
bool DecodeHexToken(const std::string& hex, std::array<std::uint8_t, kTokenBytes>& token);
std::size_t BuildRegister(std::span<std::uint8_t> output,
                          const std::array<std::uint8_t, kTokenBytes>& token);
std::size_t BuildKeepAlive(std::span<std::uint8_t> output,
                           const std::array<std::uint8_t, kTokenBytes>& token);
std::size_t BuildAudioUp(std::span<std::uint8_t> output,
                         const std::array<std::uint8_t, kTokenBytes>& token,
                         std::uint16_t sequence,
                         std::uint32_t timestamp,
                         std::uint8_t flags,
                         std::span<const std::uint8_t> opus);
bool ParseRegisterAck(std::span<const std::uint8_t> packet, std::uint32_t& peerId);
bool ParseKeepAliveAck(std::span<const std::uint8_t> packet);
bool ParseAudioDown(std::span<const std::uint8_t> packet, DownlinkAudioPacket& result);

std::vector<std::uint8_t> EncodeJsonFrame(const std::string& json);

}  // namespace ss::protocol
