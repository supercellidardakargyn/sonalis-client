#include "network.h"

#include <ws2tcpip.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>

#include <nlohmann/json.hpp>
#include <monocypher.h>

#include "http_client.h"
#include "diagnostics.h"

namespace ss {
namespace {

using Clock = std::chrono::steady_clock;

std::string WinsockError(const char* operation) {
    return std::string(operation) + " (Winsock " + std::to_string(WSAGetLastError()) + ")";
}

bool SendAll(const SOCKET socket, const std::uint8_t* data, std::size_t size) {
    while (size > 0) {
        const int chunk = static_cast<int>(std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int sent = send(socket, reinterpret_cast<const char*>(data), chunk, 0);
        if (sent == SOCKET_ERROR || sent == 0) return false;
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool DecodeBase64(const std::string& text, std::vector<std::uint8_t>& output) {
    DWORD bytes = 0;
    if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, nullptr, &bytes, nullptr, nullptr)) return false;
    output.resize(bytes);
    return CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, output.data(), &bytes, nullptr, nullptr) != FALSE;
}

std::string EncodeBase64(const std::span<const std::uint8_t> input) {
    DWORD bytes = 0;
    if (!CryptBinaryToStringA(input.data(), static_cast<DWORD>(input.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &bytes)) return {};
    std::string result(bytes, '\0');
    if (!CryptBinaryToStringA(input.data(), static_cast<DWORD>(input.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &bytes)) return {};
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

int HexDigit(const char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool ParseUuid(const std::string& value, std::array<std::uint8_t, 16>& output) {
    std::string hex;
    for (const char character : value) if (character != '-') hex.push_back(character);
    if (hex.size() != 32) return false;
    for (std::size_t index = 0; index < output.size(); ++index) {
        const int high = HexDigit(hex[index * 2]); const int low = HexDigit(hex[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

std::uint32_t PeerIdFor(const std::string& value) {
    std::uint32_t hash = 2166136261U;
    for (const unsigned char character : value) { hash ^= character; hash *= 16777619U; }
    return hash == 0 ? 1U : hash;
}

void WriteBig16(std::uint8_t* target, const std::uint16_t value) { target[0] = static_cast<std::uint8_t>(value >> 8U); target[1] = static_cast<std::uint8_t>(value); }
void WriteBig32(std::uint8_t* target, const std::uint32_t value) { for (int i = 0; i < 4; ++i) target[i] = static_cast<std::uint8_t>(value >> (24 - i * 8)); }
void WriteBig64(std::uint8_t* target, const std::uint64_t value) { for (int i = 0; i < 8; ++i) target[i] = static_cast<std::uint8_t>(value >> (56 - i * 8)); }
std::uint16_t ReadBig16(const std::uint8_t* source) { return static_cast<std::uint16_t>((source[0] << 8U) | source[1]); }
std::uint32_t ReadBig32(const std::uint8_t* source) { return (static_cast<std::uint32_t>(source[0]) << 24U) | (static_cast<std::uint32_t>(source[1]) << 16U) | (static_cast<std::uint32_t>(source[2]) << 8U) | source[3]; }
std::uint64_t ReadBig64(const std::uint8_t* source) { std::uint64_t value = 0; for (int i = 0; i < 8; ++i) value = (value << 8U) | source[i]; return value; }
std::uint64_t SteadyMilliseconds() { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count()); }
bool SameEndpoint(const sockaddr_in& left, const sockaddr_in& right) {
    return left.sin_family == right.sin_family && left.sin_port == right.sin_port
        && left.sin_addr.s_addr == right.sin_addr.s_addr;
}

}  // namespace

NetworkClient::NetworkClient() {
    WSADATA data{};
    winsockReady_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

NetworkClient::~NetworkClient() {
    Disconnect();
    if (winsockReady_) WSACleanup();
}

bool NetworkClient::Connect(const protocol::ServerAddress& address,
                            const std::string& nickname,
                            const std::string& room,
                            const bool serverDenoiseRequested,
                            std::string& error) {
    Disconnect();
    if (!winsockReady_) {
        error = "Winsock baslatilamadi";
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    const std::string portText = std::to_string(address.port);
    if (getaddrinfo(address.host.c_str(), portText.c_str(), &hints, &result) != 0 || result == nullptr) {
        error = "Sunucu adresi cozumlenemedi";
        if (result != nullptr) freeaddrinfo(result);
        return false;
    }

    sockaddr_in endpoint{};
    std::memcpy(&endpoint, result->ai_addr, sizeof(endpoint));
    freeaddrinfo(result);

    const SOCKET tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tcp == INVALID_SOCKET) {
        error = WinsockError("TCP soketi acilamadi");
        return false;
    }
    BOOL noDelay = TRUE;
    setsockopt(tcp, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    if (connect(tcp, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == SOCKET_ERROR) {
        error = WinsockError("Sunucuya baglanilamadi");
        closesocket(tcp);
        return false;
    }

    const SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp == INVALID_SOCKET) {
        error = WinsockError("UDP soketi acilamadi");
        closesocket(tcp);
        return false;
    }
    DWORD receiveTimeoutMs = 500;
    setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receiveTimeoutMs), sizeof(receiveTimeoutMs));
    if (connect(udp, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == SOCKET_ERROR) {
        error = WinsockError("UDP hedefi ayarlanamadi");
        closesocket(udp);
        closesocket(tcp);
        return false;
    }

    tcpSocket_.store(tcp);
    udpSocket_.store(udp);
    localPeerId_.store(0);
    serverDenoiseAvailable_.store(false);
    serverDenoiseEnabled_.store(false);
    serverDenoiseRequested_.store(serverDenoiseRequested);
    audioBytesSent_.store(0);
    audioPacketsSent_.store(0);
    audioPacketsRejected_.store(0);
    audioPacketsReceived_.store(0);
    decryptRejected_.store(0);
    udpToken_.fill(0);
    {
        std::scoped_lock lock(peersMutex_);
        peers_.clear();
        PublishPeerSnapshotLocked();
    }
    running_.store(true);
    SetState(ConnectionState::Connecting, "Sinyalizasyon baglandi; UDP kaydi bekleniyor");
    tcpThread_ = std::thread(&NetworkClient::TcpLoop, this);
    udpThread_ = std::thread(&NetworkClient::UdpLoop, this);

    const nlohmann::json join{{"type", "join"}, {"nickname", nickname}, {"room", room},
                              {"serverDenoise", serverDenoiseRequested}};
    if (!SendJson(join.dump())) {
        error = "Katilim mesaji gonderilemedi";
        Disconnect();
        return false;
    }
    return true;
}

bool NetworkClient::InitializeSecureCipher(const std::span<const std::uint8_t> key, std::string& error) {
    if (key.size() != 32) { error = "Gecersiz UDP anahtari"; return false; }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, L"CHACHA20_POLY1305", nullptr, 0);
    if (status < 0) { error = "Windows ChaCha20-Poly1305 destegi bulunamadi"; return false; }
    DWORD objectBytes = 0; DWORD written = 0;
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &written, 0);
    if (status < 0 || objectBytes == 0) { BCryptCloseAlgorithmProvider(algorithm, 0); error = "Sifre nesnesi olusturulamadi"; return false; }
    secureKeyObject_.assign(objectBytes, 0);
    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    status = BCryptGenerateSymmetricKey(algorithm, &keyHandle, secureKeyObject_.data(), objectBytes,
                                        const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0);
    if (status < 0) { BCryptCloseAlgorithmProvider(algorithm, 0); secureKeyObject_.clear(); error = "UDP anahtari yuklenemedi"; return false; }
    secureAlgorithm_ = algorithm; secureKey_ = keyHandle; return true;
}

bool NetworkClient::InitializeP2PCipher(const std::span<const std::uint8_t> key) noexcept {
    try {
        if (key.size() != 32) return false;
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        if (BCryptOpenAlgorithmProvider(&algorithm, L"CHACHA20_POLY1305", nullptr, 0) < 0) return false;
        DWORD objectBytes = 0; DWORD written = 0;
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes),
                              sizeof(objectBytes), &written, 0) < 0 || objectBytes == 0) {
            BCryptCloseAlgorithmProvider(algorithm, 0); return false;
        }
        std::vector<std::uint8_t> keyObject(objectBytes, 0);
        BCRYPT_KEY_HANDLE keyHandle = nullptr;
        if (BCryptGenerateSymmetricKey(algorithm, &keyHandle, keyObject.data(), objectBytes,
                                       const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm, 0); return false;
        }
        std::scoped_lock lock(p2pCipherMutex_);
        if (p2pKey_ != nullptr) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(p2pKey_));
        if (p2pAlgorithm_ != nullptr) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(p2pAlgorithm_), 0);
        p2pAlgorithm_ = algorithm; p2pKey_ = keyHandle; p2pKeyObject_ = std::move(keyObject);
        return true;
    } catch (...) { return false; }
}

void NetworkClient::DestroySecureCipher() noexcept {
    std::scoped_lock lock(secureCipherMutex_);
    if (secureKey_ != nullptr) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(secureKey_));
    if (secureAlgorithm_ != nullptr) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(secureAlgorithm_), 0);
    secureKey_ = nullptr; secureAlgorithm_ = nullptr; secureKeyObject_.clear();
}

void NetworkClient::DestroyP2PCipher() noexcept {
    std::scoped_lock lock(p2pCipherMutex_);
    if (p2pKey_ != nullptr) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(p2pKey_));
    if (p2pAlgorithm_ != nullptr) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(p2pAlgorithm_), 0);
    p2pKey_ = nullptr; p2pAlgorithm_ = nullptr; p2pKeyObject_.clear();
}

void NetworkClient::ResetP2P(const bool clearKeyAgreement) noexcept {
    p2pConfigured_.store(false);
    p2pReady_.store(false);
    p2pUsesIce_.store(false);
    p2pLastSeenMs_.store(0);
    p2pConfiguredAtMs_.store(0);
    { std::scoped_lock lock(p2pMutex_); p2pPeerUserId_.clear(); p2pPeerId_ = 0; p2pReceivedAny_ = false;
      p2pHighestIncoming_ = 0; p2pReplayMask_ = 0; p2pOutgoingSequence_.store(0); p2pPairId_.fill(0);
      p2pOutgoingPrefix_.fill(0); p2pIncomingPrefix_.fill(0); p2pEndpoint_ = {};
      p2pIcePairId_.fill(0); p2pIceDescription_.clear(); p2pIceReceived_.clear(); p2pIceReceivedBytes_ = 0;
      p2pIceRemoteApplied_ = false; }
    DestroyP2PCipher();
    {
        std::scoped_lock lock(voiceLifecycleMutex_);
        (void)voiceLifecycle_.PeerProbeFailed(sonalis::core::VoiceSession::Clock::now());
    }
    if (clearKeyAgreement) {
        crypto_wipe(p2pSecret_.data(), p2pSecret_.size());
        p2pPublic_.fill(0);
        p2pKeyAgreementReady_ = false;
        p2pAllowed_.store(false);
        iceTransport_.Stop();
    }
}

bool NetworkClient::PrepareP2PKeyAgreement() noexcept {
    try {
        if (BCryptGenRandom(nullptr, p2pSecret_.data(), static_cast<ULONG>(p2pSecret_.size()),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return false;
        crypto_x25519_public_key(p2pPublic_.data(), p2pSecret_.data());
        p2pKeyAgreementReady_ = std::ranges::any_of(p2pPublic_, [](const std::uint8_t value) { return value != 0; });
        if (!p2pKeyAgreementReady_) crypto_wipe(p2pSecret_.data(), p2pSecret_.size());
        return p2pKeyAgreementReady_;
    } catch (...) {
        crypto_wipe(p2pSecret_.data(), p2pSecret_.size());
        p2pPublic_.fill(0);
        p2pKeyAgreementReady_ = false;
        return false;
    }
}

bool NetworkClient::DeriveP2PKey(const std::span<const std::uint8_t> peerPublicKey,
                                 const std::span<const std::uint8_t> pairId,
                                 std::array<std::uint8_t, 32>& key) noexcept {
    if (!p2pKeyAgreementReady_ || peerPublicKey.size() != 32 || pairId.size() != 16) return false;
    std::array<std::uint8_t, 32> shared{};
    crypto_x25519(shared.data(), p2pSecret_.data(), peerPublicKey.data());
    const bool valid = std::ranges::any_of(shared, [](const std::uint8_t value) { return value != 0; });
    if (valid) {
        std::array<std::uint8_t, 36> context{};
        constexpr std::string_view label = "Sonalis-voice-p2p-v2";
        std::copy(label.begin(), label.end(), context.begin());
        std::copy(pairId.begin(), pairId.end(), context.begin() + static_cast<std::ptrdiff_t>(label.size()));
        crypto_blake2b_keyed(key.data(), key.size(), shared.data(), shared.size(), context.data(), context.size());
    }
    crypto_wipe(shared.data(), shared.size());
    return valid;
}

bool NetworkClient::ConnectSecure(const VoiceGrant& voiceGrant, const std::vector<PlatformMember>& members,
                                  const bool p2pEnabled, std::string& error) {
    Disconnect();
    if (!winsockReady_ || voiceGrant.host.empty() || voiceGrant.grant.empty()
        || voiceGrant.certificateFingerprint.size() != 64) {
        error = "Gecersiz ses katilim bileti veya TLS pini"; return false;
    }
    try {
        {
            std::scoped_lock lock(voiceLifecycleMutex_);
            sonalis::core::VoicePolicy policy;
            policy.peerToPeerEnabled = p2pEnabled;
            policy.serverDenoiseEnabled = voiceGrant.serverDenoise;
            policy.probeTimeout = std::chrono::seconds(3);
            voiceLifecycle_.SetPolicy(policy, sonalis::core::VoiceSession::Clock::now());
            voiceLifecycle_.Connected(sonalis::core::VoiceSession::Clock::now());
            (void)voiceLifecycle_.ParticipantsChanged(1, sonalis::core::VoiceSession::Clock::now());
        }
        p2pAllowed_.store(p2pEnabled && PrepareP2PKeyAgreement());
        std::string iceError;
        if (p2pAllowed_.load() && !voiceGrant.stunHost.empty()) {
            if (!iceTransport_.Start(voiceGrant.stunHost, voiceGrant.stunPort,
                [this](const std::span<const std::uint8_t> packet) { HandleP2PPacket(packet, nullptr); }, iceError)) {
                DiagnosticLog("voice-p2p", "ice-disabled=" + iceError);
            }
        }
        HttpClient http;
        const std::string joinUrl = "https://" + voiceGrant.host + ":" + std::to_string(voiceGrant.port) + "/join";
        nlohmann::json join{{"grant", voiceGrant.grant}};
        if (p2pAllowed_.load()) {
            join["p2pKeyAgreement"] = "x25519-v1";
            join["p2pPublicKey"] = EncodeBase64(p2pPublic_);
            const std::string iceDescription = iceTransport_.LocalDescription();
            if (!iceDescription.empty()) join["p2pIceDescription"] = iceDescription;
        }
        const std::string joinBody = join.dump();
        HttpResponse response;
        for (int attempt = 0; attempt < 3; ++attempt) {
            response = http.RequestPinnedVoiceNode(
                L"POST", joinUrl, voiceGrant.certificateFingerprint, joinBody);
            if (response.status == 200) break;
            std::string nodeError;
            try { nodeError = nlohmann::json::parse(response.body).value("error", ""); } catch (...) {}
            DiagnosticLog("voice-node", "join-http=" + std::to_string(response.status) + " error=" + nodeError);
            if (response.status != 403 || nodeError != "room_lease_inactive" || attempt == 2) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(150 * (attempt + 1)));
        }
        DiagnosticLog("voice-node", "join-http=" + std::to_string(response.status));
        if (response.status != 200) { error = "Ses dugumu katilimi reddetti"; return false; }
        const auto json = nlohmann::json::parse(response.body);
        const bool nodeServerDenoise = json.value("serverDenoise", false);
        std::vector<std::uint8_t> key; std::vector<std::uint8_t> prefix;
        if (!ParseUuid(json.value("sessionId", ""), secureSessionId_) || !DecodeBase64(json.value("udpKey", ""), key)
            || !DecodeBase64(json.value("noncePrefix", ""), prefix) || prefix.size() != secureNoncePrefix_.size()) {
            error = "Ses dugumu sifre oturumu gecersiz"; return false;
        }
        std::copy(prefix.begin(), prefix.end(), secureNoncePrefix_.begin());
        if (!InitializeSecureCipher(key, error)) return false;

        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM; hints.ai_protocol = IPPROTO_UDP;
        addrinfo* result = nullptr; const std::string port = std::to_string(voiceGrant.port);
        if (getaddrinfo(voiceGrant.host.c_str(), port.c_str(), &hints, &result) != 0 || result == nullptr) throw std::runtime_error("Ses dugumu adresi cozumlenemedi");
        const SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udp == INVALID_SOCKET) { freeaddrinfo(result); throw std::runtime_error(WinsockError("UDP soketi acilamadi")); }
        DWORD timeout = 500; setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_ANY); local.sin_port = 0;
        if (bind(udp, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
            freeaddrinfo(result); closesocket(udp); throw std::runtime_error(WinsockError("UDP soketi baglanamadi"));
        }
        if (result->ai_addrlen != sizeof(sockaddr_in)) { freeaddrinfo(result); closesocket(udp); throw std::runtime_error("Ses dugumu IPv4 adresi gecersiz"); }
        std::memcpy(&secureServerEndpoint_, result->ai_addr, sizeof(secureServerEndpoint_));
        freeaddrinfo(result);
        udpSocket_.store(udp); secureOutgoingSequence_.store(0); secureHighestIncoming_ = 0; secureReplayMask_ = 0; secureReceivedAny_ = false;
        secureBindAcknowledged_.store(false);
        secureMode_.store(true); serverDenoiseAvailable_.store(nodeServerDenoise); serverDenoiseEnabled_.store(nodeServerDenoise);
        serverDenoiseRequested_.store(voiceGrant.serverDenoise); audioBytesSent_.store(0); audioPacketsSent_.store(0);
        audioPacketsRejected_.store(0); audioPacketsReceived_.store(0); decryptRejected_.store(0);
        { std::scoped_lock lock(peersMutex_); peers_.clear(); securePeerNames_.clear();
          for (const auto& member : members) securePeerNames_[member.id] = member.nickname;
          PublishPeerSnapshotLocked(); }
        running_.store(true); SetState(ConnectionState::Connecting, "Sifreli UDP kaydi yapiliyor");
        udpThread_ = std::thread(&NetworkClient::SecureUdpLoop, this);
        const std::array<std::uint8_t, 1> bind{0x01};
        bool acknowledged = false;
        for (int attempt = 0; attempt < 10 && running_.load(); ++attempt) {
            if (!SendSecure(bind, 0x01)) throw std::runtime_error("Sifreli UDP kaydi gonderilemedi");
            std::unique_lock lock(secureBindMutex_);
            acknowledged = secureBindCv_.wait_for(lock, std::chrono::milliseconds(300), [this] { return secureBindAcknowledged_.load() || !running_.load(); });
            if (acknowledged && secureBindAcknowledged_.load()) break;
        }
        if (!acknowledged || !secureBindAcknowledged_.load()) throw std::runtime_error("Sifreli UDP kaydi zaman asimina ugradi");
        SetState(ConnectionState::Connected, "Bagli - sifreli UDP v3");
        DiagnosticLog("voice-node", "connected");
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        DiagnosticLog("voice-node", "exception=" + error);
        Disconnect();
        return false;
    }
}

void NetworkClient::Disconnect() {
    running_.store(false);
    secureBindCv_.notify_all();
    const SOCKET tcp = tcpSocket_.exchange(INVALID_SOCKET);
    if (tcp != INVALID_SOCKET) {
        shutdown(tcp, SD_BOTH);
        closesocket(tcp);
    }
    const SOCKET udp = udpSocket_.exchange(INVALID_SOCKET);
    if (udp != INVALID_SOCKET) {
        shutdown(udp, SD_BOTH);
        closesocket(udp);
    }
    const auto current = std::this_thread::get_id();
    if (tcpThread_.joinable() && tcpThread_.get_id() != current) tcpThread_.join();
    if (udpThread_.joinable() && udpThread_.get_id() != current) udpThread_.join();
    secureMode_.store(false);
    echoNonce_.store(0); echoSentAtMs_.store(0); echoRttMs_.store(-1);
    ResetP2P(true);
    DestroySecureCipher();
    {
        std::scoped_lock lock(voiceLifecycleMutex_);
        voiceLifecycle_.Disconnected();
    }

    std::vector<std::uint32_t> removed;
    {
        std::scoped_lock lock(peersMutex_);
        for (const auto& peer : peers_) removed.push_back(peer.peerId);
        peers_.clear();
        securePeerNames_.clear();
        PublishPeerSnapshotLocked();
    }
    PeerRemovedCallback callback;
    {
        std::scoped_lock lock(callbackMutex_);
        callback = peerRemovedCallback_;
    }
    if (callback) {
        for (const auto peerId : removed) callback(peerId);
    }
    localPeerId_.store(0);
    serverDenoiseAvailable_.store(false);
    serverDenoiseEnabled_.store(false);
    SetState(ConnectionState::Disconnected, "Bagli degil");
}

bool NetworkClient::SendJson(const std::string& jsonText) noexcept {
    try {
        const auto frame = protocol::EncodeJsonFrame(jsonText);
        const SOCKET socket = tcpSocket_.load();
        if (frame.empty() || socket == INVALID_SOCKET) return false;
        std::scoped_lock lock(tcpSendMutex_);
        return SendAll(socket, frame.data(), frame.size());
    } catch (...) {
        return false;
    }
}

bool NetworkClient::SendRegister() noexcept {
    std::array<std::uint8_t, protocol::kRegisterBytes> packet{};
    const auto size = protocol::BuildRegister(packet, udpToken_);
    const SOCKET socket = udpSocket_.load();
    if (size == 0 || socket == INVALID_SOCKET) return false;
    std::scoped_lock lock(udpSendMutex_);
    return send(socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(size), 0) == static_cast<int>(size);
}

bool NetworkClient::SendKeepAlive() noexcept {
    std::array<std::uint8_t, protocol::kRegisterBytes> packet{};
    const auto size = protocol::BuildKeepAlive(packet, udpToken_);
    const SOCKET socket = udpSocket_.load();
    if (size == 0 || socket == INVALID_SOCKET) return false;
    std::scoped_lock lock(udpSendMutex_);
    return send(socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(size), 0) == static_cast<int>(size);
}

bool NetworkClient::SendAudio(const std::uint16_t sequence,
                              const std::uint32_t timestamp,
                              const std::span<const std::uint8_t> opus,
                              const std::uint8_t flags) noexcept {
    if (state_.load() != ConnectionState::Connected) {
        audioPacketsRejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (secureMode_.load()) {
        if (opus.empty() || opus.size() > protocol::kMaxOpusBytes) {
            audioPacketsRejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (p2pReady_.load() && CoreAllowsPeerRoute()) {
            const std::uint64_t lastSeen = p2pLastSeenMs_.load();
            if (lastSeen != 0 && SteadyMilliseconds() - lastSeen <= 2'000) {
                std::array<std::uint8_t, 1500> direct{}; direct[0] = 0x03;
                WriteBig16(direct.data() + 1, sequence); WriteBig32(direct.data() + 3, timestamp); direct[7] = flags;
                std::copy(opus.begin(), opus.end(), direct.begin() + 8);
                if (SendP2P(std::span<const std::uint8_t>(direct.data(), 8 + opus.size()))) return true;
            }
            p2pReady_.store(false);
        }
        std::array<std::uint8_t, 1500> plaintext{}; plaintext[0] = 0x02;
        WriteBig16(plaintext.data() + 1, sequence); WriteBig32(plaintext.data() + 3, timestamp); plaintext[7] = flags;
        std::copy(opus.begin(), opus.end(), plaintext.begin() + 8);
        const bool sent = SendSecure(std::span<const std::uint8_t>(plaintext.data(), 8 + opus.size()), 0x02);
        if (!sent) audioPacketsRejected_.fetch_add(1, std::memory_order_relaxed);
        return sent;
    }
    std::array<std::uint8_t, protocol::kMaxDatagramBytes> packet{};
    const auto size = protocol::BuildAudioUp(packet, udpToken_, sequence, timestamp, flags, opus);
    const SOCKET socket = udpSocket_.load();
    if (size == 0 || socket == INVALID_SOCKET) {
        audioPacketsRejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::scoped_lock lock(udpSendMutex_);
    const bool sent = send(socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(size), 0) == static_cast<int>(size);
    if (sent) {
        audioBytesSent_.fetch_add(size);
        audioPacketsSent_.fetch_add(1);
    } else audioPacketsRejected_.fetch_add(1, std::memory_order_relaxed);
    return sent;
}

bool NetworkClient::SendSecure(const std::span<const std::uint8_t> plaintext, const std::uint8_t flags) noexcept {
    try {
        const SOCKET socket = udpSocket_.load(); if (socket == INVALID_SOCKET || secureKey_ == nullptr || plaintext.size() > 1400) return false;
        std::scoped_lock sendLock(udpSendMutex_); const std::uint64_t sequence = secureOutgoingSequence_.fetch_add(1) + 1;
        std::array<std::uint8_t, 1500> packet{}; const std::size_t packetBytes = 28 + plaintext.size() + 16;
        packet[0] = 0x53; packet[1] = 0x53; packet[2] = 3; packet[3] = flags;
        std::copy(secureSessionId_.begin(), secureSessionId_.end(), packet.begin() + 4); WriteBig64(packet.data() + 20, sequence);
        std::array<std::uint8_t, 12> nonce{}; std::copy(secureNoncePrefix_.begin(), secureNoncePrefix_.end(), nonce.begin()); WriteBig64(nonce.data() + 4, sequence);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info; BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce.data(); info.cbNonce = static_cast<ULONG>(nonce.size()); info.pbAuthData = packet.data(); info.cbAuthData = 28;
        info.pbTag = packet.data() + 28 + plaintext.size(); info.cbTag = 16; ULONG encrypted = 0;
        { std::scoped_lock cipherLock(secureCipherMutex_);
          const NTSTATUS status = BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(secureKey_), const_cast<PUCHAR>(plaintext.data()), static_cast<ULONG>(plaintext.size()),
              &info, nullptr, 0, packet.data() + 28, static_cast<ULONG>(plaintext.size()), &encrypted, 0);
          if (status < 0 || encrypted != plaintext.size()) return false; }
        const bool sent = sendto(socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packetBytes), 0,
                                 reinterpret_cast<const sockaddr*>(&secureServerEndpoint_), sizeof(secureServerEndpoint_))
            == static_cast<int>(packetBytes);
        if (sent && plaintext.size() > 1) { audioBytesSent_.fetch_add(packetBytes); audioPacketsSent_.fetch_add(1); }
        return sent;
    } catch (...) { return false; }
}

void NetworkClient::ConfigureP2P(const std::span<const std::uint8_t> plaintext) {
    if (plaintext.size() < 2 || plaintext[0] != 0x14) return;
    if (plaintext[1] == 0x00) { ResetP2P(); return; }
    const std::uint8_t mode = plaintext[1];
    if ((mode != 0x01 && mode != 0x02 && mode != 0x03) || plaintext.size() < 63 || !p2pAllowed_.load()) return;
    SetCoreParticipants(2);
    std::size_t offset = 2;
    std::array<std::uint8_t, 16> pairId{}; std::copy_n(plaintext.begin() + static_cast<std::ptrdiff_t>(offset), pairId.size(), pairId.begin()); offset += pairId.size();
    std::array<std::uint8_t, 32> keyMaterial{}; std::copy_n(plaintext.begin() + static_cast<std::ptrdiff_t>(offset), keyMaterial.size(), keyMaterial.begin()); offset += keyMaterial.size();
    std::array<std::uint8_t, 4> outgoingPrefix{}; std::copy_n(plaintext.begin() + static_cast<std::ptrdiff_t>(offset), outgoingPrefix.size(), outgoingPrefix.begin()); offset += outgoingPrefix.size();
    std::array<std::uint8_t, 4> incomingPrefix{}; std::copy_n(plaintext.begin() + static_cast<std::ptrdiff_t>(offset), incomingPrefix.size(), incomingPrefix.begin()); offset += incomingPrefix.size();
    if (offset >= plaintext.size()) return;
    const std::size_t userBytes = plaintext[offset++];
    if (userBytes == 0 || userBytes > 64 || offset + userBytes + 1 > plaintext.size()) return;
    const std::string peerUser(reinterpret_cast<const char*>(plaintext.data() + offset), userBytes); offset += userBytes;
    if (mode == 0x03) {
        if (offset + 4 > plaintext.size()) return;
        const std::size_t total = ReadBig16(plaintext.data() + offset); offset += 2;
        const std::size_t fragmentOffset = ReadBig16(plaintext.data() + offset); offset += 2;
        const std::size_t fragmentBytes = plaintext.size() - offset;
        if (total == 0 || total >= JUICE_MAX_SDP_STRING_LEN || fragmentBytes == 0 || fragmentOffset + fragmentBytes > total) return;
        bool newPair = false;
        {
            std::scoped_lock lock(p2pMutex_);
            newPair = p2pIcePairId_ != pairId || p2pIceDescription_.size() != total;
        }
        if (newPair) {
            p2pConfigured_.store(false);
            p2pReady_.store(false);
            std::array<std::uint8_t, 32> key{};
            if (!DeriveP2PKey(keyMaterial, pairId, key) || !InitializeP2PCipher(key)) {
                crypto_wipe(key.data(), key.size()); ResetP2P(); return;
            }
            crypto_wipe(key.data(), key.size());
            std::scoped_lock lock(p2pMutex_);
            p2pPairId_ = pairId; p2pIcePairId_ = pairId; p2pOutgoingPrefix_ = outgoingPrefix;
            p2pIncomingPrefix_ = incomingPrefix; p2pPeerUserId_ = peerUser; p2pPeerId_ = PeerIdFor(peerUser);
            p2pOutgoingSequence_.store(0); p2pHighestIncoming_ = 0; p2pReplayMask_ = 0; p2pReceivedAny_ = false;
            p2pIceDescription_.assign(total, 0); p2pIceReceived_.assign(total, false); p2pIceReceivedBytes_ = 0;
            p2pIceRemoteApplied_ = false;
        }
        std::string remoteDescription;
        {
            std::scoped_lock lock(p2pMutex_);
            for (std::size_t index = 0; index < fragmentBytes; ++index) {
                const std::size_t destination = fragmentOffset + index;
                p2pIceDescription_[destination] = plaintext[offset + index];
                if (!p2pIceReceived_[destination]) { p2pIceReceived_[destination] = true; ++p2pIceReceivedBytes_; }
            }
            if (p2pIceReceivedBytes_ == p2pIceDescription_.size() && !p2pIceRemoteApplied_) {
                p2pIceRemoteApplied_ = true;
                remoteDescription.assign(reinterpret_cast<const char*>(p2pIceDescription_.data()), p2pIceDescription_.size());
            }
        }
        if (remoteDescription.empty()) return;
        std::string iceError;
        if (!iceTransport_.SetRemoteDescription(remoteDescription, iceError)) {
            { std::scoped_lock lock(p2pMutex_); p2pIceRemoteApplied_ = false; }
            DiagnosticLog("voice-p2p", "remote-ice=" + iceError); ResetP2P(); return;
        }
        p2pUsesIce_.store(true); p2pLastSeenMs_.store(0);
        p2pConfiguredAtMs_.store(SteadyMilliseconds()); p2pConfigured_.store(true);
        const std::array<std::uint8_t, 1> probe{0x01}; SendP2P(probe);
        return;
    }
    const std::size_t addressBytes = plaintext[offset++];
    if (addressBytes == 0 || addressBytes > 64 || offset + addressBytes + 2 != plaintext.size()) return;
    const std::string address(reinterpret_cast<const char*>(plaintext.data() + offset), addressBytes); offset += addressBytes;
    const unsigned short port = ReadBig16(plaintext.data() + offset);
    sockaddr_in endpoint{}; endpoint.sin_family = AF_INET; endpoint.sin_port = htons(port);
    if (port == 0 || inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1) return;
    {
        std::scoped_lock lock(p2pMutex_);
        if (p2pConfigured_.load() && p2pPairId_ == pairId) {
            p2pEndpoint_ = endpoint;
            return;
        }
    }
    p2pConfigured_.store(false);
    p2pReady_.store(false);
    std::array<std::uint8_t, 32> key{};
    if (mode == 0x01) key = keyMaterial;
    else if (!DeriveP2PKey(keyMaterial, pairId, key)) { ResetP2P(); return; }
    if (!InitializeP2PCipher(key)) { ResetP2P(); return; }
    crypto_wipe(key.data(), key.size());
    {
        std::scoped_lock lock(p2pMutex_);
        p2pPairId_ = pairId; p2pOutgoingPrefix_ = outgoingPrefix; p2pIncomingPrefix_ = incomingPrefix;
        p2pEndpoint_ = endpoint; p2pPeerUserId_ = peerUser; p2pPeerId_ = PeerIdFor(peerUser);
        p2pOutgoingSequence_.store(0); p2pHighestIncoming_ = 0; p2pReplayMask_ = 0; p2pReceivedAny_ = false;
    }
    {
        std::scoped_lock lock(peersMutex_);
        const auto peerId = PeerIdFor(peerUser);
        const auto existing = std::ranges::find_if(peers_, [&](const PeerInfo& peer) { return peer.peerId == peerId; });
        if (existing == peers_.end()) {
            const auto named = securePeerNames_.find(peerUser);
            peers_.push_back({peerId, named == securePeerNames_.end() ? peerUser : named->second});
            PublishPeerSnapshotLocked();
        }
    }
    p2pLastSeenMs_.store(0);
    p2pConfiguredAtMs_.store(SteadyMilliseconds());
    p2pUsesIce_.store(false);
    p2pConfigured_.store(true);
    const std::array<std::uint8_t, 1> probe{0x01};
    SendP2P(probe);
}

bool NetworkClient::SendP2P(const std::span<const std::uint8_t> plaintext) noexcept {
    try {
        const SOCKET socket = udpSocket_.load();
        if (!p2pConfigured_.load() || socket == INVALID_SOCKET || plaintext.empty() || plaintext.size() > 1400) return false;
        std::array<std::uint8_t, 16> pairId{}; std::array<std::uint8_t, 4> prefix{}; sockaddr_in endpoint{};
        { std::scoped_lock lock(p2pMutex_); pairId = p2pPairId_; prefix = p2pOutgoingPrefix_; endpoint = p2pEndpoint_; }
        const std::uint64_t sequence = p2pOutgoingSequence_.fetch_add(1) + 1;
        std::array<std::uint8_t, 1500> packet{}; const std::size_t packetBytes = 28 + plaintext.size() + 16;
        packet[0] = 'S'; packet[1] = 'S'; packet[2] = 'P'; packet[3] = '4';
        std::copy(pairId.begin(), pairId.end(), packet.begin() + 4); WriteBig64(packet.data() + 20, sequence);
        std::array<std::uint8_t, 12> nonce{}; std::copy(prefix.begin(), prefix.end(), nonce.begin()); WriteBig64(nonce.data() + 4, sequence);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info; BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce.data(); info.cbNonce = static_cast<ULONG>(nonce.size()); info.pbAuthData = packet.data(); info.cbAuthData = 28;
        info.pbTag = packet.data() + 28 + plaintext.size(); info.cbTag = 16; ULONG encrypted = 0;
        { std::scoped_lock lock(p2pCipherMutex_);
          if (p2pKey_ == nullptr || BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(p2pKey_), const_cast<PUCHAR>(plaintext.data()), static_cast<ULONG>(plaintext.size()),
              &info, nullptr, 0, packet.data() + 28, static_cast<ULONG>(plaintext.size()), &encrypted, 0) < 0 || encrypted != plaintext.size()) return false; }
        bool sent = false;
        if (p2pUsesIce_.load()) {
            sent = iceTransport_.Send(std::span<const std::uint8_t>(packet.data(), packetBytes));
        } else {
            std::scoped_lock sendLock(udpSendMutex_);
            sent = sendto(socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packetBytes), 0,
                          reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == static_cast<int>(packetBytes);
        }
        if (sent && plaintext[0] == 0x03) { audioBytesSent_.fetch_add(packetBytes); audioPacketsSent_.fetch_add(1); }
        return sent;
    } catch (...) { return false; }
}

bool NetworkClient::HandleP2PPacket(const std::span<const std::uint8_t> packet, const sockaddr_in* remote) {
    if (!p2pConfigured_.load() || packet.size() < 45 || packet.size() > 1500
        || packet[0] != 'S' || packet[1] != 'S' || packet[2] != 'P' || packet[3] != '4') return false;
    std::array<std::uint8_t, 16> pairId{}; std::array<std::uint8_t, 4> prefix{}; sockaddr_in expected{};
    { std::scoped_lock lock(p2pMutex_); pairId = p2pPairId_; prefix = p2pIncomingPrefix_; expected = p2pEndpoint_; }
    if ((!p2pUsesIce_.load() && (remote == nullptr || !SameEndpoint(*remote, expected)))
        || !std::equal(pairId.begin(), pairId.end(), packet.begin() + 4)) return false;
    const std::uint64_t sequence = ReadBig64(packet.data() + 20);
    {
        std::scoped_lock lock(p2pMutex_);
        if (p2pReceivedAny_ && sequence <= p2pHighestIncoming_) {
            const auto delta = p2pHighestIncoming_ - sequence;
            if (delta >= 64 || (p2pReplayMask_ & (1ULL << delta)) != 0) return true;
        }
    }
    const std::size_t cipherBytes = packet.size() - 44;
    std::array<std::uint8_t, 12> nonce{}; std::copy(prefix.begin(), prefix.end(), nonce.begin()); WriteBig64(nonce.data() + 4, sequence);
    std::array<std::uint8_t, 1500> plaintext{}; BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info; BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce.data(); info.cbNonce = static_cast<ULONG>(nonce.size()); info.pbAuthData = const_cast<PUCHAR>(packet.data()); info.cbAuthData = 28;
    info.pbTag = const_cast<PUCHAR>(packet.data() + 28 + cipherBytes); info.cbTag = 16; ULONG decrypted = 0;
    NTSTATUS status = 0;
    { std::scoped_lock lock(p2pCipherMutex_);
      if (p2pKey_ == nullptr) return false;
      status = BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(p2pKey_), const_cast<PUCHAR>(packet.data() + 28), static_cast<ULONG>(cipherBytes),
          &info, nullptr, 0, plaintext.data(), static_cast<ULONG>(cipherBytes), &decrypted, 0); }
    if (status < 0 || decrypted != cipherBytes) {
        decryptRejected_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    {
        std::scoped_lock lock(p2pMutex_);
        if (!p2pReceivedAny_) { p2pReceivedAny_ = true; p2pHighestIncoming_ = sequence; p2pReplayMask_ = 1; }
        else if (sequence > p2pHighestIncoming_) { const auto shift = sequence - p2pHighestIncoming_;
            p2pReplayMask_ = shift >= 64 ? 1 : ((p2pReplayMask_ << shift) | 1); p2pHighestIncoming_ = sequence; }
        else p2pReplayMask_ |= 1ULL << (p2pHighestIncoming_ - sequence);
    }
    p2pLastSeenMs_.store(SteadyMilliseconds()); p2pReady_.store(true);
    {
        std::scoped_lock lock(voiceLifecycleMutex_);
        (void)voiceLifecycle_.PeerProbeSucceeded(sonalis::core::VoiceSession::Clock::now());
    }
    if (cipherBytes == 1 && plaintext[0] == 0x01) { const std::array<std::uint8_t, 1> ack{0x02}; SendP2P(ack); return true; }
    if (cipherBytes == 1 && plaintext[0] == 0x02) return true;
    if (cipherBytes < 8 || plaintext[0] != 0x03) return true;
    std::uint32_t peerId = 0;
    { std::scoped_lock lock(p2pMutex_); peerId = p2pPeerId_; }
    AudioCallback callback; { std::scoped_lock lock(callbackMutex_); callback = audioCallback_; }
    if (callback && peerId != 0) {
        audioPacketsReceived_.fetch_add(1, std::memory_order_relaxed);
        callback(peerId, ReadBig16(plaintext.data() + 1), ReadBig32(plaintext.data() + 3), plaintext[7],
                 std::span<const std::uint8_t>(plaintext.data() + 8, cipherBytes - 8));
    }
    return true;
}

void NetworkClient::SecureUdpLoop() {
    std::array<std::uint8_t, 1500> incoming{}; auto lastHeartbeat = Clock::now(); auto lastP2PProbe = Clock::now() - std::chrono::seconds(2);
    while (running_.load() && secureMode_.load()) {
        const auto now = Clock::now();
        if (now - lastHeartbeat >= std::chrono::seconds(5)) { const std::array<std::uint8_t, 1> heartbeat{0x01}; SendSecure(heartbeat, 0x01); lastHeartbeat = now; }
        if (p2pConfigured_.load() && now - lastP2PProbe >= std::chrono::milliseconds(500)) {
            const std::uint64_t configuredAt = p2pConfiguredAtMs_.load(std::memory_order_relaxed);
            if (!p2pReady_.load(std::memory_order_relaxed) && configuredAt != 0
                && SteadyMilliseconds() - configuredAt >= 3'000) {
                DiagnosticLog("voice-p2p", "probe-timeout-relay-fallback");
                ResetP2P();
                continue;
            }
            const std::array<std::uint8_t, 1> probe{0x01}; SendP2P(probe); lastP2PProbe = now;
        }
        const SOCKET socket = udpSocket_.load(); if (socket == INVALID_SOCKET) break;
        sockaddr_in remote{}; int remoteBytes = sizeof(remote);
        const int received = recvfrom(socket, reinterpret_cast<char*>(incoming.data()), static_cast<int>(incoming.size()), 0,
                                      reinterpret_cast<sockaddr*>(&remote), &remoteBytes);
        if (received == SOCKET_ERROR) { const int code = WSAGetLastError(); if (code == WSAETIMEDOUT || code == WSAEWOULDBLOCK || code == WSAEINTR) continue; break; }
        const std::span<const std::uint8_t> receivedPacket(incoming.data(), static_cast<std::size_t>(received));
        if (!SameEndpoint(remote, secureServerEndpoint_)) {
            HandleP2PPacket(receivedPacket, &remote);
            continue;
        }
        if (received < 45 || incoming[0] != 0x53 || incoming[1] != 0x53 || incoming[2] != 3
            || !std::equal(secureSessionId_.begin(), secureSessionId_.end(), incoming.begin() + 4)) continue;
        const std::size_t cipherBytes = static_cast<std::size_t>(received) - 28 - 16; const std::uint64_t sequence = ReadBig64(incoming.data() + 20);
        { std::scoped_lock replayLock(secureReplayMutex_); if (secureReceivedAny_ && sequence <= secureHighestIncoming_) {
            const std::uint64_t delta = secureHighestIncoming_ - sequence; if (delta >= 64 || (secureReplayMask_ & (1ULL << delta)) != 0) continue; } }
        std::array<std::uint8_t, 12> nonce{}; std::copy(secureNoncePrefix_.begin(), secureNoncePrefix_.end(), nonce.begin()); WriteBig64(nonce.data() + 4, sequence);
        std::array<std::uint8_t, 1500> plaintext{}; BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info; BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce.data(); info.cbNonce = static_cast<ULONG>(nonce.size()); info.pbAuthData = incoming.data(); info.cbAuthData = 28;
        info.pbTag = incoming.data() + 28 + cipherBytes; info.cbTag = 16; ULONG decrypted = 0; NTSTATUS status = 0;
        { std::scoped_lock cipherLock(secureCipherMutex_); status = BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(secureKey_), incoming.data() + 28, static_cast<ULONG>(cipherBytes),
              &info, nullptr, 0, plaintext.data(), static_cast<ULONG>(cipherBytes), &decrypted, 0); }
        if (status < 0 || decrypted != cipherBytes) {
            decryptRejected_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        { std::scoped_lock replayLock(secureReplayMutex_); if (!secureReceivedAny_) { secureReceivedAny_ = true; secureHighestIncoming_ = sequence; secureReplayMask_ = 1; }
          else if (sequence > secureHighestIncoming_) { const auto shift = sequence - secureHighestIncoming_; secureReplayMask_ = shift >= 64 ? 1 : ((secureReplayMask_ << shift) | 1); secureHighestIncoming_ = sequence; }
          else secureReplayMask_ |= 1ULL << (secureHighestIncoming_ - sequence); }
        if (cipherBytes > 0 && plaintext[0] == 0x11) {
            secureBindAcknowledged_.store(true);
            secureBindCv_.notify_all();
            continue;
        }
        if (cipherBytes == 9 && plaintext[0] == 0x15) {
            const std::uint64_t echoedNonce = ReadBig64(plaintext.data() + 1);
            if (echoedNonce != 0 && echoedNonce == echoNonce_.load()) {
                const auto elapsed = SteadyMilliseconds() - echoSentAtMs_.load();
                echoRttMs_.store(static_cast<int>(std::min<std::uint64_t>(elapsed, 60'000)));
                echoNonce_.store(0);
                StateCallback callback; { std::scoped_lock lock(callbackMutex_); callback = stateCallback_; }
                if (callback) callback();
            }
            continue;
        }
        if (cipherBytes >= 2 && plaintext[0] == 0x13) {
            MarkTransportClosed(plaintext[1] == 0x01
                ? "Odada tek kullanici kaldigi icin ses oturumu kapatildi"
                : "Ses oturumu dugum tarafindan kapatildi");
            return;
        }
        if (cipherBytes >= 2 && plaintext[0] == 0x14) {
            ConfigureP2P(std::span<const std::uint8_t>(plaintext.data(), cipherBytes));
            continue;
        }
        if (cipherBytes < 10 || plaintext[0] != 0x12) continue; const std::size_t senderBytes = plaintext[1];
        const std::size_t metadata = 2 + senderBytes; if (senderBytes == 0 || metadata + 7 >= cipherBytes) continue;
        const std::string sender(reinterpret_cast<const char*>(plaintext.data() + 2), senderBytes); const std::uint32_t peerId = PeerIdFor(sender);
        { std::scoped_lock lock(peersMutex_); const auto found = std::find_if(peers_.begin(), peers_.end(), [peerId](const PeerInfo& item) { return item.peerId == peerId; });
          if (found == peers_.end()) { const auto named = securePeerNames_.find(sender); peers_.push_back({peerId, named == securePeerNames_.end() ? sender : named->second}); PublishPeerSnapshotLocked(); } }
        AudioCallback callback; { std::scoped_lock lock(callbackMutex_); callback = audioCallback_; }
        if (callback) {
            audioPacketsReceived_.fetch_add(1, std::memory_order_relaxed);
            callback(peerId, ReadBig16(plaintext.data() + metadata), ReadBig32(plaintext.data() + metadata + 2), plaintext[metadata + 6],
                     std::span<const std::uint8_t>(plaintext.data() + metadata + 7, cipherBytes - metadata - 7));
        }
    }
}

void NetworkClient::TcpLoop() {
    std::vector<std::uint8_t> pending;
    pending.reserve(32 * 1024);
    std::array<std::uint8_t, 4096> incoming{};
    while (running_.load()) {
        const SOCKET socket = tcpSocket_.load();
        if (socket == INVALID_SOCKET) break;
        const int received = recv(socket, reinterpret_cast<char*>(incoming.data()), static_cast<int>(incoming.size()), 0);
        if (received <= 0) {
            if (running_.load()) MarkTransportClosed("TCP baglantisi kapandi");
            break;
        }
        pending.insert(pending.end(), incoming.begin(), incoming.begin() + received);
        try {
            while (pending.size() >= 4) {
                const std::uint32_t length = (static_cast<std::uint32_t>(pending[0]) << 24U)
                    | (static_cast<std::uint32_t>(pending[1]) << 16U)
                    | (static_cast<std::uint32_t>(pending[2]) << 8U)
                    | pending[3];
                if (length == 0 || length > 16 * 1024) throw std::runtime_error("Gecersiz TCP cercevesi");
                if (pending.size() < static_cast<std::size_t>(length) + 4) break;
                const std::string jsonText(reinterpret_cast<const char*>(pending.data() + 4), length);
                pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(length + 4));
                HandleSignal(jsonText);
            }
            if (pending.size() > 16 * 1024 + 4) throw std::runtime_error("TCP tamponu tasmasi");
        } catch (const std::exception& exception) {
            MarkTransportClosed(exception.what());
            break;
        }
    }
}

void NetworkClient::UdpLoop() {
    std::array<std::uint8_t, protocol::kMaxDatagramBytes> incoming{};
    auto lastControl = Clock::now() - std::chrono::seconds(10);
    while (running_.load()) {
        const auto now = Clock::now();
        if (now - lastControl >= std::chrono::seconds(state_.load() == ConnectionState::Connected ? 5 : 1)) {
            if (localPeerId_.load() != 0) {
                if (state_.load() == ConnectionState::Connected) SendKeepAlive();
                else SendRegister();
            }
            lastControl = now;
        }

        const SOCKET socket = udpSocket_.load();
        if (socket == INVALID_SOCKET) break;
        const int received = recv(socket, reinterpret_cast<char*>(incoming.data()), static_cast<int>(incoming.size()), 0);
        if (received == SOCKET_ERROR) {
            const int code = WSAGetLastError();
            if (code == WSAETIMEDOUT || code == WSAEWOULDBLOCK || code == WSAEINTR) continue;
            if (running_.load()) MarkTransportClosed("UDP baglantisi kapandi");
            break;
        }
        if (received <= 0) continue;
        const std::span<const std::uint8_t> packet(incoming.data(), static_cast<std::size_t>(received));
        std::uint32_t acknowledgedPeerId = 0;
        if (protocol::ParseRegisterAck(packet, acknowledgedPeerId)) {
            if (acknowledgedPeerId == localPeerId_.load()) {
                SetState(ConnectionState::Connected, "Bagli");
            }
            continue;
        }
        if (protocol::ParseKeepAliveAck(packet)) continue;

        protocol::DownlinkAudioPacket audio{};
        if (protocol::ParseAudioDown(packet, audio)) {
            AudioCallback callback;
            {
                std::scoped_lock lock(callbackMutex_);
                callback = audioCallback_;
            }
            if (callback) {
                audioPacketsReceived_.fetch_add(1, std::memory_order_relaxed);
                callback(audio.peerId, audio.sequence, audio.timestamp, audio.flags, audio.opus);
            }
        }
    }
}

void NetworkClient::HandleSignal(const std::string& jsonText) {
    const nlohmann::json message = nlohmann::json::parse(jsonText);
    const std::string type = message.value("type", "");
    if (type == "welcome") {
        const std::uint32_t peerId = message.value("peerId", 0U);
        const std::string tokenText = message.value("udpToken", "");
        std::array<std::uint8_t, protocol::kTokenBytes> token{};
        if (peerId == 0 || !protocol::DecodeHexToken(tokenText, token)) throw std::runtime_error("Gecersiz welcome mesaji");
        udpToken_ = token;
        localPeerId_.store(peerId);
        const bool available = message.value("serverDenoiseAvailable", message.value("serverDenoise", false));
        serverDenoiseAvailable_.store(available);
        serverDenoiseEnabled_.store(message.value("serverDenoiseEnabled",
                                                   available && serverDenoiseRequested_.load()));
        std::vector<PeerInfo> updated;
        if (message.contains("participants") && message["participants"].is_array()) {
            for (const auto& item : message["participants"]) {
                const auto id = item.value("peerId", 0U);
                const auto nickname = item.value("nickname", "");
                if (id != 0 && !nickname.empty()) updated.push_back({id, nickname});
            }
        }
        {
            std::scoped_lock lock(peersMutex_);
            peers_ = std::move(updated);
            PublishPeerSnapshotLocked();
        }
        SendRegister();
    } else if (type == "peer_joined") {
        const auto id = message.value("peerId", 0U);
        const auto nickname = message.value("nickname", "");
        if (id != 0 && !nickname.empty()) {
            std::scoped_lock lock(peersMutex_);
            const auto found = std::find_if(peers_.begin(), peers_.end(), [id](const PeerInfo& peer) { return peer.peerId == id; });
            if (found == peers_.end()) { peers_.push_back({id, nickname}); PublishPeerSnapshotLocked(); }
        }
    } else if (type == "room_state") {
        const bool available = message.value("serverDenoiseAvailable", message.value("serverDenoise", false));
        serverDenoiseAvailable_.store(available);
        serverDenoiseEnabled_.store(message.value("serverDenoiseEnabled",
                                                   available && serverDenoiseRequested_.load()));
    } else if (type == "denoise_state") {
        serverDenoiseAvailable_.store(message.value("available", false));
        serverDenoiseRequested_.store(message.value("requested", serverDenoiseRequested_.load()));
        serverDenoiseEnabled_.store(message.value("enabled", false));
    } else if (type == "peer_left") {
        const auto id = message.value("peerId", 0U);
        {
            std::scoped_lock lock(peersMutex_);
            std::erase_if(peers_, [id](const PeerInfo& peer) { return peer.peerId == id; });
            PublishPeerSnapshotLocked();
        }
        PeerRemovedCallback callback;
        {
            std::scoped_lock lock(callbackMutex_);
            callback = peerRemovedCallback_;
        }
        if (callback && id != 0) callback(id);
    } else if (type == "ping") {
        const nlohmann::json pong{{"type", "pong"}, {"t", message.value("t", 0LL)}};
        SendJson(pong.dump());
    } else if (type == "error") {
        MarkTransportClosed("Sunucu hatasi: " + message.value("code", "unknown"));
    }
}

void NetworkClient::SetState(const ConnectionState state, std::string text) {
    state_.store(state);
    { std::scoped_lock lock(statusMutex_); statusText_ = std::move(text); }
    StateCallback callback;
    { std::scoped_lock lock(callbackMutex_); callback = stateCallback_; }
    if (callback) callback();
}

void NetworkClient::MarkTransportClosed(const std::string& reason) {
    running_.store(false);
    SetState(ConnectionState::Error, reason);
    const SOCKET tcp = tcpSocket_.exchange(INVALID_SOCKET);
    if (tcp != INVALID_SOCKET) {
        shutdown(tcp, SD_BOTH);
        closesocket(tcp);
    }
    const SOCKET udp = udpSocket_.exchange(INVALID_SOCKET);
    if (udp != INVALID_SOCKET) {
        shutdown(udp, SD_BOTH);
        closesocket(udp);
    }
}

ConnectionState NetworkClient::State() const noexcept { return state_.load(); }

std::string NetworkClient::StatusText() const {
    std::scoped_lock lock(statusMutex_);
    return statusText_;
}

std::vector<PeerInfo> NetworkClient::Peers() const {
    std::scoped_lock lock(peersMutex_);
    return peers_;
}

std::shared_ptr<const std::vector<PeerInfo>> NetworkClient::PeersSnapshot() const noexcept {
    return peerSnapshot_.load(std::memory_order_acquire);
}

void NetworkClient::PublishPeerSnapshotLocked() {
    peerSnapshot_.store(std::make_shared<const std::vector<PeerInfo>>(peers_), std::memory_order_release);
    SetCoreParticipants(peers_.size() + (running_.load() ? 1U : 0U));
}

void NetworkClient::SetCoreParticipants(const std::size_t activeParticipants) noexcept {
    std::scoped_lock lock(voiceLifecycleMutex_);
    (void)voiceLifecycle_.ParticipantsChanged(activeParticipants, sonalis::core::VoiceSession::Clock::now());
}

bool NetworkClient::CoreAllowsPeerRoute() const noexcept {
    std::scoped_lock lock(voiceLifecycleMutex_);
    return voiceLifecycle_.Route() == sonalis::core::VoiceRoute::PeerToPeer;
}

std::uint32_t NetworkClient::LocalPeerId() const noexcept { return localPeerId_.load(); }
bool NetworkClient::ServerDenoiseAvailable() const noexcept { return serverDenoiseAvailable_.load(); }
bool NetworkClient::ServerDenoiseEnabled() const noexcept { return serverDenoiseEnabled_.load(); }
std::uint64_t NetworkClient::AudioBytesSent() const noexcept { return audioBytesSent_.load(); }
std::uint64_t NetworkClient::AudioPacketsSent() const noexcept { return audioPacketsSent_.load(); }
NetworkDiagnosticsSnapshot NetworkClient::Diagnostics() const noexcept {
    return {
        audioPacketsSent_.load(std::memory_order_relaxed),
        audioPacketsRejected_.load(std::memory_order_relaxed),
        audioPacketsReceived_.load(std::memory_order_relaxed),
        decryptRejected_.load(std::memory_order_relaxed),
    };
}
bool NetworkClient::BeginEncryptedEchoTest() noexcept {
    if (!secureMode_.load() || state_.load() != ConnectionState::Connected) return false;
    if (echoNonce_.load() != 0) {
        const auto started = echoSentAtMs_.load();
        if (started == 0 || SteadyMilliseconds() - started <= 5'000) return false;
        echoNonce_.store(0);
    }
    std::array<std::uint8_t, 9> request{};
    request[0] = 0x04;
    std::uint64_t nonce = 0;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&nonce), sizeof(nonce),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 || nonce == 0) return false;
    WriteBig64(request.data() + 1, nonce);
    echoRttMs_.store(-1); echoSentAtMs_.store(SteadyMilliseconds()); echoNonce_.store(nonce);
    if (SendSecure(request, 0x06)) return true;
    echoNonce_.store(0); echoSentAtMs_.store(0);
    return false;
}

std::string NetworkClient::EchoTestStatus() const {
    if (echoNonce_.load() != 0) {
        const auto started = echoSentAtMs_.load();
        if (started != 0 && SteadyMilliseconds() - started > 5'000) {
            return "Zaman asimi: UDP/TLS veya guvenlik duvarini kontrol edin";
        }
        return "Sifreli UDP yaniti bekleniyor...";
    }
    const int rtt = echoRttMs_.load();
    return rtt >= 0 ? "Sifreli ses yolu calisiyor - RTT " + std::to_string(rtt) + " ms" : "Test edilmedi";
}
VoicePath NetworkClient::ActiveVoicePath() const noexcept {
    if (p2pConfigured_.load() && !p2pReady_.load()) return VoicePath::Probing;
    if (!p2pReady_.load()) return VoicePath::Relay;
    const std::uint64_t lastSeen = p2pLastSeenMs_.load();
    return lastSeen != 0 && SteadyMilliseconds() - lastSeen <= 2'000 ? VoicePath::DirectPeer : VoicePath::Relay;
}

bool NetworkClient::SetServerDenoiseRequested(const bool enabled) noexcept {
    serverDenoiseRequested_.store(enabled);
    if (state_.load() != ConnectionState::Connected && state_.load() != ConnectionState::Connecting) return true;
    if (secureMode_.load()) return enabled == serverDenoiseEnabled_.load();
    try {
        return SendJson(nlohmann::json{{"type", "set_denoise"}, {"enabled", enabled}}.dump());
    } catch (...) {
        return false;
    }
}

void NetworkClient::SetAudioCallback(AudioCallback callback) {
    std::scoped_lock lock(callbackMutex_);
    audioCallback_ = std::move(callback);
}

void NetworkClient::SetPeerRemovedCallback(PeerRemovedCallback callback) {
    std::scoped_lock lock(callbackMutex_);
    peerRemovedCallback_ = std::move(callback);
}

void NetworkClient::SetStateCallback(StateCallback callback) {
    std::scoped_lock lock(callbackMutex_);
    stateCallback_ = std::move(callback);
}

}  // namespace ss
