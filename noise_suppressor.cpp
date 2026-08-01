#include "noise_suppressor.h"

#include <algorithm>
#include <array>

#include <rnnoise.h>

namespace ss {

NoiseSuppressor::NoiseSuppressor() : state_(rnnoise_create(nullptr)) {}
NoiseSuppressor::~NoiseSuppressor() { if (state_ != nullptr) rnnoise_destroy(state_); }
bool NoiseSuppressor::IsValid() const noexcept { return state_ != nullptr; }

void NoiseSuppressor::Process(float* samples, const int sampleCount) noexcept {
    if (state_ == nullptr || samples == nullptr || sampleCount <= 0) return;
    std::array<float, 480> frame{};
    for (int offset = 0; offset < sampleCount; offset += static_cast<int>(frame.size())) {
        const int count = std::min(static_cast<int>(frame.size()), sampleCount - offset);
        std::fill(frame.begin(), frame.end(), 0.0F);
        for (int index = 0; index < count; ++index) frame[static_cast<std::size_t>(index)] = samples[offset + index] * 32768.0F;
        rnnoise_process_frame(state_, frame.data(), frame.data());
        for (int index = 0; index < count; ++index) samples[offset + index] = std::clamp(frame[static_cast<std::size_t>(index)] / 32768.0F, -1.0F, 1.0F);
    }
}

}  // namespace ss
