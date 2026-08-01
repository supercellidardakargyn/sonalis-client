#include <jni.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>

#include "sonalis/core/voice_codec.h"

namespace {

struct Codec final {
    explicit Codec(const std::uint32_t bitrate) : encoder(bitrate) {}
    sonalis::core::VoiceEncoder encoder;
    sonalis::core::VoiceDecoder decoder;
};

Codec* Handle(const jlong value) noexcept {
    return reinterpret_cast<Codec*>(static_cast<std::uintptr_t>(value));
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_tr_sonalis_core_NativeVoiceCodec_nativeCreate(JNIEnv*, jclass, const jint bitrate) {
    auto codec = std::unique_ptr<Codec>(new (std::nothrow) Codec(
        static_cast<std::uint32_t>(std::clamp(bitrate, 12'000, 64'000))));
    if (!codec || !codec->encoder.Valid() || !codec->decoder.Valid()) return 0;
    return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(codec.release()));
}

extern "C" JNIEXPORT void JNICALL
Java_tr_sonalis_core_NativeVoiceCodec_nativeDestroy(JNIEnv*, jclass, const jlong handle) {
    delete Handle(handle);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_tr_sonalis_core_NativeVoiceCodec_nativeSetBitrate(JNIEnv*, jclass, const jlong handle,
                                                        const jint bitrate) {
    auto* codec = Handle(handle);
    return codec != nullptr && codec->encoder.SetBitrate(static_cast<std::uint32_t>(bitrate)) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_tr_sonalis_core_NativeVoiceCodec_nativeEncode(JNIEnv* environment, jclass, const jlong handle,
                                                    jfloatArray input, jbyteArray output) {
    auto* codec = Handle(handle);
    if (codec == nullptr || input == nullptr || output == nullptr
        || environment->GetArrayLength(input) < static_cast<jsize>(sonalis::core::VoiceFrameSamples)) return -1;
    std::array<float, sonalis::core::VoiceFrameSamples> pcm{};
    environment->GetFloatArrayRegion(input, 0, static_cast<jsize>(pcm.size()), pcm.data());
    const jsize outputSize = std::min<jsize>(environment->GetArrayLength(output),
        static_cast<jsize>(sonalis::core::MaximumOpusPacketBytes));
    if (outputSize <= 0) return -1;
    std::array<std::uint8_t, sonalis::core::MaximumOpusPacketBytes> packet{};
    const int encoded = codec->encoder.Encode(pcm,
        std::span<std::uint8_t>(packet.data(), static_cast<std::size_t>(outputSize)));
    if (encoded > 0) environment->SetByteArrayRegion(output, 0, encoded,
        reinterpret_cast<const jbyte*>(packet.data()));
    return encoded;
}

extern "C" JNIEXPORT jint JNICALL
Java_tr_sonalis_core_NativeVoiceCodec_nativeDecode(JNIEnv* environment, jclass, const jlong handle,
                                                    jbyteArray input, const jint bytes,
                                                    jfloatArray output, const jboolean fec) {
    auto* codec = Handle(handle);
    if (codec == nullptr || output == nullptr
        || environment->GetArrayLength(output) < static_cast<jsize>(sonalis::core::VoiceFrameSamples)) return -1;
    std::array<float, sonalis::core::VoiceFrameSamples> pcm{};
    int decoded = -1;
    if (input == nullptr || bytes <= 0) {
        decoded = codec->decoder.Conceal(pcm);
    } else {
        const jsize bounded = std::min<jsize>({environment->GetArrayLength(input), bytes,
            static_cast<jsize>(sonalis::core::MaximumOpusPacketBytes)});
        std::array<std::uint8_t, sonalis::core::MaximumOpusPacketBytes> packet{};
        environment->GetByteArrayRegion(input, 0, bounded, reinterpret_cast<jbyte*>(packet.data()));
        decoded = codec->decoder.Decode(std::span<const std::uint8_t>(packet.data(), static_cast<std::size_t>(bounded)),
                                        pcm, fec == JNI_TRUE);
    }
    if (decoded > 0) environment->SetFloatArrayRegion(output, 0, decoded, pcm.data());
    return decoded;
}
