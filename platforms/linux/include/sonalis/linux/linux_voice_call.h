#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "sonalis/core/bounded_spsc_queue.h"
#include "sonalis/core/voice_codec.h"
#include "sonalis/linux/linux_api.h"
#include "sonalis/linux/linux_voice_transport.h"
#include "sonalis/linux/pipewire_audio_backend.h"

namespace sonalis::linux_platform {

class LinuxVoiceCall final {
public:
    using StateCallback = std::function<void(std::string)>;

    explicit LinuxVoiceCall(StateCallback state);
    ~LinuxVoiceCall();
    bool Connect(const LinuxVoiceGrant& grant, std::string& error);
    void Close() noexcept;

private:
    struct CaptureFrame final { std::array<float, core::VoiceFrameSamples> samples{}; };
    void Capture(std::span<const float> samples) noexcept;
    void Worker(std::stop_token stopToken) noexcept;

    StateCallback state_;
    PipeWireAudioBackend audio_;
    std::unique_ptr<LinuxVoiceTransport> transport_;
    std::unique_ptr<core::VoiceEncoder> encoder_;
    std::unordered_map<std::string, std::unique_ptr<core::VoiceDecoder>> decoders_;
    core::BoundedSpscQueue<CaptureFrame, 32> captured_;
    core::BoundedSpscQueue<LinuxVoiceFrame, 128> received_;
    std::array<float, core::VoiceFrameSamples> partial_{};
    std::size_t partialCount_{};
    std::atomic<std::uint16_t> sequence_{};
    std::atomic<std::uint32_t> timestamp_{};
    std::atomic<bool> running_{};
    std::mutex wakeMutex_;
    std::condition_variable_any wake_;
    std::jthread worker_;
};

}  // namespace sonalis::linux_platform
