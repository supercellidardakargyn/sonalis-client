#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>

#include <juice/juice.h>

namespace ss {

// A small, bounded libjuice wrapper. TURN is intentionally not configured:
// Voice1 remains the media fallback when direct ICE connectivity is unavailable.
class IceTransport final {
public:
    using ReceiveCallback = std::function<void(std::span<const std::uint8_t>)>;

    IceTransport() = default;
    ~IceTransport();
    IceTransport(const IceTransport&) = delete;
    IceTransport& operator=(const IceTransport&) = delete;

    bool Start(const std::string& stunHost, std::uint16_t stunPort,
               ReceiveCallback receive, std::string& error);
    bool SetRemoteDescription(const std::string& description, std::string& error);
    bool Send(std::span<const std::uint8_t> datagram) noexcept;
    void Stop() noexcept;

    [[nodiscard]] bool IsStarted() const noexcept;
    [[nodiscard]] bool IsConnected() const noexcept;
    [[nodiscard]] std::string LocalDescription() const;

private:
    static void StateChanged(juice_agent_t*, juice_state_t state, void* user) noexcept;
    static void GatheringDone(juice_agent_t*, void* user) noexcept;
    static void Received(juice_agent_t*, const char* data, std::size_t size, void* user) noexcept;

    mutable std::mutex mutex_;
    std::condition_variable stateChanged_;
    juice_agent_t* agent_{};
    ReceiveCallback receive_;
    std::string stunHost_;
    std::string localDescription_;
    std::atomic<bool> gatheringDone_{false};
    std::atomic<bool> connected_{false};
};

}  // namespace ss
