#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "sonalis/linux/linux_api.h"

namespace sonalis::linux_platform {

struct LinuxVoiceFrame final {
    std::string senderId;
    std::uint16_t sequence{};
    std::uint32_t timestamp{};
    std::uint8_t flags{};
    std::vector<std::uint8_t> opus;
};

class LinuxVoiceTransport final {
public:
    using FrameCallback = std::function<void(LinuxVoiceFrame)>;
    using StateCallback = std::function<void(std::string)>;

    LinuxVoiceTransport(FrameCallback frame, StateCallback state);
    ~LinuxVoiceTransport();
    LinuxVoiceTransport(const LinuxVoiceTransport&) = delete;
    LinuxVoiceTransport& operator=(const LinuxVoiceTransport&) = delete;

    bool Connect(const LinuxVoiceGrant& grant, std::string& error);
    bool SendAudio(std::uint16_t sequence, std::uint32_t timestamp, std::uint8_t flags,
                   std::span<const std::uint8_t> opus) noexcept;
    void Close() noexcept;

private:
    bool JoinPinned(const LinuxVoiceGrant& grant, std::string& error);
    bool SendSecure(std::span<const std::uint8_t> plaintext, std::uint8_t flags) noexcept;
    void ReceiveLoop(std::stop_token stopToken) noexcept;
    bool Replayed(std::uint64_t sequence) noexcept;
    void AcceptSequence(std::uint64_t sequence) noexcept;

    FrameCallback frame_;
    StateCallback state_;
    int socket_{-1};
    std::array<std::uint8_t, 16> sessionId_{};
    std::array<std::uint8_t, 32> key_{};
    std::array<std::uint8_t, 4> noncePrefix_{};
    std::atomic<std::uint64_t> outgoing_{};
    std::uint64_t highestIncoming_{};
    std::uint64_t replayMask_{};
    bool receivedAny_{};
    std::atomic<bool> bound_{};
    std::atomic<bool> running_{};
    std::mutex sendMutex_;
    std::mutex replayMutex_;
    std::jthread receiveThread_;
};

}  // namespace sonalis::linux_platform
