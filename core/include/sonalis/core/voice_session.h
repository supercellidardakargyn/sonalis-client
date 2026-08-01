#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "voice_transport.h"

namespace sonalis::core {

enum class VoiceLifecycle : std::uint8_t { Disconnected, Active, Sleeping };

struct VoicePolicy final {
    bool peerToPeerEnabled{true};
    bool serverDenoiseEnabled{};
    std::chrono::seconds peerStability{10};
    std::chrono::seconds soloSleepDelay{60};
    std::chrono::seconds probeTimeout{2};
};

struct VoiceDecision final {
    VoiceLifecycle lifecycle{VoiceLifecycle::Disconnected};
    VoiceRoute route{VoiceRoute::Relay};
    bool beginPeerProbe{};
    bool cancelPeerProbe{};
    bool releaseMedia{};
    bool requestWakeGrant{};
};

// Platform-independent voice lifecycle. It does not own sockets, clocks or
// audio devices; callers feed monotonic timestamps and execute the returned
// one-shot decisions on their platform worker.
class VoiceSession final {
public:
    using Clock = std::chrono::steady_clock;

    explicit VoiceSession(VoicePolicy policy = {}) noexcept;
    void Connected(Clock::time_point now) noexcept;
    void Disconnected() noexcept;
    [[nodiscard]] VoiceDecision ParticipantsChanged(std::size_t activeParticipants,
                                                     Clock::time_point now) noexcept;
    [[nodiscard]] VoiceDecision Tick(Clock::time_point now) noexcept;
    [[nodiscard]] VoiceDecision PeerProbeSucceeded(Clock::time_point now) noexcept;
    [[nodiscard]] VoiceDecision PeerProbeFailed(Clock::time_point now) noexcept;
    void SetPolicy(VoicePolicy policy, Clock::time_point now) noexcept;
    [[nodiscard]] VoiceLifecycle Lifecycle() const noexcept { return lifecycle_; }
    [[nodiscard]] VoiceRoute Route() const noexcept { return route_; }

private:
    [[nodiscard]] VoiceDecision Snapshot() const noexcept;
    void ResetPeerCandidate(Clock::time_point now) noexcept;

    VoicePolicy policy_;
    VoiceLifecycle lifecycle_{VoiceLifecycle::Disconnected};
    VoiceRoute route_{VoiceRoute::Relay};
    std::size_t participants_{};
    Clock::time_point occupancyChangedAt_{};
    Clock::time_point probeStartedAt_{};
    bool peerProbeIssued_{};
};

}  // namespace sonalis::core
