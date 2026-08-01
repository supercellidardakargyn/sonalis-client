#include "sonalis/core/voice_codec.h"

#include <algorithm>
#include <cmath>

#include <opus.h>

namespace sonalis::core {

VoiceEncoder::VoiceEncoder(const std::uint32_t bitrate) noexcept {
    int error = OPUS_OK;
    encoder_ = opus_encoder_create(VoiceSampleRate, 1, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || encoder_ == nullptr) { encoder_ = nullptr; return; }
    if (!SetBitrate(bitrate)
        || opus_encoder_ctl(encoder_, OPUS_SET_VBR(1)) != OPUS_OK
        || opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(3)) != OPUS_OK
        || opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(1)) != OPUS_OK
        || opus_encoder_ctl(encoder_, OPUS_SET_PACKET_LOSS_PERC(5)) != OPUS_OK
        || opus_encoder_ctl(encoder_, OPUS_SET_DTX(1)) != OPUS_OK
        || opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)) != OPUS_OK) {
        opus_encoder_destroy(encoder_);
        encoder_ = nullptr;
    }
}

VoiceEncoder::~VoiceEncoder() { if (encoder_ != nullptr) opus_encoder_destroy(encoder_); }

bool VoiceEncoder::SetBitrate(const std::uint32_t bitrate) noexcept {
    return encoder_ != nullptr && opus_encoder_ctl(
        encoder_, OPUS_SET_BITRATE(static_cast<int>(std::clamp<std::uint32_t>(bitrate, 12'000, 64'000)))) == OPUS_OK;
}

int VoiceEncoder::Encode(const std::span<const float, VoiceFrameSamples> pcm,
                         const std::span<std::uint8_t> output) noexcept {
    if (encoder_ == nullptr || output.empty()) return OPUS_BAD_ARG;
    const auto capacity = static_cast<opus_int32>(std::min<std::size_t>(output.size(), MaximumOpusPacketBytes));
    return opus_encode_float(encoder_, pcm.data(), static_cast<int>(VoiceFrameSamples), output.data(), capacity);
}

VoiceDecoder::VoiceDecoder() noexcept {
    int error = OPUS_OK;
    decoder_ = opus_decoder_create(VoiceSampleRate, 1, &error);
    if (error != OPUS_OK) decoder_ = nullptr;
}

VoiceDecoder::~VoiceDecoder() { if (decoder_ != nullptr) opus_decoder_destroy(decoder_); }

int VoiceDecoder::Decode(const std::span<const std::uint8_t> packet,
                         const std::span<float, VoiceFrameSamples> output, const bool fec) noexcept {
    if (decoder_ == nullptr || packet.empty() || packet.size() > MaximumOpusPacketBytes) return OPUS_BAD_ARG;
    return opus_decode_float(decoder_, packet.data(), static_cast<opus_int32>(packet.size()), output.data(),
                             static_cast<int>(VoiceFrameSamples), fec ? 1 : 0);
}

int VoiceDecoder::Conceal(const std::span<float, VoiceFrameSamples> output) noexcept {
    if (decoder_ == nullptr) return OPUS_BAD_ARG;
    return opus_decode_float(decoder_, nullptr, 0, output.data(), static_cast<int>(VoiceFrameSamples), 0);
}

void VoiceDecoder::Reset() noexcept { if (decoder_ != nullptr) (void)opus_decoder_ctl(decoder_, OPUS_RESET_STATE); }

float RootMeanSquare(const std::span<const float> samples) noexcept {
    if (samples.empty()) return 0.0F;
    double sum = 0.0;
    for (const float sample : samples) {
        const double value = std::clamp(static_cast<double>(sample), -1.0, 1.0);
        sum += value * value;
    }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
}

}  // namespace sonalis::core
