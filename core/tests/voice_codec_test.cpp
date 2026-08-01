#include "sonalis/core/voice_codec.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>

int main() {
    sonalis::core::VoiceEncoder encoder(24'000);
    sonalis::core::VoiceDecoder decoder;
    assert(encoder.Valid());
    assert(decoder.Valid());
    std::array<float, sonalis::core::VoiceFrameSamples> input{};
    for (std::size_t index = 0; index < input.size(); ++index) {
        input[index] = 0.15F * std::sin(2.0 * 3.141592653589793 * 440.0 * static_cast<double>(index) / 48'000.0);
    }
    std::array<std::uint8_t, sonalis::core::MaximumOpusPacketBytes> packet{};
    const int encoded = encoder.Encode(input, packet);
    assert(encoded > 0 && static_cast<std::size_t>(encoded) <= packet.size());
    std::array<float, sonalis::core::VoiceFrameSamples> output{};
    const int decoded = decoder.Decode(std::span<const std::uint8_t>(packet.data(), static_cast<std::size_t>(encoded)), output);
    assert(decoded == static_cast<int>(output.size()));
    assert(sonalis::core::RootMeanSquare(input) > 0.01F);
    assert(sonalis::core::RootMeanSquare(output) > 0.001F);
    assert(encoder.SetBitrate(32'000));
    decoder.Reset();
    assert(decoder.Conceal(output) == static_cast<int>(output.size()));
    return 0;
}
