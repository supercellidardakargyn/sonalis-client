#include "sonalis/linux/linux_api.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include <json-glib/json-glib.h>
#include <curl/curl.h>

namespace sonalis::linux_platform {
namespace {

struct GObjectRelease final {
    void operator()(gpointer value) const noexcept { if (value != nullptr) g_object_unref(value); }
};

using Parser = std::unique_ptr<JsonParser, GObjectRelease>;

::JsonObject* Parse(const std::vector<std::uint8_t>& bytes, Parser& parser) {
    parser.reset(json_parser_new());
    if (!parser || bytes.empty()) return nullptr;
    GError* error = nullptr;
    const gboolean loaded = json_parser_load_from_data(
        parser.get(), reinterpret_cast<const gchar*>(bytes.data()), static_cast<gssize>(bytes.size()), &error);
    if (!loaded) { if (error != nullptr) g_error_free(error); return nullptr; }
    JsonNode* root = json_parser_get_root(parser.get());
    return root != nullptr && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : nullptr;
}

std::string String(::JsonObject* object, const char* name) {
    if (object == nullptr || !json_object_has_member(object, name)) return {};
    JsonNode* node = json_object_get_member(object, name);
    if (node == nullptr || !JSON_NODE_HOLDS_VALUE(node)
        || json_node_get_value_type(node) != G_TYPE_STRING) return {};
    const char* value = json_node_get_string(node);
    return value != nullptr ? value : "";
}

std::int64_t Integer(::JsonObject* object, const char* name, const std::int64_t fallback = 0) {
    if (object == nullptr || !json_object_has_member(object, name)) return fallback;
    JsonNode* node = json_object_get_member(object, name);
    return node != nullptr && JSON_NODE_HOLDS_VALUE(node) ? json_node_get_int(node) : fallback;
}

bool Boolean(::JsonObject* object, const char* name, const bool fallback = false) {
    if (object == nullptr || !json_object_has_member(object, name)) return fallback;
    JsonNode* node = json_object_get_member(object, name);
    return node != nullptr && JSON_NODE_HOLDS_VALUE(node) ? json_node_get_boolean(node) != FALSE : fallback;
}

::JsonArray* Array(::JsonObject* object, const char* name) {
    if (object == nullptr || !json_object_has_member(object, name)) return nullptr;
    JsonNode* node = json_object_get_member(object, name);
    return node != nullptr && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : nullptr;
}

void Wipe(std::string& value) noexcept {
    std::fill(value.begin(), value.end(), '\0');
    value.clear();
}

std::string EscapeQuery(const std::string& input) {
    CURL* curl = curl_easy_init();
    if (curl == nullptr) return {};
    char* escaped = curl_easy_escape(curl, input.c_str(), static_cast<int>(input.size()));
    std::string output = escaped != nullptr ? escaped : "";
    if (escaped != nullptr) curl_free(escaped);
    curl_easy_cleanup(curl);
    return output;
}

bool ValidUuid(const std::string& value) {
    if (value.size() != 36) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return false;
        } else if (!g_ascii_isxdigit(static_cast<guchar>(value[index]))) return false;
    }
    return true;
}

}  // namespace

LinuxApi::LinuxApi(std::string origin) : http_(std::move(origin)) {}

std::vector<std::uint8_t> LinuxApi::JsonObject(
    const std::initializer_list<std::pair<const char*, std::string>> values) {
    std::unique_ptr<JsonBuilder, GObjectRelease> builder(json_builder_new());
    json_builder_begin_object(builder.get());
    for (const auto& [name, value] : values) {
        json_builder_set_member_name(builder.get(), name);
        json_builder_add_string_value(builder.get(), value.c_str());
    }
    json_builder_end_object(builder.get());
    JsonNode* root = json_builder_get_root(builder.get());
    std::unique_ptr<JsonGenerator, GObjectRelease> generator(json_generator_new());
    json_generator_set_root(generator.get(), root);
    gsize size = 0;
    gchar* data = json_generator_to_data(generator.get(), &size);
    std::vector<std::uint8_t> output(reinterpret_cast<std::uint8_t*>(data),
                                     reinterpret_cast<std::uint8_t*>(data) + size);
    g_free(data);
    json_node_free(root);
    return output;
}

void LinuxApi::Login(std::string login, std::string password, StatusCompletion completion) {
    core::HttpRequest request{core::HttpMethod::Post, "/api/v1/auth/login",
        JsonObject({{"login", std::move(login)}, {"password", std::move(password)},
                    {"deviceName", "Linux"}, {"clientVersion", "5.1.0"}}), {}, false};
    if (!http_.Submit(std::move(request), [this, completion = std::move(completion)](core::HttpResponse response) mutable {
        Parser parser;
        ::JsonObject* root = Parse(response.body, parser);
        if (response.status < 200 || response.status >= 300 || root == nullptr) {
            completion(false, response.safeErrorCode.empty() ? "login_failed" : response.safeErrorCode);
            return;
        }
        std::string access = String(root, "accessToken");
        std::string refresh = String(root, "refreshToken");
        if (access.empty() || refresh.size() < 32) { completion(false, "login_contract_invalid"); return; }
        std::string error;
        const auto refreshBytes = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(refresh.data()), refresh.size());
        if (!secureStore_.Put("refresh_token", refreshBytes, error)) {
            Wipe(refresh);
            completion(false, "secure_store_write_failed");
            return;
        }
        Wipe(refresh);
        http_.SetBearerToken(std::move(access));
        completion(true, {});
    })) completion(false, "worker_queue_full");
}

void LinuxApi::Restore(StatusCompletion completion) {
    std::string error;
    auto token = secureStore_.Get("refresh_token", error);
    if (token.empty()) { completion(false, "session_missing"); return; }
    std::string value(reinterpret_cast<const char*>(token.data()), token.size());
    std::fill(token.begin(), token.end(), 0);
    Refresh(std::move(value), std::move(completion));
}

void LinuxApi::Refresh(std::string token, StatusCompletion completion) {
    core::HttpRequest request{core::HttpMethod::Post, "/api/v1/auth/refresh",
        JsonObject({{"refreshToken", token}, {"clientVersion", "5.1.0"}}), {}, false};
    std::fill(token.begin(), token.end(), '\0');
    if (!http_.Submit(std::move(request), [this, completion = std::move(completion)](core::HttpResponse response) mutable {
        Parser parser;
        ::JsonObject* root = Parse(response.body, parser);
        if (response.status < 200 || response.status >= 300 || root == nullptr) {
            completion(false, response.status == 401 ? "session_expired" : "refresh_failed");
            return;
        }
        std::string access = String(root, "accessToken");
        std::string refresh = String(root, "refreshToken");
        if (access.empty()) { completion(false, "refresh_contract_invalid"); return; }
        if (!refresh.empty()) {
            std::string error;
            const auto refreshBytes = std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(refresh.data()), refresh.size());
            if (!secureStore_.Put("refresh_token", refreshBytes, error)) {
                Wipe(refresh);
                completion(false, "secure_store_write_failed");
                return;
            }
            Wipe(refresh);
        }
        http_.SetBearerToken(std::move(access));
        completion(true, {});
    })) completion(false, "worker_queue_full");
}

void LinuxApi::Logout(StatusCompletion completion) {
    std::string error;
    auto token = secureStore_.Get("refresh_token", error);
    std::string value(reinterpret_cast<const char*>(token.data()), token.size());
    std::fill(token.begin(), token.end(), 0);
    core::HttpRequest request{core::HttpMethod::Post, "/api/v1/auth/logout",
        JsonObject({{"refreshToken", value}}), {}, false};
    Wipe(value);
    if (!http_.Submit(std::move(request), [this, completion = std::move(completion)](core::HttpResponse) mutable {
        std::string ignored;
        (void)secureStore_.Erase("refresh_token", ignored);
        http_.SetBearerToken({});
        completion(true, {});
    })) completion(false, "worker_queue_full");
}

void LinuxApi::Rooms(RoomsCompletion completion) {
    core::HttpRequest request{core::HttpMethod::Get, "/api/v1/me/rooms", {}, {}, true};
    if (!http_.Submit(std::move(request), [completion = std::move(completion)](core::HttpResponse response) mutable {
        Parser parser;
        ::JsonObject* root = Parse(response.body, parser);
        ::JsonArray* rows = Array(root, "rooms");
        if (response.status != 200 || rows == nullptr) { completion({}, "rooms_failed"); return; }
        std::vector<core::Room> rooms;
        const guint count = std::min<guint>(json_array_get_length(rows), core::ClientState::MaximumRooms);
        rooms.reserve(count);
        for (guint index = 0; index < count; ++index) {
            ::JsonObject* row = json_array_get_object_element(rows, index);
            rooms.push_back({String(row, "id"), String(row, "name"), String(row, "role")});
        }
        completion(std::move(rooms), {});
    })) completion({}, "worker_queue_full");
}

void LinuxApi::Channels(std::string roomId, ChannelsCompletion completion) {
    if (!ValidUuid(roomId)) { completion({}, "invalid_room_id"); return; }
    const std::string path = "/api/v1/rooms/" + roomId + "/overview";
    core::HttpRequest request{core::HttpMethod::Get, path, {}, {}, true};
    if (!http_.Submit(std::move(request), [roomId = std::move(roomId), completion = std::move(completion)](
        core::HttpResponse response) mutable {
        Parser parser;
        ::JsonObject* root = Parse(response.body, parser);
        ::JsonArray* rows = Array(root, "channels");
        if (response.status != 200 || rows == nullptr) { completion({}, "channels_failed"); return; }
        std::vector<core::Channel> channels;
        const guint count = std::min<guint>(json_array_get_length(rows), core::ClientState::MaximumChannels);
        channels.reserve(count);
        for (guint index = 0; index < count; ++index) {
            ::JsonObject* row = json_array_get_object_element(rows, index);
            channels.push_back({String(row, "id"), roomId, String(row, "categoryId"), String(row, "name"),
                String(row, "type") == "voice" ? core::ChannelKind::Voice : core::ChannelKind::Text,
                static_cast<std::uint32_t>(std::max<std::int64_t>(0, Integer(row, "unreadCount"))),
                static_cast<std::uint32_t>(std::max<std::int64_t>(0, Integer(row, "mentionCount")))});
        }
        completion(std::move(channels), {});
    })) completion({}, "worker_queue_full");
}

void LinuxApi::Messages(std::string channelId, std::string beforeCursor, MessagesCompletion completion) {
    if (!ValidUuid(channelId) || beforeCursor.size() > 256) { completion({}, "invalid_message_cursor"); return; }
    std::string path = "/api/v1/channels/" + channelId + "/messages?limit=50";
    if (!beforeCursor.empty()) path += "&beforeCursor=" + EscapeQuery(beforeCursor);
    core::HttpRequest request{core::HttpMethod::Get, std::move(path), {}, {}, true};
    if (!http_.Submit(std::move(request), [channelId = std::move(channelId), completion = std::move(completion)](
        core::HttpResponse response) mutable {
        Parser parser;
        ::JsonObject* root = Parse(response.body, parser);
        ::JsonArray* rows = Array(root, "messages");
        if (response.status != 200 || rows == nullptr) { completion({}, "messages_failed"); return; }
        std::vector<LinuxEncryptedMessage> messages;
        const guint count = std::min<guint>(json_array_get_length(rows), 50);
        messages.reserve(count);
        for (guint index = 0; index < count; ++index) {
            ::JsonObject* row = json_array_get_object_element(rows, index);
            messages.push_back({String(row, "id"), String(row, "channelId").empty() ? channelId : String(row, "channelId"),
                String(row, "senderId"), String(row, "ciphertext"), String(row, "nonce"),
                String(row, "signature"), String(row, "createdAt")});
        }
        completion(std::move(messages), {});
    })) completion({}, "worker_queue_full");
}

void LinuxApi::RealtimeGrant(RealtimeCompletion completion) {
    core::HttpRequest request{core::HttpMethod::Post, "/api/v1/realtime/grant", JsonObject({})};
    if (!http_.Submit(std::move(request), [completion = std::move(completion)](core::HttpResponse response) mutable {
        Parser parser;
        ::JsonObject* root = Parse(response.body, parser);
        LinuxRealtimeGrant grant{String(root, "websocketUrl"), String(root, "ticket")};
        const bool valid = response.status == 200 && grant.websocketUrl.starts_with("wss://")
            && grant.ticket.size() >= 32 && grant.ticket.size() <= 128;
        if (!valid) { completion({}, "realtime_grant_invalid"); return; }
        completion(std::move(grant), {});
    })) completion({}, "worker_queue_full");
}

void LinuxApi::VoiceGrant(std::string roomId, std::string channelId, const bool serverDenoise,
                          const bool peerToPeer, VoiceCompletion completion) {
    if (!ValidUuid(roomId) || !ValidUuid(channelId)) { completion({}, "invalid_voice_channel"); return; }
    std::unique_ptr<JsonBuilder, GObjectRelease> builder(json_builder_new());
    json_builder_begin_object(builder.get());
    for (const auto& [name, value] : std::array<std::pair<const char*, std::string>, 2>{
        std::pair{"roomId", roomId}, std::pair{"channelId", channelId}}) {
        json_builder_set_member_name(builder.get(), name);
        json_builder_add_string_value(builder.get(), value.c_str());
    }
    json_builder_set_member_name(builder.get(), "serverDenoiseRequested");
    json_builder_add_boolean_value(builder.get(), serverDenoise);
    json_builder_set_member_name(builder.get(), "p2pEnabled");
    json_builder_add_boolean_value(builder.get(), peerToPeer);
    json_builder_set_member_name(builder.get(), "regionLatency");
    json_builder_begin_object(builder.get());
    json_builder_end_object(builder.get());
    json_builder_end_object(builder.get());
    JsonNode* root = json_builder_get_root(builder.get());
    std::unique_ptr<JsonGenerator, GObjectRelease> generator(json_generator_new());
    json_generator_set_root(generator.get(), root);
    gsize size = 0;
    gchar* data = json_generator_to_data(generator.get(), &size);
    std::vector<std::uint8_t> body(reinterpret_cast<std::uint8_t*>(data),
                                   reinterpret_cast<std::uint8_t*>(data) + size);
    g_free(data);
    json_node_free(root);
    core::HttpRequest request{core::HttpMethod::Post, "/api/v1/voice/join-grant", std::move(body)};
    if (!http_.Submit(std::move(request), [completion = std::move(completion)](core::HttpResponse response) mutable {
        Parser parser;
        ::JsonObject* object = Parse(response.body, parser);
        LinuxVoiceGrant grant{String(object, "grant"), String(object, "roomId"), String(object, "channelId"),
            String(object, "host"), String(object, "certificateFingerprint"), String(object, "routeType"),
            static_cast<std::uint16_t>(std::clamp<std::int64_t>(Integer(object, "port", 25'565), 1, 65'535)),
            static_cast<std::uint32_t>(std::clamp<std::int64_t>(Integer(object, "bitrate", 24'000), 12'000, 64'000)),
            Boolean(object, "serverDenoise"), Boolean(object, "p2pEnabled"), Boolean(object, "canSpeak", true)};
        const bool valid = response.status == 200 && grant.grant.size() >= 32 && !grant.host.empty()
            && grant.certificateFingerprint.size() == 64;
        if (!valid) { completion({}, "voice_grant_invalid"); return; }
        completion(std::move(grant), {});
    })) completion({}, "worker_queue_full");
}

}  // namespace sonalis::linux_platform
