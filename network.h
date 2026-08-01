#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "protocol.h"
#include "platform_api.h"
#include "ice_transport.h"
#include "sonalis/core/voice_session.h"

namespace ss {

struct PeerInfo {
    std::uint32_t peerId{};
    std::string nickname;
};

enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Error,
};

enum class VoicePath { Relay, Probing, DirectPeer };

struct NetworkDiagnosticsSnapshot {
    std::uint64_t udpAudioSent{};
    std::uint64_t udpAudioRejected{};
    std::uint64_t udpAudioReceived{};
    std::uint64_t udpDecryptRejected{};
};

class NetworkClient final {
public:
    using AudioCallback = std::function<void(std::uint32_t, std::uint16_t, std::uint32_t,
                                             std::uint8_t, std::span<const std::uint8_t>)>;
    using PeerRemovedCallback = std::function<void(std::uint32_t)>;
    using StateCallback = std::function<void()>;

    NetworkClient();
    ~NetworkClient();
    NetworkClient(const NetworkClient&) = delete;
    NetworkClient& operator=(const NetworkClient&) = delete;

    bool Connect(const protocol::ServerAddress& address,
                 const std::string& nickname,
                 const std::string& room,
                 bool serverDenoiseRequested,
                 std::string& error);
    bool ConnectSecure(const VoiceGrant& voiceGrant, const std::vector<PlatformMember>& members,
                       bool p2pEnabled, std::string& error);
    void Disconnect();

    bool SendAudio(std::uint16_t sequence,
                   std::uint32_t timestamp,
                   std::span<const std::uint8_t> opus,
                   std::uint8_t flags = 0) noexcept;

    [[nodiscard]] ConnectionState State() const noexcept;
    [[nodiscard]] std::string StatusText() const;
    [[nodiscard]] std::vector<PeerInfo> Peers() const;
    [[nodiscard]] std::shared_ptr<const std::vector<PeerInfo>> PeersSnapshot() const noexcept;
    [[nodiscard]] std::uint32_t LocalPeerId() const noexcept;
    [[nodiscard]] bool ServerDenoiseAvailable() const noexcept;
    [[nodiscard]] bool ServerDenoiseEnabled() const noexcept;
    [[nodiscard]] std::uint64_t AudioBytesSent() const noexcept;
    [[nodiscard]] std::uint64_t AudioPacketsSent() const noexcept;
    [[nodiscard]] VoicePath ActiveVoicePath() const noexcept;
    [[nodiscard]] NetworkDiagnosticsSnapshot Diagnostics() const noexcept;
    bool BeginEncryptedEchoTest() noexcept;
    [[nodiscard]] std::string EchoTestStatus() const;
    bool SetServerDenoiseRequested(bool enabled) noexcept;

    void SetAudioCallback(AudioCallback callback);
    void SetPeerRemovedCallback(PeerRemovedCallback callback);
    void SetStateCallback(StateCallback callback);

private:
    void TcpLoop();
    void UdpLoop();
    void SecureUdpLoop();
    bool SendSecure(std::span<const std::uint8_t> plaintext, std::uint8_t flags) noexcept;
    bool SendP2P(std::span<const std::uint8_t> plaintext) noexcept;
    bool InitializeSecureCipher(std::span<const std::uint8_t> key, std::string& error);
    bool InitializeP2PCipher(std::span<const std::uint8_t> key) noexcept;
    void DestroySecureCipher() noexcept;
    void DestroyP2PCipher() noexcept;
    void ResetP2P(bool clearKeyAgreement = false) noexcept;
    bool PrepareP2PKeyAgreement() noexcept;
    bool DeriveP2PKey(std::span<const std::uint8_t> peerPublicKey,
                      std::span<const std::uint8_t> pairId,
                      std::array<std::uint8_t, 32>& key) noexcept;
    void ConfigureP2P(std::span<const std::uint8_t> plaintext);
    bool HandleP2PPacket(std::span<const std::uint8_t> packet, const sockaddr_in* remote);
    void HandleSignal(const std::string& jsonText);
    bool SendJson(const std::string& jsonText) noexcept;
    bool SendRegister() noexcept;
    bool SendKeepAlive() noexcept;
    void SetState(ConnectionState state, std::string text);
    void MarkTransportClosed(const std::string& reason);
    void PublishPeerSnapshotLocked();
    void SetCoreParticipants(std::size_t activeParticipants) noexcept;
    [[nodiscard]] bool CoreAllowsPeerRoute() const noexcept;

    bool winsockReady_{false};
    std::atomic<bool> running_{false};
    std::atomic<ConnectionState> state_{ConnectionState::Disconnected};
    std::atomic<SOCKET> tcpSocket_{INVALID_SOCKET};
    std::atomic<SOCKET> udpSocket_{INVALID_SOCKET};
    sockaddr_in secureServerEndpoint_{};
    std::thread tcpThread_;
    std::thread udpThread_;
    std::array<std::uint8_t, protocol::kTokenBytes> udpToken_{};
    std::atomic<std::uint32_t> localPeerId_{0};
    std::atomic<bool> serverDenoiseAvailable_{false};
    std::atomic<bool> serverDenoiseEnabled_{false};
    std::atomic<bool> serverDenoiseRequested_{false};
    std::atomic<std::uint64_t> audioBytesSent_{0};
    std::atomic<std::uint64_t> audioPacketsSent_{0};
    std::atomic<std::uint64_t> audioPacketsRejected_{0};
    std::atomic<std::uint64_t> audioPacketsReceived_{0};
    std::atomic<std::uint64_t> decryptRejected_{0};
    std::atomic<std::uint64_t> echoNonce_{0};
    std::atomic<std::uint64_t> echoSentAtMs_{0};
    std::atomic<int> echoRttMs_{-1};
    std::atomic<bool> secureMode_{false};
    std::array<std::uint8_t, 16> secureSessionId_{};
    std::array<std::uint8_t, 4> secureNoncePrefix_{};
    std::atomic<std::uint64_t> secureOutgoingSequence_{0};
    std::uint64_t secureHighestIncoming_{};
    std::uint64_t secureReplayMask_{};
    bool secureReceivedAny_{false};
    void* secureAlgorithm_{};
    void* secureKey_{};
    std::vector<std::uint8_t> secureKeyObject_;
    std::mutex secureReplayMutex_;
    std::mutex secureCipherMutex_;
    std::atomic<bool> p2pConfigured_{false};
    std::atomic<bool> p2pReady_{false};
    std::atomic<bool> p2pAllowed_{false};
    std::atomic<bool> p2pUsesIce_{false};
    IceTransport iceTransport_;
    std::array<std::uint8_t, 32> p2pSecret_{};
    std::array<std::uint8_t, 32> p2pPublic_{};
    bool p2pKeyAgreementReady_{false};
    std::array<std::uint8_t, 16> p2pPairId_{};
    std::array<std::uint8_t, 4> p2pOutgoingPrefix_{};
    std::array<std::uint8_t, 4> p2pIncomingPrefix_{};
    sockaddr_in p2pEndpoint_{};
    std::atomic<std::uint64_t> p2pOutgoingSequence_{0};
    std::uint64_t p2pHighestIncoming_{};
    std::uint64_t p2pReplayMask_{};
    bool p2pReceivedAny_{false};
    std::atomic<std::uint64_t> p2pLastSeenMs_{0};
    std::atomic<std::uint64_t> p2pConfiguredAtMs_{0};
    std::string p2pPeerUserId_;
    std::uint32_t p2pPeerId_{};
    std::array<std::uint8_t, 16> p2pIcePairId_{};
    std::vector<std::uint8_t> p2pIceDescription_;
    std::vector<bool> p2pIceReceived_;
    std::size_t p2pIceReceivedBytes_{};
    bool p2pIceRemoteApplied_{};
    void* p2pAlgorithm_{};
    void* p2pKey_{};
    std::vector<std::uint8_t> p2pKeyObject_;
    std::mutex p2pMutex_;
    std::mutex p2pCipherMutex_;
    std::mutex secureBindMutex_;
    std::condition_variable secureBindCv_;
    std::atomic<bool> secureBindAcknowledged_{false};
    std::unordered_map<std::string, std::string> securePeerNames_;
    std::mutex tcpSendMutex_;
    std::mutex udpSendMutex_;
    mutable std::mutex statusMutex_;
    std::string statusText_{"Bagli degil"};
    mutable std::mutex peersMutex_;
    std::vector<PeerInfo> peers_;
    std::atomic<std::shared_ptr<const std::vector<PeerInfo>>> peerSnapshot_{
        std::make_shared<const std::vector<PeerInfo>>()};
    mutable std::mutex voiceLifecycleMutex_;
    sonalis::core::VoiceSession voiceLifecycle_;
    mutable std::mutex callbackMutex_;
    AudioCallback audioCallback_;
    PeerRemovedCallback peerRemovedCallback_;
    StateCallback stateCallback_;
};

}  // namespace ss
