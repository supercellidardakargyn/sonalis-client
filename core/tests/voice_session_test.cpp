#include <cassert>
#include <chrono>

#include "sonalis/core/voice_session.h"

int main() {
    using namespace std::chrono_literals;
    using sonalis::core::VoiceLifecycle;
    using sonalis::core::VoiceRoute;
    using sonalis::core::VoiceSession;

    const auto start = VoiceSession::Clock::time_point{};
    VoiceSession session;
    session.Connected(start);
    (void)session.ParticipantsChanged(2, start);
    assert(!session.Tick(start + 9s).beginPeerProbe);
    assert(session.Tick(start + 10s).beginPeerProbe);
    assert(session.Route() == VoiceRoute::Probing);
    (void)session.PeerProbeSucceeded(start + 11s);
    assert(session.Route() == VoiceRoute::PeerToPeer);
    assert(session.ParticipantsChanged(3, start + 12s).route == VoiceRoute::Relay);

    (void)session.ParticipantsChanged(1, start + 20s);
    assert(!session.Tick(start + 79s).releaseMedia);
    assert(session.Tick(start + 80s).releaseMedia);
    assert(session.Lifecycle() == VoiceLifecycle::Sleeping);
    assert(session.ParticipantsChanged(2, start + 81s).requestWakeGrant);
    return 0;
}
