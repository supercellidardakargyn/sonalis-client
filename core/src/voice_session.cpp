#include "sonalis/core/voice_session.h"

#include <utility>

namespace sonalis::core {

VoiceSession::VoiceSession(VoicePolicy policy) noexcept : policy_(std::move(policy)) {}

void VoiceSession::Connected(const Clock::time_point now) noexcept {
    lifecycle_ = VoiceLifecycle::Active;
    route_ = VoiceRoute::Relay;
    participants_ = 0;
    occupancyChangedAt_ = now;
    probeStartedAt_ = {};
    peerProbeIssued_ = false;
}

void VoiceSession::Disconnected() noexcept {
    lifecycle_ = VoiceLifecycle::Disconnected;
    route_ = VoiceRoute::Relay;
    participants_ = 0;
    peerProbeIssued_ = false;
    probeStartedAt_ = {};
}

VoiceDecision VoiceSession::ParticipantsChanged(const std::size_t activeParticipants,
                                                const Clock::time_point now) noexcept {
    const auto previous = participants_;
    participants_ = activeParticipants;
    if (previous != activeParticipants) occupancyChangedAt_ = now;

    VoiceDecision decision = Snapshot();
    if (lifecycle_ == VoiceLifecycle::Disconnected) return decision;
    if (lifecycle_ == VoiceLifecycle::Sleeping && activeParticipants >= 2) {
        lifecycle_ = VoiceLifecycle::Active;
        route_ = VoiceRoute::Relay;
        ResetPeerCandidate(now);
        decision = Snapshot();
        decision.requestWakeGrant = true;
        return decision;
    }
    if (activeParticipants != 2 || policy_.serverDenoiseEnabled || !policy_.peerToPeerEnabled) {
        decision.cancelPeerProbe = route_ != VoiceRoute::Relay || peerProbeIssued_;
        route_ = VoiceRoute::Relay;
        ResetPeerCandidate(now);
        decision.lifecycle = lifecycle_;
        decision.route = route_;
    }
    return decision;
}

VoiceDecision VoiceSession::Tick(const Clock::time_point now) noexcept {
    VoiceDecision decision = Snapshot();
    if (lifecycle_ != VoiceLifecycle::Active) return decision;

    if (participants_ <= 1 && now - occupancyChangedAt_ >= policy_.soloSleepDelay) {
        lifecycle_ = VoiceLifecycle::Sleeping;
        route_ = VoiceRoute::Relay;
        peerProbeIssued_ = false;
        decision = Snapshot();
        decision.releaseMedia = true;
        return decision;
    }

    const bool mayProbe = participants_ == 2 && policy_.peerToPeerEnabled && !policy_.serverDenoiseEnabled;
    if (mayProbe && route_ == VoiceRoute::Relay && !peerProbeIssued_
        && now - occupancyChangedAt_ >= policy_.peerStability) {
        peerProbeIssued_ = true;
        probeStartedAt_ = now;
        route_ = VoiceRoute::Probing;
        decision = Snapshot();
        decision.beginPeerProbe = true;
        return decision;
    }
    if (route_ == VoiceRoute::Probing && now - probeStartedAt_ >= policy_.probeTimeout) {
        return PeerProbeFailed(now);
    }
    return decision;
}

VoiceDecision VoiceSession::PeerProbeSucceeded(const Clock::time_point now) noexcept {
    if (lifecycle_ != VoiceLifecycle::Active || participants_ != 2
        || !policy_.peerToPeerEnabled || policy_.serverDenoiseEnabled) {
        return PeerProbeFailed(now);
    }
    route_ = VoiceRoute::PeerToPeer;
    peerProbeIssued_ = false;
    probeStartedAt_ = {};
    return Snapshot();
}

VoiceDecision VoiceSession::PeerProbeFailed(const Clock::time_point now) noexcept {
    const bool wasProbing = route_ == VoiceRoute::Probing || peerProbeIssued_;
    route_ = VoiceRoute::Relay;
    peerProbeIssued_ = false;
    probeStartedAt_ = {};
    // Back off until the two-person topology has been stable again.
    occupancyChangedAt_ = now;
    VoiceDecision decision = Snapshot();
    decision.cancelPeerProbe = wasProbing;
    return decision;
}

void VoiceSession::SetPolicy(VoicePolicy policy, const Clock::time_point now) noexcept {
    policy_ = std::move(policy);
    if (!policy_.peerToPeerEnabled || policy_.serverDenoiseEnabled) {
        route_ = VoiceRoute::Relay;
        ResetPeerCandidate(now);
    }
}

VoiceDecision VoiceSession::Snapshot() const noexcept {
    return VoiceDecision{.lifecycle = lifecycle_, .route = route_};
}

void VoiceSession::ResetPeerCandidate(const Clock::time_point now) noexcept {
    peerProbeIssued_ = false;
    probeStartedAt_ = {};
    occupancyChangedAt_ = now;
}

}  // namespace sonalis::core
