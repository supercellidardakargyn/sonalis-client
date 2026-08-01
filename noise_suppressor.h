#pragma once

struct DenoiseState;

namespace ss {

class NoiseSuppressor final {
public:
    NoiseSuppressor();
    ~NoiseSuppressor();
    NoiseSuppressor(const NoiseSuppressor&) = delete;
    NoiseSuppressor& operator=(const NoiseSuppressor&) = delete;
    void Process(float* samples, int sampleCount) noexcept;
    [[nodiscard]] bool IsValid() const noexcept;

private:
    DenoiseState* state_{};
};

}  // namespace ss
