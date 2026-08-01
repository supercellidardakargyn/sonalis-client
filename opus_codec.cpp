#include "opus_codec.h"

#include <algorithm>
#include <cmath>

#include <opus.h>

namespace ss {

VoiceEncoder::VoiceEncoder(const int bitrate) {
    int result = OPUS_OK;
    encoder_ = opus_encoder_create(kAudioSampleRate, kChannels, OPUS_APPLICATION_VOIP, &result);
    if (result != OPUS_OK || encoder_ == nullptr) {
        error_ = opus_strerror(result);
        encoder_ = nullptr;
        return;
    }
    SetBitrate(bitrate);
    opus_encoder_ctl(encoder_, OPUS_SET_VBR(1));
    opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(3));
    opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(encoder_, OPUS_SET_PACKET_LOSS_PERC(5));
    opus_encoder_ctl(encoder_, OPUS_SET_DTX(1));
    opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
}

VoiceEncoder::~VoiceEncoder() {
    if (encoder_ != nullptr) opus_encoder_destroy(encoder_);
}

bool VoiceEncoder::IsValid() const noexcept { return encoder_ != nullptr; }
const std::string& VoiceEncoder::Error() const noexcept { return error_; }

bool VoiceEncoder::SetBitrate(const int bitrate) noexcept {
    if (encoder_ == nullptr) return false;
    return opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(std::clamp(bitrate, 12'000, 64'000))) == OPUS_OK;
}

int VoiceEncoder::Encode(const float* samples, std::uint8_t* output, const int capacity) noexcept {
    if (encoder_ == nullptr || samples == nullptr || output == nullptr || capacity <= 0) return OPUS_BAD_ARG;
    return opus_encode_float(encoder_, samples, kOpusFrameSamples, output, capacity);
}

VoiceDecoder::VoiceDecoder() {
    int result = OPUS_OK;
    decoder_ = opus_decoder_create(kAudioSampleRate, kChannels, &result);
    if (result != OPUS_OK) decoder_ = nullptr;
}

VoiceDecoder::~VoiceDecoder() {
    if (decoder_ != nullptr) opus_decoder_destroy(decoder_);
}

bool VoiceDecoder::IsValid() const noexcept { return decoder_ != nullptr; }

int VoiceDecoder::Decode(const std::uint8_t* packet, const int packetBytes, float* output, const bool fec) noexcept {
    if (decoder_ == nullptr || output == nullptr) return OPUS_BAD_ARG;
    return opus_decode_float(decoder_, packet, packetBytes, output, kOpusFrameSamples, fec ? 1 : 0);
}

void VoiceDecoder::Reset() noexcept {
    if (decoder_ != nullptr) opus_decoder_ctl(decoder_, OPUS_RESET_STATE);
}

float ComputeRms(const float* samples, const int count) noexcept {
    if (samples == nullptr || count <= 0) return 0.0F;
    double sum = 0.0;
    for (int index = 0; index < count; ++index) {
        const double value = std::clamp(static_cast<double>(samples[index]), -1.0, 1.0);
        sum += value * value;
    }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(count)));
}

float RmsToDbfs(const float rms) noexcept {
    if (rms <= 0.000001F) return -96.0F;
    return std::clamp(20.0F * std::log10(rms), -96.0F, 0.0F);
}

float RmsToMeter(const float rms) noexcept {
    const float db = RmsToDbfs(rms);
    return std::clamp((db + 60.0F) / 60.0F, 0.0F, 1.0F);
}

float SmoothMeterLevel(const float previous, const float target) noexcept {
    const float safePrevious = std::clamp(previous, 0.0F, 1.0F);
    const float safeTarget = std::clamp(target, 0.0F, 1.0F);
    const float response = safeTarget > safePrevious ? 0.65F : 0.18F;
    return safePrevious + (safeTarget - safePrevious) * response;
}

float AdvanceBlockLimiterGain(const float previousGain, const float blockPeak) noexcept {
    const float safePrevious = std::clamp(previousGain, 0.0F, 1.0F);
    const float safePeak = std::max(0.0F, blockPeak);
    const float target = safePeak > 0.98F ? 0.98F / safePeak : 1.0F;
    return target < safePrevious ? target : std::min(1.0F, safePrevious + 0.025F);
}

}  // namespace ss
