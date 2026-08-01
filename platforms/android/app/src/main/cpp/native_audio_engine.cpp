#include <aaudio/AAudio.h>
#include <jni.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace {

template <std::size_t Capacity>
class FloatRing final {
public:
    std::size_t Write(const float* values, const std::size_t count) noexcept {
        const auto read = read_.load(std::memory_order_acquire);
        const auto write = write_.load(std::memory_order_relaxed);
        const std::size_t free = Capacity - (write - read);
        const std::size_t accepted = std::min(count, free);
        for (std::size_t index = 0; index < accepted; ++index) values_[(write + index) % Capacity] = values[index];
        write_.store(write + accepted, std::memory_order_release);
        return accepted;
    }

    std::size_t Read(float* output, const std::size_t count) noexcept {
        const auto write = write_.load(std::memory_order_acquire);
        const auto read = read_.load(std::memory_order_relaxed);
        const std::size_t available = write - read;
        const std::size_t accepted = std::min(count, available);
        for (std::size_t index = 0; index < accepted; ++index) output[index] = values_[(read + index) % Capacity];
        read_.store(read + accepted, std::memory_order_release);
        return accepted;
    }

    void Reset() noexcept { read_.store(0); write_.store(0); }

private:
    std::array<float, Capacity> values_{};
    std::atomic<std::size_t> read_{};
    std::atomic<std::size_t> write_{};
};

struct AudioHandle final {
    AAudioStream* capture{};
    AAudioStream* render{};
    FloatRing<48'000> captured;
    FloatRing<48'000> playback;
    std::atomic<bool> failed{};
};

aaudio_data_callback_result_t CaptureCallback(AAudioStream*, void* user, void* audioData,
                                               const int32_t frames) noexcept {
    auto* state = static_cast<AudioHandle*>(user);
    state->captured.Write(static_cast<const float*>(audioData), static_cast<std::size_t>(frames));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

aaudio_data_callback_result_t RenderCallback(AAudioStream*, void* user, void* audioData,
                                              const int32_t frames) noexcept {
    auto* state = static_cast<AudioHandle*>(user);
    auto* output = static_cast<float*>(audioData);
    const std::size_t requested = static_cast<std::size_t>(frames);
    const std::size_t read = state->playback.Read(output, requested);
    std::fill(output + read, output + requested, 0.0F);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void ErrorCallback(AAudioStream*, void* user, aaudio_result_t) noexcept {
    static_cast<AudioHandle*>(user)->failed.store(true, std::memory_order_release);
}

bool OpenStream(AudioHandle& state, const bool input, AAudioStream** stream) noexcept {
    AAudioStreamBuilder* builder{};
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || builder == nullptr) return false;
    AAudioStreamBuilder_setDirection(builder, input ? AAUDIO_DIRECTION_INPUT : AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setSampleRate(builder, 48'000);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setFramesPerDataCallback(builder, 960);
    AAudioStreamBuilder_setDataCallback(builder, input ? CaptureCallback : RenderCallback, &state);
    AAudioStreamBuilder_setErrorCallback(builder, ErrorCallback, &state);
    const aaudio_result_t result = AAudioStreamBuilder_openStream(builder, stream);
    AAudioStreamBuilder_delete(builder);
    return result == AAUDIO_OK && *stream != nullptr;
}

void Close(AudioHandle& state) noexcept {
    if (state.capture != nullptr) {
        AAudioStream_requestStop(state.capture);
        AAudioStream_close(state.capture);
        state.capture = nullptr;
    }
    if (state.render != nullptr) {
        AAudioStream_requestStop(state.render);
        AAudioStream_close(state.render);
        state.render = nullptr;
    }
    state.captured.Reset();
    state.playback.Reset();
}

AudioHandle* Handle(const jlong value) noexcept {
    return reinterpret_cast<AudioHandle*>(static_cast<std::uintptr_t>(value));
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_tr_sonalis_core_NativeAudioEngine_nativeCreate(JNIEnv*, jclass) {
    return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(new (std::nothrow) AudioHandle{}));
}

extern "C" JNIEXPORT void JNICALL
Java_tr_sonalis_core_NativeAudioEngine_nativeDestroy(JNIEnv*, jclass, const jlong handle) {
    std::unique_ptr<AudioHandle> state(Handle(handle));
    if (state) Close(*state);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_tr_sonalis_core_NativeAudioEngine_nativeStart(JNIEnv*, jclass, const jlong handle,
                                                    const jboolean captureEnabled) {
    auto* state = Handle(handle);
    if (state == nullptr) return JNI_FALSE;
    Close(*state);
    state->failed.store(false, std::memory_order_release);
    if (captureEnabled == JNI_TRUE && (!OpenStream(*state, true, &state->capture)
        || AAudioStream_requestStart(state->capture) != AAUDIO_OK)) {
        Close(*state);
        return JNI_FALSE;
    }
    if (!OpenStream(*state, false, &state->render)
        || AAudioStream_requestStart(state->render) != AAUDIO_OK) {
        Close(*state);
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_tr_sonalis_core_NativeAudioEngine_nativeStop(JNIEnv*, jclass, const jlong handle) {
    if (auto* state = Handle(handle); state != nullptr) Close(*state);
}

extern "C" JNIEXPORT jint JNICALL
Java_tr_sonalis_core_NativeAudioEngine_nativeReadCapture(JNIEnv* environment, jclass,
                                                          const jlong handle, jfloatArray output) {
    auto* state = Handle(handle);
    if (state == nullptr || output == nullptr) return 0;
    const jsize count = environment->GetArrayLength(output);
    jboolean copied{};
    jfloat* values = environment->GetFloatArrayElements(output, &copied);
    if (values == nullptr) return 0;
    const std::size_t read = state->captured.Read(values, static_cast<std::size_t>(count));
    environment->ReleaseFloatArrayElements(output, values, 0);
    return static_cast<jint>(read);
}

extern "C" JNIEXPORT jint JNICALL
Java_tr_sonalis_core_NativeAudioEngine_nativeWritePlayback(JNIEnv* environment, jclass,
                                                            const jlong handle, jfloatArray input,
                                                            const jint count) {
    auto* state = Handle(handle);
    if (state == nullptr || input == nullptr || count <= 0) return 0;
    const jsize length = environment->GetArrayLength(input);
    const jsize bounded = std::min(length, count);
    jboolean copied{};
    jfloat* values = environment->GetFloatArrayElements(input, &copied);
    if (values == nullptr) return 0;
    const std::size_t written = state->playback.Write(values, static_cast<std::size_t>(bounded));
    environment->ReleaseFloatArrayElements(input, values, JNI_ABORT);
    return static_cast<jint>(written);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_tr_sonalis_core_NativeAudioEngine_nativeHealthy(JNIEnv*, jclass, const jlong handle) {
    auto* state = Handle(handle);
    return state != nullptr && !state->failed.load(std::memory_order_acquire) ? JNI_TRUE : JNI_FALSE;
}
