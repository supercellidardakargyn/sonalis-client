#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace sonalis::core {

struct AudioFormat final {
    std::uint32_t sampleRate{48'000};
    std::uint16_t channels{1};
    std::uint16_t frameSamples{960};
};

struct AudioDevice final { std::string id; std::string name; bool isDefault{}; };

class AudioBackend {
public:
    using CaptureCallback = std::function<void(std::span<const float>)>;
    virtual ~AudioBackend() = default;
    [[nodiscard]] virtual std::vector<AudioDevice> CaptureDevices() = 0;
    [[nodiscard]] virtual std::vector<AudioDevice> RenderDevices() = 0;
    virtual bool Start(const std::string& captureId, const std::string& renderId,
                       AudioFormat format, CaptureCallback capture, std::string& error) = 0;
    virtual void Render(std::span<const float> monoPcm) noexcept = 0;
    virtual void Stop() noexcept = 0;
};

}  // namespace sonalis::core

