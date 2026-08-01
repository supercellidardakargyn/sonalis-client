#include <jni.h>

#include <cstdint>

#include "sonalis/core/c_api.h"

namespace {

sonalis_voice_session* Session(const jlong handle) noexcept {
    return reinterpret_cast<sonalis_voice_session*>(static_cast<std::uintptr_t>(handle));
}

jlong Handle(sonalis_voice_session* session) noexcept {
    return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(session));
}

jint Pack(const sonalis_voice_decision value) noexcept {
    return value.lifecycle | value.route << 2 | value.begin_peer_probe << 4
        | value.cancel_peer_probe << 5 | value.release_media << 6 | value.request_wake_grant << 7;
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL
Java_tr_sonalis_core_NativeCore_nativeAbiVersion(JNIEnv*, jclass) {
    return static_cast<jint>(sonalis_core_abi_version());
}

extern "C" JNIEXPORT jlong JNICALL
Java_tr_sonalis_core_NativeVoiceSession_nativeCreate(JNIEnv*, jclass, const jboolean p2p,
                                                       const jboolean serverDenoise) {
    return Handle(sonalis_voice_session_create({
        static_cast<uint8_t>(p2p), static_cast<uint8_t>(serverDenoise), 10'000, 60'000, 2'000,
    }));
}

extern "C" JNIEXPORT void JNICALL
Java_tr_sonalis_core_NativeVoiceSession_nativeDestroy(JNIEnv*, jclass, const jlong handle) {
    sonalis_voice_session_destroy(Session(handle));
}

extern "C" JNIEXPORT void JNICALL
Java_tr_sonalis_core_NativeVoiceSession_nativeConnected(JNIEnv*, jclass, const jlong handle,
                                                          const jlong monotonicMs) {
    sonalis_voice_session_connected(Session(handle), static_cast<uint64_t>(monotonicMs));
}

extern "C" JNIEXPORT void JNICALL
Java_tr_sonalis_core_NativeVoiceSession_nativeDisconnected(JNIEnv*, jclass, const jlong handle) {
    sonalis_voice_session_disconnected(Session(handle));
}

extern "C" JNIEXPORT jint JNICALL
Java_tr_sonalis_core_NativeVoiceSession_nativeParticipantsChanged(
    JNIEnv*, jclass, const jlong handle, const jint participants, const jlong monotonicMs) {
    return Pack(sonalis_voice_session_participants_changed(
        Session(handle), static_cast<uint32_t>(participants < 0 ? 0 : participants),
        static_cast<uint64_t>(monotonicMs)));
}

extern "C" JNIEXPORT jint JNICALL
Java_tr_sonalis_core_NativeVoiceSession_nativeTick(JNIEnv*, jclass, const jlong handle,
                                                    const jlong monotonicMs) {
    return Pack(sonalis_voice_session_tick(Session(handle), static_cast<uint64_t>(monotonicMs)));
}

extern "C" JNIEXPORT jint JNICALL
Java_tr_sonalis_core_NativeVoiceSession_nativeProbeSucceeded(JNIEnv*, jclass, const jlong handle,
                                                              const jlong monotonicMs) {
    return Pack(sonalis_voice_session_probe_succeeded(Session(handle), static_cast<uint64_t>(monotonicMs)));
}

extern "C" JNIEXPORT jint JNICALL
Java_tr_sonalis_core_NativeVoiceSession_nativeProbeFailed(JNIEnv*, jclass, const jlong handle,
                                                           const jlong monotonicMs) {
    return Pack(sonalis_voice_session_probe_failed(Session(handle), static_cast<uint64_t>(monotonicMs)));
}
