#include "sonalis/linux/linux_voice_call.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <span>
#include <utility>

namespace sonalis::linux_platform {

LinuxVoiceCall::LinuxVoiceCall(StateCallback state) : state_(std::move(state)) {}

LinuxVoiceCall::~LinuxVoiceCall() { Close(); }

bool LinuxVoiceCall::Connect(const LinuxVoiceGrant& grant, std::string& error) {
    Close();
    encoder_ = std::make_unique<core::VoiceEncoder>(grant.bitrate);
    if (!encoder_->Valid()) { error = "opus_encoder_failed"; encoder_.reset(); return false; }
    transport_ = std::make_unique<LinuxVoiceTransport>(
        [this](LinuxVoiceFrame frame) {
            if (running_.load(std::memory_order_acquire) && received_.TryPush(std::move(frame))) wake_.notify_one();
        },
        [this](std::string status) { if (state_) state_(std::move(status)); });
    if (!transport_->Connect(grant, error)) { transport_.reset(); encoder_.reset(); return false; }
    running_.store(true, std::memory_order_release);
    worker_ = std::jthread([this](const std::stop_token token) { Worker(token); });
    const std::string captureId = grant.canSpeak ? "default" : "";
    if (!audio_.Start(captureId, "default", {48'000, 1, core::VoiceFrameSamples},
                      [this](const std::span<const float> samples) { Capture(samples); }, error)) {
        Close();
        return false;
    }
    return true;
}

void LinuxVoiceCall::Capture(const std::span<const float> samples) noexcept {
    if (!running_.load(std::memory_order_acquire)) return;
    std::size_t offset = 0;
    while (offset < samples.size()) {
        const std::size_t copied = std::min(samples.size() - offset, partial_.size() - partialCount_);
        std::copy_n(samples.data() + offset, copied, partial_.data() + partialCount_);
        partialCount_ += copied;
        offset += copied;
        if (partialCount_ == partial_.size()) {
            CaptureFrame frame;
            frame.samples = partial_;
            (void)captured_.TryPush(std::move(frame));
            partialCount_ = 0;
            wake_.notify_one();
        }
    }
}

void LinuxVoiceCall::Worker(const std::stop_token stopToken) noexcept {
    std::array<std::uint8_t, core::MaximumOpusPacketBytes> packet{};
    std::array<float, core::VoiceFrameSamples> decoded{};
    std::array<float, core::VoiceFrameSamples> mixed{};
    auto nextMix = std::chrono::steady_clock::now();
    while (!stopToken.stop_requested() && running_.load(std::memory_order_acquire)) {
        while (const auto captured = captured_.TryPop()) {
            const int bytes = encoder_->Encode(captured->samples, packet);
            if (bytes > 0) {
                const auto sequence = static_cast<std::uint16_t>(sequence_.fetch_add(1) + 1);
                const auto timestamp = timestamp_.fetch_add(core::VoiceFrameSamples);
                (void)transport_->SendAudio(sequence, timestamp, 0,
                    std::span<const std::uint8_t>(packet.data(), static_cast<std::size_t>(bytes)));
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextMix) {
            mixed.fill(0.0F);
            std::size_t talkers = 0;
            while (const auto incoming = received_.TryPop()) {
                auto& decoder = decoders_[incoming->senderId];
                if (!decoder) decoder = std::make_unique<core::VoiceDecoder>();
                const int samples = decoder->Decode(incoming->opus, decoded, false);
                if (samples != static_cast<int>(decoded.size())) continue;
                for (std::size_t index = 0; index < mixed.size(); ++index) mixed[index] += decoded[index];
                ++talkers;
            }
            if (talkers > 0) {
                const float gain = 1.0F / std::sqrt(static_cast<float>(talkers));
                for (float& sample : mixed) sample = std::clamp(sample * gain, -1.0F, 1.0F);
                audio_.Render(mixed);
            }
            nextMix = now + std::chrono::milliseconds(20);
        }
        std::unique_lock lock(wakeMutex_);
        wake_.wait_until(lock, std::min(nextMix, std::chrono::steady_clock::now()
            + std::chrono::milliseconds(20)));
    }
}

void LinuxVoiceCall::Close() noexcept {
    running_.store(false, std::memory_order_release);
    wake_.notify_all();
    audio_.Stop();
    if (transport_) transport_->Close();
    if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
    transport_.reset();
    encoder_.reset();
    decoders_.clear();
    while (captured_.TryPop()) {}
    while (received_.TryPop()) {}
    partial_.fill(0.0F);
    partialCount_ = 0;
    sequence_.store(0);
    timestamp_.store(0);
}

}  // namespace sonalis::linux_platform
