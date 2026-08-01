#pragma once

#include <functional>
#include <initializer_list>
#include <cstdint>
#include <string>
#include <vector>

#include "sonalis/core/client_state.h"
#include "sonalis/linux/curl_http_client.h"
#include "sonalis/linux/secret_service_store.h"

namespace sonalis::linux_platform {

struct LinuxEncryptedMessage final {
    std::string id;
    std::string channelId;
    std::string senderId;
    std::string ciphertext;
    std::string nonce;
    std::string signature;
    std::string createdAt;
};

struct LinuxRealtimeGrant final {
    std::string websocketUrl;
    std::string ticket;
};

struct LinuxVoiceGrant final {
    std::string grant;
    std::string roomId;
    std::string channelId;
    std::string host;
    std::string certificateFingerprint;
    std::string routeType;
    std::uint16_t port{25'565};
    std::uint32_t bitrate{24'000};
    bool serverDenoise{};
    bool peerToPeer{};
    bool canSpeak{true};
};

class LinuxApi final {
public:
    using StatusCompletion = std::function<void(bool, std::string)>;
    using RoomsCompletion = std::function<void(std::vector<core::Room>, std::string)>;
    using ChannelsCompletion = std::function<void(std::vector<core::Channel>, std::string)>;
    using MessagesCompletion = std::function<void(std::vector<LinuxEncryptedMessage>, std::string)>;
    using RealtimeCompletion = std::function<void(LinuxRealtimeGrant, std::string)>;
    using VoiceCompletion = std::function<void(LinuxVoiceGrant, std::string)>;

    explicit LinuxApi(std::string origin = "https://sonalis.tr");
    void Login(std::string login, std::string password, StatusCompletion completion);
    void Restore(StatusCompletion completion);
    void Logout(StatusCompletion completion);
    void Rooms(RoomsCompletion completion);
    void Channels(std::string roomId, ChannelsCompletion completion);
    void Messages(std::string channelId, std::string beforeCursor, MessagesCompletion completion);
    void RealtimeGrant(RealtimeCompletion completion);
    void VoiceGrant(std::string roomId, std::string channelId, bool serverDenoise,
                    bool peerToPeer, VoiceCompletion completion);

private:
    void Refresh(std::string token, StatusCompletion completion);
    static std::vector<std::uint8_t> JsonObject(
        std::initializer_list<std::pair<const char*, std::string>> values);

    CurlHttpClient http_;
    SecretServiceStore secureStore_;
};

}  // namespace sonalis::linux_platform
