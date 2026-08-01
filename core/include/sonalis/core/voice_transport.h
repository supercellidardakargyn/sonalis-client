#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace sonalis::core {

enum class VoiceRoute : std::uint8_t { Relay, Probing, PeerToPeer };

struct EncodedVoiceFrame final {
    std::uint16_t sequence{};
    std::uint32_t timestamp{};
    std::uint8_t flags{};
    std::span<const std::uint8_t> opus;
};

class VoiceTransport {
public:
    using ReceiveCallback = std::function<void(std::string_view, EncodedVoiceFrame)>;
    virtual ~VoiceTransport() = default;
    virtual bool Connect(std::string joinGrant, bool allowPeerToPeer, std::string& error) = 0;
    virtual bool Send(EncodedVoiceFrame frame) noexcept = 0;
    virtual void Disconnect() noexcept = 0;
    [[nodiscard]] virtual VoiceRoute Route() const noexcept = 0;
    virtual void SetReceiveCallback(ReceiveCallback callback) = 0;
};

}  // namespace sonalis::core
