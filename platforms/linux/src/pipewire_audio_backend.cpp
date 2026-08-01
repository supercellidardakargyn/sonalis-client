#include "sonalis/linux/pipewire_audio_backend.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>

#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

namespace sonalis::linux_platform {

PipeWireAudioBackend::PipeWireAudioBackend() { pw_init(nullptr, nullptr); }

PipeWireAudioBackend::~PipeWireAudioBackend() {
    Stop();
    pw_deinit();
}

std::vector<core::AudioDevice> PipeWireAudioBackend::CaptureDevices() {
    return {{"default", "PipeWire varsayılan mikrofon", true}};
}

std::vector<core::AudioDevice> PipeWireAudioBackend::RenderDevices() {
    return {{"default", "PipeWire varsayılan çıkış", true}};
}

bool PipeWireAudioBackend::Start(const std::string& captureId, const std::string& renderId,
                                 const core::AudioFormat format, CaptureCallback capture,
                                 std::string& error) {
    Stop();
    if ((captureId != "default" && !captureId.empty()) || (renderId != "default" && !renderId.empty())
        || format.sampleRate != 48'000 || format.channels != 1 || format.frameSamples != 960) {
        error = "unsupported_audio_format_or_device";
        return false;
    }
    loop_ = pw_main_loop_new(nullptr);
    if (loop_ == nullptr) { error = "pipewire_loop_failed"; return false; }
    capture_ = std::move(capture);
    captureEvents_ = {};
    captureEvents_.version = PW_VERSION_STREAM_EVENTS;
    captureEvents_.process = CaptureProcess;
    renderEvents_ = {};
    renderEvents_.version = PW_VERSION_STREAM_EVENTS;
    renderEvents_.process = RenderProcess;
    if (!captureId.empty() && !ConnectStream(captureStream_, PW_DIRECTION_INPUT, &captureEvents_, error)) {
        Stop();
        return false;
    }
    if (!ConnectStream(renderStream_, PW_DIRECTION_OUTPUT, &renderEvents_, error)) {
        Stop();
        return false;
    }
    started_.store(true, std::memory_order_release);
    loopThread_ = std::jthread([this](std::stop_token) { pw_main_loop_run(loop_); });
    return true;
}

bool PipeWireAudioBackend::ConnectStream(pw_stream*& stream, const pw_direction direction,
                                         const pw_stream_events* events, std::string& error) {
    pw_properties* properties = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, direction == PW_DIRECTION_INPUT ? "Capture" : "Playback",
        PW_KEY_MEDIA_ROLE, "Communication",
        PW_KEY_NODE_NAME, direction == PW_DIRECTION_INPUT ? "Sonalis Capture" : "Sonalis Playback",
        nullptr);
    stream = pw_stream_new_simple(pw_main_loop_get_loop(loop_), "Sonalis", properties, events, this);
    if (stream == nullptr) { error = "pipewire_stream_failed"; return false; }
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = 48'000;
    info.channels = 1;
    info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    std::array<std::uint8_t, 1'024> podBuffer{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer.data(), podBuffer.size());
    const spa_pod* parameters[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS
                                                    | PW_STREAM_FLAG_RT_PROCESS);
    if (pw_stream_connect(stream, direction, PW_ID_ANY, flags, parameters, 1) < 0) {
        error = "pipewire_connect_failed";
        return false;
    }
    return true;
}

void PipeWireAudioBackend::CaptureProcess(void* data) noexcept {
    auto* self = static_cast<PipeWireAudioBackend*>(data);
    pw_buffer* wrapper = pw_stream_dequeue_buffer(self->captureStream_);
    if (wrapper == nullptr) return;
    spa_buffer* buffer = wrapper->buffer;
    if (buffer->n_datas > 0 && buffer->datas[0].data != nullptr && buffer->datas[0].chunk != nullptr) {
        const spa_chunk* chunk = buffer->datas[0].chunk;
        if (chunk->offset > buffer->datas[0].maxsize || chunk->size > buffer->datas[0].maxsize - chunk->offset) {
            pw_stream_queue_buffer(self->captureStream_, wrapper);
            return;
        }
        const auto* samples = static_cast<const float*>(buffer->datas[0].data) + chunk->offset / sizeof(float);
        const std::size_t count = std::min<std::size_t>(chunk->size / sizeof(float), 960);
        if (count > 0 && self->capture_) self->capture_(std::span<const float>(samples, count));
    }
    pw_stream_queue_buffer(self->captureStream_, wrapper);
}

void PipeWireAudioBackend::RenderProcess(void* data) noexcept {
    auto* self = static_cast<PipeWireAudioBackend*>(data);
    pw_buffer* wrapper = pw_stream_dequeue_buffer(self->renderStream_);
    if (wrapper == nullptr) return;
    spa_buffer* buffer = wrapper->buffer;
    if (buffer->n_datas > 0 && buffer->datas[0].data != nullptr && buffer->datas[0].chunk != nullptr) {
        auto* output = static_cast<float*>(buffer->datas[0].data);
        const std::size_t maximum = buffer->datas[0].maxsize / sizeof(float);
        const std::size_t capacity = std::min<std::size_t>(
            wrapper->requested > 0 ? wrapper->requested : 960, maximum);
        const auto frame = self->playback_.TryPop();
        const std::size_t count = frame ? std::min(frame->count, capacity) : 0;
        if (frame && count > 0) std::copy_n(frame->samples.data(), count, output);
        std::fill(output + count, output + capacity, 0.0F);
        buffer->datas[0].chunk->offset = 0;
        buffer->datas[0].chunk->stride = sizeof(float);
        buffer->datas[0].chunk->size = static_cast<std::uint32_t>(capacity * sizeof(float));
    }
    pw_stream_queue_buffer(self->renderStream_, wrapper);
}

void PipeWireAudioBackend::Render(const std::span<const float> monoPcm) noexcept {
    if (!started_.load(std::memory_order_acquire) || monoPcm.empty()) return;
    Frame frame;
    frame.count = std::min(monoPcm.size(), frame.samples.size());
    std::copy_n(monoPcm.data(), frame.count, frame.samples.data());
    (void)playback_.TryPush(std::move(frame));
}

void PipeWireAudioBackend::Stop() noexcept {
    started_.store(false, std::memory_order_release);
    if (loop_ != nullptr) pw_main_loop_quit(loop_);
    if (loopThread_.joinable()) loopThread_.join();
    if (captureStream_ != nullptr) { pw_stream_destroy(captureStream_); captureStream_ = nullptr; }
    if (renderStream_ != nullptr) { pw_stream_destroy(renderStream_); renderStream_ = nullptr; }
    if (loop_ != nullptr) { pw_main_loop_destroy(loop_); loop_ = nullptr; }
    capture_ = {};
}

}  // namespace sonalis::linux_platform
