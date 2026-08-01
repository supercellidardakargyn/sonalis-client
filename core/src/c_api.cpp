#include "sonalis/core/c_api.h"

#include <chrono>
#include <new>

#include "sonalis/core/event_generation.h"
#include "sonalis/core/portable_crypto.h"
#include "sonalis/core/voice_session.h"

struct sonalis_voice_session final {
    explicit sonalis_voice_session(sonalis::core::VoicePolicy policy) noexcept : value(policy) {}
    sonalis::core::VoiceSession value;
};

struct sonalis_event_generation final {
    sonalis::core::EventGeneration value;
};

namespace {

sonalis::core::VoiceSession::Clock::time_point TimePoint(const uint64_t monotonicMs) noexcept {
    using Duration = sonalis::core::VoiceSession::Clock::duration;
    return sonalis::core::VoiceSession::Clock::time_point(
        std::chrono::duration_cast<Duration>(std::chrono::milliseconds(monotonicMs)));
}

sonalis::core::VoicePolicy Policy(const sonalis_voice_policy value) noexcept {
    return {
        .peerToPeerEnabled = value.peer_to_peer_enabled != 0,
        .serverDenoiseEnabled = value.server_denoise_enabled != 0,
        .peerStability = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::milliseconds(value.peer_stability_ms)),
        .soloSleepDelay = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::milliseconds(value.solo_sleep_delay_ms)),
        .probeTimeout = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::milliseconds(value.probe_timeout_ms)),
    };
}

sonalis_voice_decision Decision(const sonalis::core::VoiceDecision value) noexcept {
    return {
        .lifecycle = static_cast<uint8_t>(value.lifecycle),
        .route = static_cast<uint8_t>(value.route),
        .begin_peer_probe = static_cast<uint8_t>(value.beginPeerProbe),
        .cancel_peer_probe = static_cast<uint8_t>(value.cancelPeerProbe),
        .release_media = static_cast<uint8_t>(value.releaseMedia),
        .request_wake_grant = static_cast<uint8_t>(value.requestWakeGrant),
    };
}

sonalis_voice_decision EmptyDecision() noexcept { return {}; }

}  // namespace

extern "C" {

uint32_t sonalis_core_abi_version(void) { return SONALIS_CORE_ABI_VERSION; }

sonalis_voice_session* sonalis_voice_session_create(const sonalis_voice_policy policy) {
    return new (std::nothrow) sonalis_voice_session(Policy(policy));
}

void sonalis_voice_session_destroy(sonalis_voice_session* session) { delete session; }

void sonalis_voice_session_connected(sonalis_voice_session* session, const uint64_t monotonicMs) {
    if (session != nullptr) session->value.Connected(TimePoint(monotonicMs));
}

void sonalis_voice_session_disconnected(sonalis_voice_session* session) {
    if (session != nullptr) session->value.Disconnected();
}

sonalis_voice_decision sonalis_voice_session_participants_changed(
    sonalis_voice_session* session, const uint32_t activeParticipants, const uint64_t monotonicMs) {
    return session == nullptr ? EmptyDecision()
        : Decision(session->value.ParticipantsChanged(activeParticipants, TimePoint(monotonicMs)));
}

sonalis_voice_decision sonalis_voice_session_tick(
    sonalis_voice_session* session, const uint64_t monotonicMs) {
    return session == nullptr ? EmptyDecision() : Decision(session->value.Tick(TimePoint(monotonicMs)));
}

sonalis_voice_decision sonalis_voice_session_probe_succeeded(
    sonalis_voice_session* session, const uint64_t monotonicMs) {
    return session == nullptr ? EmptyDecision()
        : Decision(session->value.PeerProbeSucceeded(TimePoint(monotonicMs)));
}

sonalis_voice_decision sonalis_voice_session_probe_failed(
    sonalis_voice_session* session, const uint64_t monotonicMs) {
    return session == nullptr ? EmptyDecision()
        : Decision(session->value.PeerProbeFailed(TimePoint(monotonicMs)));
}

void sonalis_voice_session_set_policy(
    sonalis_voice_session* session, const sonalis_voice_policy policy, const uint64_t monotonicMs) {
    if (session != nullptr) session->value.SetPolicy(Policy(policy), TimePoint(monotonicMs));
}

sonalis_event_generation* sonalis_event_generation_create(void) {
    return new (std::nothrow) sonalis_event_generation{};
}

void sonalis_event_generation_destroy(sonalis_event_generation* state) { delete state; }

uint64_t sonalis_event_generation_mark_dirty(sonalis_event_generation* state) {
    return state == nullptr ? 0 : state->value.MarkDirty();
}

uint8_t sonalis_event_generation_try_begin(sonalis_event_generation* state, uint64_t* token) {
    if (state == nullptr || token == nullptr) return 0;
    return state->value.TryBegin(*token) ? 1 : 0;
}

uint8_t sonalis_event_generation_complete(sonalis_event_generation* state, const uint64_t token) {
    return state != nullptr && state->value.Complete(token) ? 1 : 0;
}

void sonalis_event_generation_reset(sonalis_event_generation* state) {
    if (state != nullptr) state->value.Reset();
}

uint8_t sonalis_crypto_aead_lock(
    uint8_t* packedCiphertext, const uint64_t packedSize,
    const uint8_t* key, const uint8_t* nonce,
    const uint8_t* associatedData, const uint64_t associatedSize,
    const uint8_t* plaintext, const uint64_t plaintextSize) {
    if (packedCiphertext == nullptr || key == nullptr || nonce == nullptr
        || (associatedSize != 0 && associatedData == nullptr)
        || (plaintextSize != 0 && plaintext == nullptr)) return 0;
    return sonalis::core::AeadLock(
        std::span<std::uint8_t>(packedCiphertext, static_cast<std::size_t>(packedSize)),
        std::span<const std::uint8_t, sonalis::core::SymmetricKeyBytes>(key, sonalis::core::SymmetricKeyBytes),
        std::span<const std::uint8_t, sonalis::core::XChaChaNonceBytes>(nonce, sonalis::core::XChaChaNonceBytes),
        std::span<const std::uint8_t>(associatedData, static_cast<std::size_t>(associatedSize)),
        std::span<const std::uint8_t>(plaintext, static_cast<std::size_t>(plaintextSize))) ? 1 : 0;
}

uint8_t sonalis_crypto_aead_unlock(
    uint8_t* plaintext, const uint64_t plaintextSize,
    const uint8_t* key, const uint8_t* nonce,
    const uint8_t* associatedData, const uint64_t associatedSize,
    const uint8_t* packedCiphertext, const uint64_t packedSize) {
    if (plaintext == nullptr || key == nullptr || nonce == nullptr || packedCiphertext == nullptr
        || (associatedSize != 0 && associatedData == nullptr)) return 0;
    return sonalis::core::AeadUnlock(
        std::span<std::uint8_t>(plaintext, static_cast<std::size_t>(plaintextSize)),
        std::span<const std::uint8_t, sonalis::core::SymmetricKeyBytes>(key, sonalis::core::SymmetricKeyBytes),
        std::span<const std::uint8_t, sonalis::core::XChaChaNonceBytes>(nonce, sonalis::core::XChaChaNonceBytes),
        std::span<const std::uint8_t>(associatedData, static_cast<std::size_t>(associatedSize)),
        std::span<const std::uint8_t>(packedCiphertext, static_cast<std::size_t>(packedSize))) ? 1 : 0;
}

void sonalis_crypto_ed25519_key_pair(uint8_t* secret, uint8_t* publicKey, const uint8_t* seed) {
    if (secret != nullptr && publicKey != nullptr && seed != nullptr) {
        sonalis::core::Ed25519KeyPair(std::span<std::uint8_t, 64>(secret, 64),
            std::span<std::uint8_t, 32>(publicKey, 32), std::span<const std::uint8_t, 32>(seed, 32));
    }
}

void sonalis_crypto_ed25519_sign(uint8_t* signature, const uint8_t* secret,
                                 const uint8_t* message, const uint64_t messageSize) {
    if (signature != nullptr && secret != nullptr && (messageSize == 0 || message != nullptr)) {
        sonalis::core::Ed25519Sign(std::span<std::uint8_t, 64>(signature, 64),
            std::span<const std::uint8_t, 64>(secret, 64),
            std::span<const std::uint8_t>(message, static_cast<std::size_t>(messageSize)));
    }
}

uint8_t sonalis_crypto_ed25519_verify(const uint8_t* signature, const uint8_t* publicKey,
                                      const uint8_t* message, const uint64_t messageSize) {
    return signature != nullptr && publicKey != nullptr && (messageSize == 0 || message != nullptr)
        && sonalis::core::Ed25519Verify(std::span<const std::uint8_t, 64>(signature, 64),
            std::span<const std::uint8_t, 32>(publicKey, 32),
            std::span<const std::uint8_t>(message, static_cast<std::size_t>(messageSize))) ? 1 : 0;
}

void sonalis_crypto_x25519_public(uint8_t* publicKey, const uint8_t* secret) {
    if (publicKey != nullptr && secret != nullptr) sonalis::core::X25519Public(
        std::span<std::uint8_t, 32>(publicKey, 32), std::span<const std::uint8_t, 32>(secret, 32));
}

void sonalis_crypto_x25519(uint8_t* shared, const uint8_t* secret, const uint8_t* publicKey) {
    if (shared != nullptr && secret != nullptr && publicKey != nullptr) {
        sonalis::core::X25519(std::span<std::uint8_t, 32>(shared, 32),
            std::span<const std::uint8_t, 32>(secret, 32),
            std::span<const std::uint8_t, 32>(publicKey, 32));
    }
}

void sonalis_crypto_wipe(uint8_t* bytes, const uint64_t size) {
    if (bytes != nullptr) sonalis::core::SecureWipe(
        std::span<std::uint8_t>(bytes, static_cast<std::size_t>(size)));
}

}  // extern "C"
