import Foundation

struct SonalisVoiceDecision {
    let lifecycle: UInt8
    let route: UInt8
    let beginPeerProbe: Bool
    let cancelPeerProbe: Bool
    let releaseMedia: Bool
    let requestWakeGrant: Bool
}

final class SonalisVoiceSession {
    private var handle: OpaquePointer?

    init(peerToPeer: Bool = true, serverDenoise: Bool = false) {
        let policy = sonalis_voice_policy(
            peer_to_peer_enabled: peerToPeer ? 1 : 0,
            server_denoise_enabled: serverDenoise ? 1 : 0,
            peer_stability_ms: 10_000,
            solo_sleep_delay_ms: 60_000,
            probe_timeout_ms: 2_000
        )
        handle = sonalis_voice_session_create(policy)
        precondition(handle != nil, "sonalis_core_allocation_failed")
    }

    deinit { sonalis_voice_session_destroy(handle) }

    func connected(at monotonicMs: UInt64) { sonalis_voice_session_connected(handle, monotonicMs) }
    func disconnected() { sonalis_voice_session_disconnected(handle) }
    func participantsChanged(_ count: UInt32, at monotonicMs: UInt64) -> SonalisVoiceDecision {
        unpack(sonalis_voice_session_participants_changed(handle, count, monotonicMs))
    }
    func tick(at monotonicMs: UInt64) -> SonalisVoiceDecision {
        unpack(sonalis_voice_session_tick(handle, monotonicMs))
    }
    func probeSucceeded(at monotonicMs: UInt64) -> SonalisVoiceDecision {
        unpack(sonalis_voice_session_probe_succeeded(handle, monotonicMs))
    }
    func probeFailed(at monotonicMs: UInt64) -> SonalisVoiceDecision {
        unpack(sonalis_voice_session_probe_failed(handle, monotonicMs))
    }

    private func unpack(_ value: sonalis_voice_decision) -> SonalisVoiceDecision {
        SonalisVoiceDecision(
            lifecycle: value.lifecycle,
            route: value.route,
            beginPeerProbe: value.begin_peer_probe != 0,
            cancelPeerProbe: value.cancel_peer_probe != 0,
            releaseMedia: value.release_media != 0,
            requestWakeGrant: value.request_wake_grant != 0
        )
    }
}

