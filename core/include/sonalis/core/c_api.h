#pragma once

#include <stdint.h>

#if defined(_WIN32)
#  if defined(SONALIS_CORE_EXPORTS)
#    define SONALIS_CORE_API __declspec(dllexport)
#  else
#    define SONALIS_CORE_API
#  endif
#else
#  define SONALIS_CORE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sonalis_voice_session sonalis_voice_session;
typedef struct sonalis_event_generation sonalis_event_generation;
typedef struct sonalis_voice_encoder sonalis_voice_encoder;
typedef struct sonalis_voice_decoder sonalis_voice_decoder;

#define SONALIS_CORE_ABI_VERSION 2u

SONALIS_CORE_API uint32_t sonalis_core_abi_version(void);

typedef struct sonalis_voice_policy {
    uint8_t peer_to_peer_enabled;
    uint8_t server_denoise_enabled;
    uint32_t peer_stability_ms;
    uint32_t solo_sleep_delay_ms;
    uint32_t probe_timeout_ms;
} sonalis_voice_policy;

typedef struct sonalis_voice_decision {
    uint8_t lifecycle;
    uint8_t route;
    uint8_t begin_peer_probe;
    uint8_t cancel_peer_probe;
    uint8_t release_media;
    uint8_t request_wake_grant;
} sonalis_voice_decision;

SONALIS_CORE_API sonalis_voice_session* sonalis_voice_session_create(sonalis_voice_policy policy);
SONALIS_CORE_API void sonalis_voice_session_destroy(sonalis_voice_session* session);
SONALIS_CORE_API void sonalis_voice_session_connected(sonalis_voice_session* session, uint64_t monotonic_ms);
SONALIS_CORE_API void sonalis_voice_session_disconnected(sonalis_voice_session* session);
SONALIS_CORE_API sonalis_voice_decision sonalis_voice_session_participants_changed(
    sonalis_voice_session* session, uint32_t active_participants, uint64_t monotonic_ms);
SONALIS_CORE_API sonalis_voice_decision sonalis_voice_session_tick(
    sonalis_voice_session* session, uint64_t monotonic_ms);
SONALIS_CORE_API sonalis_voice_decision sonalis_voice_session_probe_succeeded(
    sonalis_voice_session* session, uint64_t monotonic_ms);
SONALIS_CORE_API sonalis_voice_decision sonalis_voice_session_probe_failed(
    sonalis_voice_session* session, uint64_t monotonic_ms);
SONALIS_CORE_API void sonalis_voice_session_set_policy(
    sonalis_voice_session* session, sonalis_voice_policy policy, uint64_t monotonic_ms);

SONALIS_CORE_API sonalis_event_generation* sonalis_event_generation_create(void);
SONALIS_CORE_API void sonalis_event_generation_destroy(sonalis_event_generation* state);
SONALIS_CORE_API uint64_t sonalis_event_generation_mark_dirty(sonalis_event_generation* state);
SONALIS_CORE_API uint8_t sonalis_event_generation_try_begin(
    sonalis_event_generation* state, uint64_t* token);
SONALIS_CORE_API uint8_t sonalis_event_generation_complete(
    sonalis_event_generation* state, uint64_t token);
SONALIS_CORE_API void sonalis_event_generation_reset(sonalis_event_generation* state);

SONALIS_CORE_API uint8_t sonalis_crypto_aead_lock(
    uint8_t* packed_ciphertext, uint64_t packed_size,
    const uint8_t* key_32, const uint8_t* nonce_24,
    const uint8_t* associated_data, uint64_t associated_size,
    const uint8_t* plaintext, uint64_t plaintext_size);
SONALIS_CORE_API uint8_t sonalis_crypto_aead_unlock(
    uint8_t* plaintext, uint64_t plaintext_size,
    const uint8_t* key_32, const uint8_t* nonce_24,
    const uint8_t* associated_data, uint64_t associated_size,
    const uint8_t* packed_ciphertext, uint64_t packed_size);
SONALIS_CORE_API void sonalis_crypto_ed25519_key_pair(
    uint8_t* secret_64, uint8_t* public_32, const uint8_t* seed_32);
SONALIS_CORE_API void sonalis_crypto_ed25519_sign(
    uint8_t* signature_64, const uint8_t* secret_64,
    const uint8_t* message, uint64_t message_size);
SONALIS_CORE_API uint8_t sonalis_crypto_ed25519_verify(
    const uint8_t* signature_64, const uint8_t* public_32,
    const uint8_t* message, uint64_t message_size);
SONALIS_CORE_API void sonalis_crypto_x25519_public(uint8_t* public_32, const uint8_t* secret_32);
SONALIS_CORE_API void sonalis_crypto_x25519(
    uint8_t* shared_32, const uint8_t* secret_32, const uint8_t* public_32);
SONALIS_CORE_API void sonalis_crypto_wipe(uint8_t* bytes, uint64_t size);

SONALIS_CORE_API sonalis_voice_encoder* sonalis_voice_encoder_create(uint32_t bitrate);
SONALIS_CORE_API void sonalis_voice_encoder_destroy(sonalis_voice_encoder* encoder);
SONALIS_CORE_API uint8_t sonalis_voice_encoder_set_bitrate(
    sonalis_voice_encoder* encoder, uint32_t bitrate);
SONALIS_CORE_API int32_t sonalis_voice_encoder_encode(
    sonalis_voice_encoder* encoder, const float* pcm_960,
    uint8_t* output, uint32_t output_capacity);
SONALIS_CORE_API sonalis_voice_decoder* sonalis_voice_decoder_create(void);
SONALIS_CORE_API void sonalis_voice_decoder_destroy(sonalis_voice_decoder* decoder);
SONALIS_CORE_API int32_t sonalis_voice_decoder_decode(
    sonalis_voice_decoder* decoder, const uint8_t* packet, uint32_t packet_size,
    float* output_960, uint8_t fec);
SONALIS_CORE_API int32_t sonalis_voice_decoder_conceal(
    sonalis_voice_decoder* decoder, float* output_960);
SONALIS_CORE_API void sonalis_voice_decoder_reset(sonalis_voice_decoder* decoder);
SONALIS_CORE_API float sonalis_voice_rms(const float* samples, uint32_t sample_count);

#ifdef __cplusplus
}
#endif
