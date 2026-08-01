#include "protocol.h"

#include <algorithm>
#include <charconv>
#include <limits>

namespace ss::protocol {
namespace {

void Write16(std::uint8_t* output, const std::uint16_t value) {
    output[0] = static_cast<std::uint8_t>(value >> 8U);
    output[1] = static_cast<std::uint8_t>(value);
}

void Write32(std::uint8_t* output, const std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value >> 24U);
    output[1] = static_cast<std::uint8_t>(value >> 16U);
    output[2] = static_cast<std::uint8_t>(value >> 8U);
    output[3] = static_cast<std::uint8_t>(value);
}

std::uint16_t Read16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(input[0]) << 8U) | input[1]);
}

std::uint32_t Read32(const std::uint8_t* input) {
    return (static_cast<std::uint32_t>(input[0]) << 24U)
        | (static_cast<std::uint32_t>(input[1]) << 16U)
        | (static_cast<std::uint32_t>(input[2]) << 8U)
        | static_cast<std::uint32_t>(input[3]);
}

bool HasPrefix(const std::span<const std::uint8_t> packet, const DatagramType type) {
    return packet.size() >= 4 && packet[0] == kMagic0 && packet[1] == kMagic1
        && packet[2] == kVersion && packet[3] == static_cast<std::uint8_t>(type);
}

int HexValue(const char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

}  // namespace

bool ParseServerAddress(const std::string& value, ServerAddress& result, std::string& error) {
    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last = value.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
        error = "Sunucu adresi gerekli";
        return false;
    }
    const std::string trimmed = value.substr(first, last - first + 1);
    if (trimmed.find('[') != std::string::npos || trimmed.find(']') != std::string::npos) {
        error = "Bu surumde IPv6 adresi desteklenmiyor";
        return false;
    }

    result = {};
    result.port = kDefaultPort;
    const auto colon = trimmed.rfind(':');
    if (colon == std::string::npos) {
        result.host = trimmed;
    } else {
        if (trimmed.find(':') != colon) {
            error = "Gecersiz sunucu adresi";
            return false;
        }
        result.host = trimmed.substr(0, colon);
        const std::string portText = trimmed.substr(colon + 1);
        unsigned int parsedPort = 0;
        const auto [end, ec] = std::from_chars(portText.data(), portText.data() + portText.size(), parsedPort);
        if (ec != std::errc{} || end != portText.data() + portText.size() || parsedPort < 1 || parsedPort > 65535) {
            error = "Port 1-65535 arasinda olmali";
            return false;
        }
        result.port = static_cast<std::uint16_t>(parsedPort);
    }
    if (result.host.empty() || result.host.size() > 253) {
        error = "Gecersiz sunucu adresi";
        return false;
    }
    return true;
}

bool DecodeHexToken(const std::string& hex, std::array<std::uint8_t, kTokenBytes>& token) {
    if (hex.size() != kTokenBytes * 2) return false;
    for (std::size_t index = 0; index < token.size(); ++index) {
        const int high = HexValue(hex[index * 2]);
        const int low = HexValue(hex[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        token[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

std::size_t BuildRegister(const std::span<std::uint8_t> output,
                          const std::array<std::uint8_t, kTokenBytes>& token) {
    if (output.size() < kRegisterBytes) return 0;
    output[0] = kMagic0;
    output[1] = kMagic1;
    output[2] = kVersion;
    output[3] = static_cast<std::uint8_t>(DatagramType::Register);
    std::copy(token.begin(), token.end(), output.begin() + 4);
    return kRegisterBytes;
}

std::size_t BuildKeepAlive(const std::span<std::uint8_t> output,
                           const std::array<std::uint8_t, kTokenBytes>& token) {
    const auto size = BuildRegister(output, token);
    if (size != 0) output[3] = static_cast<std::uint8_t>(DatagramType::KeepAliveUp);
    return size;
}

std::size_t BuildAudioUp(const std::span<std::uint8_t> output,
                         const std::array<std::uint8_t, kTokenBytes>& token,
                         const std::uint16_t sequence,
                         const std::uint32_t timestamp,
                         const std::uint8_t flags,
                         const std::span<const std::uint8_t> opus) {
    if (opus.empty() || opus.size() > kMaxOpusBytes || output.size() < kAudioUpHeaderBytes + opus.size()) return 0;
    output[0] = kMagic0;
    output[1] = kMagic1;
    output[2] = kVersion;
    output[3] = static_cast<std::uint8_t>(DatagramType::AudioUp);
    std::copy(token.begin(), token.end(), output.begin() + 4);
    Write16(output.data() + 20, sequence);
    Write32(output.data() + 22, timestamp);
    output[26] = flags;
    std::copy(opus.begin(), opus.end(), output.begin() + kAudioUpHeaderBytes);
    return kAudioUpHeaderBytes + opus.size();
}

bool ParseRegisterAck(const std::span<const std::uint8_t> packet, std::uint32_t& peerId) {
    if (packet.size() != 8 || !HasPrefix(packet, DatagramType::RegisterAck)) return false;
    peerId = Read32(packet.data() + 4);
    return peerId != 0;
}

bool ParseKeepAliveAck(const std::span<const std::uint8_t> packet) {
    return packet.size() == 4 && HasPrefix(packet, DatagramType::KeepAliveAck);
}

bool ParseAudioDown(const std::span<const std::uint8_t> packet, DownlinkAudioPacket& result) {
    if (packet.size() <= kAudioDownHeaderBytes || packet.size() > kMaxDatagramBytes
        || !HasPrefix(packet, DatagramType::AudioDown)) {
        return false;
    }
    const std::size_t opusLength = packet.size() - kAudioDownHeaderBytes;
    if (opusLength > kMaxOpusBytes) return false;
    result.peerId = Read32(packet.data() + 4);
    result.sequence = Read16(packet.data() + 8);
    result.timestamp = Read32(packet.data() + 10);
    result.flags = packet[14];
    result.opus = packet.subspan(kAudioDownHeaderBytes);
    return result.peerId != 0;
}

std::vector<std::uint8_t> EncodeJsonFrame(const std::string& json) {
    if (json.empty() || json.size() > 16 * 1024) return {};
    std::vector<std::uint8_t> output(json.size() + 4);
    Write32(output.data(), static_cast<std::uint32_t>(json.size()));
    std::copy(json.begin(), json.end(), output.begin() + 4);
    return output;
}

}  // namespace ss::protocol
