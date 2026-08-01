#include "sonalis/core/c_api.h"

#include <cassert>

int main() {
    assert(sonalis_core_abi_version() == SONALIS_CORE_ABI_VERSION);
    const sonalis_voice_policy policy{1, 0, 10'000, 60'000, 2'000};
    sonalis_voice_session* session = sonalis_voice_session_create(policy);
    assert(session != nullptr);
    sonalis_voice_session_connected(session, 1'000);
    auto decision = sonalis_voice_session_participants_changed(session, 2, 1'000);
    assert(decision.route == 0);
    decision = sonalis_voice_session_tick(session, 11'000);
    assert(decision.begin_peer_probe == 1);
    decision = sonalis_voice_session_probe_succeeded(session, 11'100);
    assert(decision.route == 2);
    sonalis_voice_session_destroy(session);

    sonalis_event_generation* events = sonalis_event_generation_create();
    assert(events != nullptr);
    assert(sonalis_event_generation_mark_dirty(events) == 1);
    uint64_t eventToken = 0;
    assert(sonalis_event_generation_try_begin(events, &eventToken) == 1);
    assert(eventToken == 1);
    assert(sonalis_event_generation_mark_dirty(events) == 2);
    assert(sonalis_event_generation_complete(events, eventToken) == 1);
    sonalis_event_generation_destroy(events);

    sonalis_voice_encoder* encoder = sonalis_voice_encoder_create(24'000);
    sonalis_voice_decoder* decoder = sonalis_voice_decoder_create();
    assert(encoder != nullptr);
    assert(decoder != nullptr);
    float pcm[960]{};
    pcm[0] = 0.5F;
    uint8_t packet[1'275]{};
    const auto encoded = sonalis_voice_encoder_encode(encoder, pcm, packet, sizeof(packet));
    assert(encoded > 0);
    float decoded[960]{};
    assert(sonalis_voice_decoder_decode(
        decoder, packet, static_cast<uint32_t>(encoded), decoded, 0) == 960);
    assert(sonalis_voice_rms(pcm, 960) > 0.0F);
    assert(sonalis_voice_decoder_conceal(decoder, decoded) == 960);
    sonalis_voice_decoder_destroy(decoder);
    sonalis_voice_encoder_destroy(encoder);
}
