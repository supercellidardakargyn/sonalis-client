#include "sonalis/core/c_api.h"

#include <new>
#include <span>

#include "sonalis/core/voice_codec.h"

struct sonalis_voice_encoder final {
    explicit sonalis_voice_encoder(const std::uint32_t bitrate) noexcept : value(bitrate) {}
    sonalis::core::VoiceEncoder value;
};

struct sonalis_voice_decoder final {
    sonalis::core::VoiceDecoder value;
};

extern "C" {

sonalis_voice_encoder* sonalis_voice_encoder_create(const uint32_t bitrate) {
    auto* encoder = new (std::nothrow) sonalis_voice_encoder(bitrate);
    if (encoder != nullptr && !encoder->value.Valid()) {
        delete encoder;
        return nullptr;
    }
    return encoder;
}

void sonalis_voice_encoder_destroy(sonalis_voice_encoder* encoder) { delete encoder; }

uint8_t sonalis_voice_encoder_set_bitrate(
    sonalis_voice_encoder* encoder, const uint32_t bitrate) {
    return encoder != nullptr && encoder->value.SetBitrate(bitrate) ? 1 : 0;
}

int32_t sonalis_voice_encoder_encode(
    sonalis_voice_encoder* encoder, const float* pcm, uint8_t* output,
    const uint32_t outputCapacity) {
    if (encoder == nullptr || pcm == nullptr || output == nullptr || outputCapacity == 0) return -1;
    return encoder->value.Encode(
        std::span<const float, sonalis::core::VoiceFrameSamples>(pcm, sonalis::core::VoiceFrameSamples),
        std::span<std::uint8_t>(output, outputCapacity));
}

sonalis_voice_decoder* sonalis_voice_decoder_create(void) {
    auto* decoder = new (std::nothrow) sonalis_voice_decoder{};
    if (decoder != nullptr && !decoder->value.Valid()) {
        delete decoder;
        return nullptr;
    }
    return decoder;
}

void sonalis_voice_decoder_destroy(sonalis_voice_decoder* decoder) { delete decoder; }

int32_t sonalis_voice_decoder_decode(
    sonalis_voice_decoder* decoder, const uint8_t* packet, const uint32_t packetSize,
    float* output, const uint8_t fec) {
    if (decoder == nullptr || packet == nullptr || packetSize == 0 || output == nullptr) return -1;
    return decoder->value.Decode(
        std::span<const std::uint8_t>(packet, packetSize),
        std::span<float, sonalis::core::VoiceFrameSamples>(output, sonalis::core::VoiceFrameSamples),
        fec != 0);
}

int32_t sonalis_voice_decoder_conceal(sonalis_voice_decoder* decoder, float* output) {
    if (decoder == nullptr || output == nullptr) return -1;
    return decoder->value.Conceal(
        std::span<float, sonalis::core::VoiceFrameSamples>(output, sonalis::core::VoiceFrameSamples));
}

void sonalis_voice_decoder_reset(sonalis_voice_decoder* decoder) {
    if (decoder != nullptr) decoder->value.Reset();
}

float sonalis_voice_rms(const float* samples, const uint32_t sampleCount) {
    if (samples == nullptr || sampleCount == 0 || sampleCount > 48'000) return 0.0F;
    return sonalis::core::RootMeanSquare(std::span<const float>(samples, sampleCount));
}

}  // extern "C"
