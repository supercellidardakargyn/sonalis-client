#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <thread>

#include <pipewire/pipewire.h>

#include "sonalis/core/audio_backend.h"
#include "sonalis/core/bounded_spsc_queue.h"

namespace sonalis::linux_platform {

class PipeWireAudioBackend final : public core::AudioBackend {
public:
    PipeWireAudioBackend();
    ~PipeWireAudioBackend() override;

    [[nodiscard]] std::vector<core::AudioDevice> CaptureDevices() override;
    [[nodiscard]] std::vector<core::AudioDevice> RenderDevices() override;
    bool Start(const std::string& captureId, const std::string& renderId,
               core::AudioFormat format, CaptureCallback capture, std::string& error) override;
    void Render(std::span<const float> monoPcm) noexcept override;
    void Stop() noexcept override;

private:
    struct Frame final { std::array<float, 960> samples{}; std::size_t count{}; };
    static void CaptureProcess(void* data) noexcept;
    static void RenderProcess(void* data) noexcept;
    bool ConnectStream(pw_stream*& stream, pw_direction direction,
                       const pw_stream_events* events, std::string& error);

    pw_main_loop* loop_{};
    pw_stream* captureStream_{};
    pw_stream* renderStream_{};
    pw_stream_events captureEvents_{};
    pw_stream_events renderEvents_{};
    std::jthread loopThread_;
    CaptureCallback capture_;
    core::BoundedSpscQueue<Frame, 16> playback_;
    std::atomic<bool> started_{};
};

}  // namespace sonalis::linux_platform
