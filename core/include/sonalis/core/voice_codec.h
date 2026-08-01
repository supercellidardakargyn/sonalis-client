#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

struct OpusDecoder;
struct OpusEncoder;

namespace sonalis::core {

inline constexpr std::uint32_t VoiceSampleRate = 48'000;
inline constexpr std::size_t VoiceFrameSamples = 960;
inline constexpr std::size_t MaximumOpusPacketBytes = 1'275;

class VoiceEncoder final {
public:
    explicit VoiceEncoder(std::uint32_t bitrate = 24'000) noexcept;
    ~VoiceEncoder();
    VoiceEncoder(const VoiceEncoder&) = delete;
    VoiceEncoder& operator=(const VoiceEncoder&) = delete;

    [[nodiscard]] bool Valid() const noexcept { return encoder_ != nullptr; }
    bool SetBitrate(std::uint32_t bitrate) noexcept;
    [[nodiscard]] int Encode(std::span<const float, VoiceFrameSamples> pcm,
                             std::span<std::uint8_t> output) noexcept;

private:
    OpusEncoder* encoder_{};
};

class VoiceDecoder final {
public:
    VoiceDecoder() noexcept;
    ~VoiceDecoder();
    VoiceDecoder(const VoiceDecoder&) = delete;
    VoiceDecoder& operator=(const VoiceDecoder&) = delete;

    [[nodiscard]] bool Valid() const noexcept { return decoder_ != nullptr; }
    [[nodiscard]] int Decode(std::span<const std::uint8_t> packet,
                             std::span<float, VoiceFrameSamples> output,
                             bool fec = false) noexcept;
    [[nodiscard]] int Conceal(std::span<float, VoiceFrameSamples> output) noexcept;
    void Reset() noexcept;

private:
    OpusDecoder* decoder_{};
};

[[nodiscard]] float RootMeanSquare(std::span<const float> samples) noexcept;

}  // namespace sonalis::core
