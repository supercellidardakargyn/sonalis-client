#include "sonalis/linux/linux_voice_transport.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <glib.h>
#include <json-glib/json-glib.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace sonalis::linux_platform {
namespace {

constexpr std::size_t HeaderBytes = 28;
constexpr std::size_t TagBytes = 16;
constexpr std::size_t MaximumDatagram = 1'500;

struct AddressRelease final { void operator()(addrinfo* value) const noexcept { if (value) freeaddrinfo(value); } };
struct SslContextRelease final { void operator()(SSL_CTX* value) const noexcept { if (value) SSL_CTX_free(value); } };
struct SslRelease final { void operator()(SSL* value) const noexcept { if (value) SSL_free(value); } };
struct X509Release final { void operator()(X509* value) const noexcept { if (value) X509_free(value); } };
struct CipherRelease final { void operator()(EVP_CIPHER_CTX* value) const noexcept { if (value) EVP_CIPHER_CTX_free(value); } };
struct GObjectRelease final { void operator()(gpointer value) const noexcept { if (value) g_object_unref(value); } };

void Put16(std::uint8_t* output, const std::uint16_t value) noexcept {
    output[0] = static_cast<std::uint8_t>(value >> 8U);
    output[1] = static_cast<std::uint8_t>(value);
}
void Put32(std::uint8_t* output, const std::uint32_t value) noexcept {
    for (int index = 0; index < 4; ++index) output[index] = static_cast<std::uint8_t>(value >> (24 - index * 8));
}
void Put64(std::uint8_t* output, const std::uint64_t value) noexcept {
    for (int index = 0; index < 8; ++index) output[index] = static_cast<std::uint8_t>(value >> (56 - index * 8));
}
std::uint16_t Get16(const std::uint8_t* input) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(input[0]) << 8U) | input[1]);
}
std::uint32_t Get32(const std::uint8_t* input) noexcept {
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) value = (value << 8U) | input[index];
    return value;
}
std::uint64_t Get64(const std::uint8_t* input) noexcept {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) value = (value << 8U) | input[index];
    return value;
}

bool DecodeHex(std::string_view input, std::array<std::uint8_t, 32>& output) {
    if (input.size() != output.size() * 2) return false;
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto digit = [](const char value) -> int {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            if (value >= 'A' && value <= 'F') return value - 'A' + 10;
            return -1;
        };
        const int high = digit(input[index * 2]);
        const int low = digit(input[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

bool DecodeUuid(std::string_view value, std::array<std::uint8_t, 16>& output) {
    std::array<char, 32> compact{};
    std::size_t count = 0;
    for (const char character : value) {
        if (character == '-') continue;
        if (count >= compact.size()) return false;
        compact[count++] = character;
    }
    if (count != compact.size()) return false;
    for (std::size_t index = 0; index < output.size(); ++index) {
        unsigned valueByte = 0;
        const auto result = std::from_chars(compact.data() + index * 2, compact.data() + index * 2 + 2,
                                            valueByte, 16);
        if (result.ec != std::errc{}) return false;
        output[index] = static_cast<std::uint8_t>(valueByte);
    }
    return true;
}

std::string JsonString(std::string_view input) {
    std::string output;
    output.reserve(input.size() + 8);
    for (const char value : input) {
        if (value == '\\' || value == '"') output.push_back('\\');
        output.push_back(value);
    }
    return output;
}

std::string JsonValue(JsonObject* object, const char* name) {
    if (!object || !json_object_has_member(object, name)) return {};
    const char* value = json_object_get_string_member(object, name);
    return value ? value : "";
}

bool DecodeBase64(std::string_view input, std::span<std::uint8_t> output) {
    gsize size = 0;
    guchar* decoded = g_base64_decode(std::string(input).c_str(), &size);
    if (!decoded || size != output.size()) { g_free(decoded); return false; }
    std::copy_n(decoded, size, output.data());
    OPENSSL_cleanse(decoded, size);
    g_free(decoded);
    return true;
}

}  // namespace

LinuxVoiceTransport::LinuxVoiceTransport(FrameCallback frame, StateCallback state)
    : frame_(std::move(frame)), state_(std::move(state)) {}

LinuxVoiceTransport::~LinuxVoiceTransport() { Close(); }

bool LinuxVoiceTransport::Connect(const LinuxVoiceGrant& grant, std::string& error) {
    Close();
    if (grant.peerToPeer) { error = "linux_relay_grant_required"; return false; }
    if (!JoinPinned(grant, error)) return false;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* addressesRaw = nullptr;
    const std::string port = std::to_string(grant.port);
    if (getaddrinfo(grant.host.c_str(), port.c_str(), &hints, &addressesRaw) != 0) {
        error = "voice_dns_failed";
        return false;
    }
    std::unique_ptr<addrinfo, AddressRelease> addresses(addressesRaw);
    for (addrinfo* address = addresses.get(); address != nullptr; address = address->ai_next) {
        const int candidate = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate < 0) continue;
        if (::connect(candidate, address->ai_addr, address->ai_addrlen) == 0) { socket_ = candidate; break; }
        ::close(candidate);
    }
    if (socket_ < 0) { error = "voice_udp_connect_failed"; return false; }
    timeval timeout{0, 500'000};
    (void)setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    outgoing_.store(0, std::memory_order_release);
    highestIncoming_ = replayMask_ = 0;
    receivedAny_ = false;
    bound_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    receiveThread_ = std::jthread([this](const std::stop_token token) { ReceiveLoop(token); });
    const std::array<std::uint8_t, 1> bind{0x01};
    for (int attempt = 0; attempt < 10 && !bound_.load(std::memory_order_acquire); ++attempt) {
        (void)SendSecure(bind, 0x01);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    if (!bound_.load(std::memory_order_acquire)) {
        error = "voice_udp_bind_timeout";
        Close();
        return false;
    }
    if (state_) state_("connected");
    return true;
}

bool LinuxVoiceTransport::JoinPinned(const LinuxVoiceGrant& grant, std::string& error) {
    std::array<std::uint8_t, 32> expected{};
    if (!DecodeHex(grant.certificateFingerprint, expected)) { error = "certificate_pin_invalid"; return false; }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addressesRaw = nullptr;
    const std::string port = std::to_string(grant.port);
    if (getaddrinfo(grant.host.c_str(), port.c_str(), &hints, &addressesRaw) != 0) {
        error = "voice_dns_failed";
        return false;
    }
    std::unique_ptr<addrinfo, AddressRelease> addresses(addressesRaw);
    int tcp = -1;
    for (addrinfo* address = addresses.get(); address != nullptr; address = address->ai_next) {
        const int candidate = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate < 0) continue;
        timeval timeout{8, 0};
        (void)setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (::connect(candidate, address->ai_addr, address->ai_addrlen) == 0) { tcp = candidate; break; }
        ::close(candidate);
    }
    if (tcp < 0) { error = "voice_tls_connect_failed"; return false; }
    const auto closeTcp = std::unique_ptr<int, std::function<void(int*)>>(&tcp, [](int* value) {
        if (value && *value >= 0) ::close(*value);
    });
    std::unique_ptr<SSL_CTX, SslContextRelease> context(SSL_CTX_new(TLS_client_method()));
    if (!context) { error = "voice_tls_context_failed"; return false; }
    SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION);
    SSL_CTX_set_verify(context.get(), SSL_VERIFY_NONE, nullptr);
    std::unique_ptr<SSL, SslRelease> ssl(SSL_new(context.get()));
    if (!ssl) { error = "voice_tls_context_failed"; return false; }
    (void)SSL_set_tlsext_host_name(ssl.get(), grant.host.c_str());
    SSL_set_fd(ssl.get(), tcp);
    if (SSL_connect(ssl.get()) != 1) { error = "voice_tls_handshake_failed"; return false; }
    std::unique_ptr<X509, X509Release> certificate(SSL_get1_peer_certificate(ssl.get()));
    if (!certificate) { error = "certificate_missing"; return false; }
    unsigned char* der = nullptr;
    const int derSize = i2d_X509(certificate.get(), &der);
    std::array<std::uint8_t, 32> actual{};
    unsigned int digestSize = 0;
    const bool fingerprintOk = derSize > 0
        && EVP_Digest(der, static_cast<std::size_t>(derSize), actual.data(), &digestSize, EVP_sha256(), nullptr) == 1
        && digestSize == actual.size() && CRYPTO_memcmp(expected.data(), actual.data(), actual.size()) == 0;
    OPENSSL_free(der);
    if (!fingerprintOk) { error = "certificate_pin_mismatch"; return false; }
    const std::string body = "{\"grant\":\"" + JsonString(grant.grant) + "\"}";
    const std::string request = "POST /join HTTP/1.1\r\nHost: " + grant.host + "\r\n"
        "Content-Type: application/json; charset=utf-8\r\nConnection: close\r\nContent-Length: "
        + std::to_string(body.size()) + "\r\n\r\n" + body;
    std::size_t sent = 0;
    while (sent < request.size()) {
        const int count = SSL_write(ssl.get(), request.data() + sent,
                                    static_cast<int>(request.size() - sent));
        if (count <= 0) { error = "voice_join_write_failed"; return false; }
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    response.reserve(4'096);
    std::array<char, 4'096> buffer{};
    for (;;) {
        const int count = SSL_read(ssl.get(), buffer.data(), static_cast<int>(buffer.size()));
        if (count <= 0) break;
        if (response.size() + static_cast<std::size_t>(count) > 64 * 1'024) {
            error = "voice_join_response_too_large";
            return false;
        }
        response.append(buffer.data(), static_cast<std::size_t>(count));
    }
    const auto headerEnd = response.find("\r\n\r\n");
    if (headerEnd == std::string::npos || !response.starts_with("HTTP/1.1 200 ")) {
        error = "voice_join_rejected";
        return false;
    }
    const std::string payload = response.substr(headerEnd + 4);
    std::unique_ptr<JsonParser, GObjectRelease> parser(json_parser_new());
    GError* jsonError = nullptr;
    if (!json_parser_load_from_data(parser.get(), payload.data(), static_cast<gssize>(payload.size()), &jsonError)) {
        if (jsonError) g_error_free(jsonError);
        error = "voice_join_invalid_json";
        return false;
    }
    JsonNode* root = json_parser_get_root(parser.get());
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) { error = "voice_join_invalid_json"; return false; }
    JsonObject* object = json_node_get_object(root);
    if (!DecodeUuid(JsonValue(object, "sessionId"), sessionId_)
        || !DecodeBase64(JsonValue(object, "udpKey"), key_)
        || !DecodeBase64(JsonValue(object, "noncePrefix"), noncePrefix_)) {
        error = "voice_session_invalid";
        return false;
    }
    return true;
}

bool LinuxVoiceTransport::SendAudio(const std::uint16_t sequence, const std::uint32_t timestamp,
                                    const std::uint8_t flags, const std::span<const std::uint8_t> opus) noexcept {
    if (opus.empty() || opus.size() > 1'275) return false;
    std::array<std::uint8_t, 1'283> plaintext{};
    plaintext[0] = 0x02;
    Put16(plaintext.data() + 1, sequence);
    Put32(plaintext.data() + 3, timestamp);
    plaintext[7] = flags;
    std::copy(opus.begin(), opus.end(), plaintext.begin() + 8);
    return SendSecure(std::span<const std::uint8_t>(plaintext.data(), opus.size() + 8), 0x02);
}

bool LinuxVoiceTransport::SendSecure(const std::span<const std::uint8_t> plaintext,
                                     const std::uint8_t flags) noexcept {
    if (!running_.load(std::memory_order_acquire) || socket_ < 0 || plaintext.size() > 1'400) return false;
    std::lock_guard lock(sendMutex_);
    const std::uint64_t sequence = outgoing_.fetch_add(1, std::memory_order_acq_rel) + 1;
    std::array<std::uint8_t, MaximumDatagram> packet{};
    packet[0] = 0x53; packet[1] = 0x53; packet[2] = 3; packet[3] = flags;
    std::copy(sessionId_.begin(), sessionId_.end(), packet.begin() + 4);
    Put64(packet.data() + 20, sequence);
    std::array<std::uint8_t, 12> nonce{};
    std::copy(noncePrefix_.begin(), noncePrefix_.end(), nonce.begin());
    Put64(nonce.data() + 4, sequence);
    std::unique_ptr<EVP_CIPHER_CTX, CipherRelease> cipher(EVP_CIPHER_CTX_new());
    int count = 0;
    int encrypted = 0;
    if (!cipher || EVP_EncryptInit_ex(cipher.get(), EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1
        || EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_AEAD_SET_IVLEN, nonce.size(), nullptr) != 1
        || EVP_EncryptInit_ex(cipher.get(), nullptr, nullptr, key_.data(), nonce.data()) != 1
        || EVP_EncryptUpdate(cipher.get(), nullptr, &count, packet.data(), HeaderBytes) != 1
        || EVP_EncryptUpdate(cipher.get(), packet.data() + HeaderBytes, &count,
                             plaintext.data(), static_cast<int>(plaintext.size())) != 1) return false;
    encrypted = count;
    if (EVP_EncryptFinal_ex(cipher.get(), packet.data() + HeaderBytes + encrypted, &count) != 1) return false;
    encrypted += count;
    if (EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_AEAD_GET_TAG, TagBytes,
                            packet.data() + HeaderBytes + encrypted) != 1) return false;
    const std::size_t size = HeaderBytes + static_cast<std::size_t>(encrypted) + TagBytes;
    return ::send(socket_, packet.data(), size, MSG_NOSIGNAL) == static_cast<ssize_t>(size);
}

void LinuxVoiceTransport::ReceiveLoop(const std::stop_token stopToken) noexcept {
    std::array<std::uint8_t, MaximumDatagram> packet{};
    auto lastHeartbeat = std::chrono::steady_clock::now();
    while (!stopToken.stop_requested() && running_.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() - lastHeartbeat >= std::chrono::seconds(5)) {
            const std::array<std::uint8_t, 1> heartbeat{0x01};
            (void)SendSecure(heartbeat, 0x01);
            lastHeartbeat = std::chrono::steady_clock::now();
        }
        const ssize_t received = ::recv(socket_, packet.data(), packet.size(), 0);
        if (received < 0) continue;
        const std::size_t size = static_cast<std::size_t>(received);
        if (size < HeaderBytes + TagBytes + 1 || packet[0] != 0x53 || packet[1] != 0x53 || packet[2] != 3
            || !std::equal(sessionId_.begin(), sessionId_.end(), packet.begin() + 4)) continue;
        const std::uint64_t sequence = Get64(packet.data() + 20);
        if (Replayed(sequence)) continue;
        std::array<std::uint8_t, 1'456> plaintext{};
        const std::size_t cipherBytes = size - HeaderBytes - TagBytes;
        std::array<std::uint8_t, 12> nonce{};
        std::copy(noncePrefix_.begin(), noncePrefix_.end(), nonce.begin());
        Put64(nonce.data() + 4, sequence);
        std::unique_ptr<EVP_CIPHER_CTX, CipherRelease> cipher(EVP_CIPHER_CTX_new());
        int count = 0;
        int clearBytes = 0;
        if (!cipher || EVP_DecryptInit_ex(cipher.get(), EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1
            || EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_AEAD_SET_IVLEN, nonce.size(), nullptr) != 1
            || EVP_DecryptInit_ex(cipher.get(), nullptr, nullptr, key_.data(), nonce.data()) != 1
            || EVP_DecryptUpdate(cipher.get(), nullptr, &count, packet.data(), HeaderBytes) != 1
            || EVP_DecryptUpdate(cipher.get(), plaintext.data(), &count, packet.data() + HeaderBytes,
                                 static_cast<int>(cipherBytes)) != 1) continue;
        clearBytes = count;
        if (EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_AEAD_SET_TAG, TagBytes,
                                packet.data() + HeaderBytes + cipherBytes) != 1
            || EVP_DecryptFinal_ex(cipher.get(), plaintext.data() + clearBytes, &count) != 1) continue;
        clearBytes += count;
        AcceptSequence(sequence);
        if (clearBytes == 1 && plaintext[0] == 0x11) { bound_.store(true, std::memory_order_release); continue; }
        if (clearBytes >= 2 && plaintext[0] == 0x13) { if (state_) state_("sleeping"); continue; }
        if (clearBytes < 10 || plaintext[0] != 0x12) continue;
        const std::size_t senderBytes = plaintext[1];
        const std::size_t metadata = 2 + senderBytes;
        if (senderBytes == 0 || senderBytes > 64 || metadata + 7 >= static_cast<std::size_t>(clearBytes)) continue;
        LinuxVoiceFrame frame;
        frame.senderId.assign(reinterpret_cast<const char*>(plaintext.data() + 2), senderBytes);
        frame.sequence = Get16(plaintext.data() + metadata);
        frame.timestamp = Get32(plaintext.data() + metadata + 2);
        frame.flags = plaintext[metadata + 6];
        frame.opus.assign(plaintext.begin() + static_cast<std::ptrdiff_t>(metadata + 7),
                          plaintext.begin() + clearBytes);
        if (frame_) frame_(std::move(frame));
    }
}

bool LinuxVoiceTransport::Replayed(const std::uint64_t sequence) noexcept {
    std::lock_guard lock(replayMutex_);
    if (!receivedAny_ || sequence > highestIncoming_) return false;
    const std::uint64_t delta = highestIncoming_ - sequence;
    return delta >= 64 || (replayMask_ & (std::uint64_t{1} << delta)) != 0;
}

void LinuxVoiceTransport::AcceptSequence(const std::uint64_t sequence) noexcept {
    std::lock_guard lock(replayMutex_);
    if (!receivedAny_) { receivedAny_ = true; highestIncoming_ = sequence; replayMask_ = 1; return; }
    if (sequence > highestIncoming_) {
        const std::uint64_t shift = sequence - highestIncoming_;
        replayMask_ = shift >= 64 ? 1 : (replayMask_ << shift) | 1;
        highestIncoming_ = sequence;
    } else replayMask_ |= std::uint64_t{1} << (highestIncoming_ - sequence);
}

void LinuxVoiceTransport::Close() noexcept {
    running_.store(false, std::memory_order_release);
    if (receiveThread_.joinable()) receiveThread_.request_stop();
    if (socket_ >= 0) { ::shutdown(socket_, SHUT_RDWR); ::close(socket_); socket_ = -1; }
    if (receiveThread_.joinable()) receiveThread_.join();
    bound_.store(false, std::memory_order_release);
    OPENSSL_cleanse(key_.data(), key_.size());
    sessionId_.fill(0);
    noncePrefix_.fill(0);
    if (state_) state_("disconnected");
}

}  // namespace sonalis::linux_platform
