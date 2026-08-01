#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct OpusDecoder;
struct OpusEncoder;

namespace ss {

inline constexpr int kAudioSampleRate = 48'000;
inline constexpr int kChannels = 1;
inline constexpr int kOpusFrameSamples = 960;
inline constexpr int kOpusPacketCapacity = 400;

class VoiceEncoder final {
public:
    explicit VoiceEncoder(int bitrate = 24'000);
    ~VoiceEncoder();
    VoiceEncoder(const VoiceEncoder&) = delete;
    VoiceEncoder& operator=(const VoiceEncoder&) = delete;

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] const std::string& Error() const noexcept;
    bool SetBitrate(int bitrate) noexcept;
    int Encode(const float* samples, std::uint8_t* output, int capacity) noexcept;

private:
    OpusEncoder* encoder_{};
    std::string error_;
};

class VoiceDecoder final {
public:
    VoiceDecoder();
    ~VoiceDecoder();
    VoiceDecoder(const VoiceDecoder&) = delete;
    VoiceDecoder& operator=(const VoiceDecoder&) = delete;

    [[nodiscard]] bool IsValid() const noexcept;
    int Decode(const std::uint8_t* packet, int packetBytes, float* output, bool fec) noexcept;
    void Reset() noexcept;

private:
    OpusDecoder* decoder_{};
};

float ComputeRms(const float* samples, int count) noexcept;
float RmsToDbfs(float rms) noexcept;
float RmsToMeter(float rms) noexcept;
float SmoothMeterLevel(float previous, float target) noexcept;
float AdvanceBlockLimiterGain(float previousGain, float blockPeak) noexcept;

}  // namespace ss
