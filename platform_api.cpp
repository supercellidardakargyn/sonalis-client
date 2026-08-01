#include "platform_api.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <nlohmann/json.hpp>

#include "diagnostics.h"
#include "json_contract.h"
#include "settings.h"

namespace ss {
namespace {

#ifndef SONALIS_VERSION
#define SONALIS_VERSION "0.0.0"
#endif
constexpr const char* kClientVersion = SONALIS_VERSION;

std::string IsoTimestamp(const std::uint64_t timestampMs) {
    const std::time_t seconds = static_cast<std::time_t>(timestampMs / 1000ULL);
    std::tm utc{};
    gmtime_s(&utc, &seconds);
    char result[40]{};
    std::snprintf(result, sizeof(result),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%03lluZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec,
                  static_cast<unsigned long long>(timestampMs % 1000ULL));
    return result;
}

PlatformUser ParseUser(const nlohmann::json& value) {
    return {JsonStringOr(value, "id"), JsonStringOr(value, "username"), JsonStringOr(value, "nickname"),
            JsonStringOr(value, "role", "user"), JsonStringOr(value, "plan", "free")};
}

std::string UrlEncode(const std::string& value) {
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (const unsigned char character : value) {
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9') || character == '-' || character == '_'
            || character == '.' || character == '~') {
            encoded << static_cast<char>(character);
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(character);
        }
    }
    return encoded.str();
}

std::string IsoAfterMinutes(const int minutes) {
    if (minutes <= 0) return {};
    const auto target = std::chrono::system_clock::now() + std::chrono::minutes(minutes);
    const std::time_t time = std::chrono::system_clock::to_time_t(target);
    std::tm utc{};
    if (gmtime_s(&utc, &time) != 0) return {};
    std::ostringstream value;
    value << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return value.str();
}

std::string ErrorCodeFrom(const HttpResponse& response) {
    try {
        return JsonStringOr(nlohmann::json::parse(response.body), "error",
                            "HTTP " + std::to_string(response.status));
    } catch (...) {
        return "HTTP " + std::to_string(response.status);
    }
}

std::string RouteCategory(const std::string& path) {
    if (path == "/api/v1/me/rooms") return "rooms";
    if (path == "/api/v1/friends") return "friends";
    if (path == "/api/v1/realtime/grant") return "realtime_grant";
    if (path.starts_with("/api/v1/rooms/") && !path.ends_with("/overview")) return "room_members_or_action";
    if (path.ends_with("/overview")) return "room_overview";
    if (path.starts_with("/api/v1/messages/") || path.starts_with("/api/v1/conversations/")) return "messaging";
    if (path.starts_with("/api/v1/voice/")) return "voice_grant";
    return "platform";
}

}  // namespace

void PlatformApi::SetOrigin(std::string origin) {
    while (!origin.empty() && origin.back() == '/') origin.pop_back();
    if (!origin.starts_with("https://") && !origin.starts_with("http://")) origin = "https://" + origin;
#if defined(_DEBUG)
    const bool developmentOrigin = origin.starts_with("http://localhost")
        || origin.starts_with("http://127.0.0.1") || origin.starts_with("http://[::1]");
#else
    constexpr bool developmentOrigin = false;
#endif
    if (!origin.starts_with("https://") && !developmentOrigin) {
        DiagnosticLog("security", "insecure_control_origin_rejected");
        origin = "https://sonalis.tr";
    }
    std::scoped_lock lock(mutex_); origin_ = std::move(origin);
}

void PlatformApi::SetClientDeviceId(std::string deviceId) {
    std::scoped_lock lock(mutex_);
    clientDeviceId_ = std::move(deviceId);
}

std::string PlatformApi::Origin() const { std::scoped_lock lock(mutex_); return origin_; }

std::string PlatformApi::ErrorFrom(const HttpResponse& response) {
    try {
        const std::string code = ErrorCodeFrom(response);
        if (code == "device_bound_to_another_account") {
            return "Bu Sonalis kurulumu baska bir hesaba bagli. Bu bilgisayarda yalniz bagli hesap kullanilabilir.";
        }
        if (code == "native_client_update_required") {
            return "Bu istemci surumu desteklenmiyor. Sonalis'i guncelleyip yeniden deneyin.";
        }
        if (code == "device_binding_failed") {
            return "Cihaz hesaba baglanamadi. Biraz sonra yeniden deneyin.";
        }
        if (code == "device_session_mismatch") {
            return "Kayitli oturum bu Windows kurulumuna ait degil. Guvenlik icin yeniden giris yapin.";
        }
        if (code.starts_with("device_license_")) {
            return "Bu cihaz lisansi gecersiz, suresi dolmus veya yonetici tarafindan iptal edilmis.";
        }
        if (code == "access_token_expired" || code == "access_token_invalid"
            || code == "refresh_invalid" || code == "session_expired") {
            return "session_expired";
        }
        if (code == "session_refresh_failed") return "session_refresh_failed";
        return code;
    }
    catch (...) { return "HTTP " + std::to_string(response.status); }
}

std::optional<NativeLoginChallenge> PlatformApi::RequestNativeLoginChallenge(
    const std::string& signingPublicKey, const std::string& keyAlgorithm, std::string& error) {
    try {
        std::string installationId;
        { std::scoped_lock lock(mutex_); installationId = clientDeviceId_; }
        if (installationId.empty()) { error = "Kurulum kimligi hazir degil"; return std::nullopt; }
        const auto response = http_.Request(
            L"POST", Origin() + "/api/v1/auth/native-installation/challenge",
            nlohmann::json{{"installationId", installationId}, {"signingPublicKey", signingPublicKey},
                           {"keyAlgorithm", keyAlgorithm}}.dump());
        if (response.status != 201) { error = ErrorFrom(response); return std::nullopt; }
        const auto json = nlohmann::json::parse(response.body);
        return NativeLoginChallenge{JsonStringOr(json, "id"), JsonStringOr(json, "challenge")};
    } catch (const std::exception& exception) { error = exception.what(); return std::nullopt; }
}

bool PlatformApi::Login(const std::string& login, const std::string& password,
                        const std::string& signingPublicKey, const std::string& keyAlgorithm, const NativeLoginChallenge& challenge,
                        const std::string& signature, std::string& error) {
    try {
        std::string deviceId;
        { std::scoped_lock lock(mutex_); deviceId = clientDeviceId_; }
        nlohmann::json body{{"login", login}, {"password", password}, {"deviceName", "Windows Native Client"},
                            {"clientVersion", kClientVersion}, {"challengeId", challenge.id},
                            {"challenge", challenge.challenge}, {"devicePublicKey", signingPublicKey},
                            {"deviceKeyAlgorithm", keyAlgorithm}, {"deviceSignature", signature}};
        if (!deviceId.empty()) body["deviceId"] = deviceId;
        const HttpResponse response = http_.Request(L"POST", Origin() + "/api/v1/auth/login", body.dump());
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        const auto json = nlohmann::json::parse(response.body);
        std::string vaultError;
        if (!vault_.SaveRefreshToken(JsonStringOr(json, "refreshToken"), vaultError)) { error = vaultError; return false; }
        const std::string deviceLicense = JsonStringOr(json, "deviceLicense");
        if (!vault_.SaveDeviceLicense(deviceLicense, vaultError)) { vault_.ClearRefreshToken(); error = vaultError; return false; }
        StoreAuthenticatedSession(JsonStringOr(json, "accessToken"), deviceLicense,
                                  JsonIntegerOr<int>(json, "expiresIn", 900));
        { std::scoped_lock lock(mutex_); user_ = ParseUser(json.at("user")); }
        return IsAuthenticated();
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

PlatformApi::RefreshOutcome PlatformApi::Refresh(std::string& error, const bool force) {
    std::unique_lock refreshLock(refreshMutex_);
    if (refreshInProgress_) {
        refreshCv_.wait(refreshLock, [this] { return !refreshInProgress_; });
        error = lastRefreshError_;
        return lastRefreshOutcome_;
    }
    if (!force) {
        std::scoped_lock lock(mutex_);
        if (sessionState_ == PlatformSessionState::Active && !accessToken_.empty()
            && accessTokenExpiresAt_ > std::chrono::steady_clock::now() + std::chrono::seconds(60)) {
            return RefreshOutcome::Success;
        }
    }
    refreshInProgress_ = true;
    refreshLock.unlock();
    const auto finish = [this, &error](const RefreshOutcome outcome) {
        {
            std::scoped_lock lock(refreshMutex_);
            lastRefreshOutcome_ = outcome;
            lastRefreshError_ = error;
            refreshInProgress_ = false;
        }
        refreshCv_.notify_all();
        return outcome;
    };
    try {
        const std::string refresh = vault_.LoadRefreshToken();
        if (refresh.empty()) {
            error = "Kayitli oturum yok";
            Logout();
            return finish(RefreshOutcome::SessionExpired);
        }
        std::string deviceId;
        { std::scoped_lock lock(mutex_); deviceId = clientDeviceId_; }
        nlohmann::json body{{"refreshToken", refresh}, {"clientVersion", kClientVersion}};
        if (!deviceId.empty()) body["deviceId"] = deviceId;
        const HttpResponse response = http_.Request(L"POST", Origin() + "/api/v1/auth/refresh", body.dump());
        if (response.status != 200) {
            const std::string code = ErrorCodeFrom(response);
            error = ErrorFrom(response);
            DiagnosticLog("session", "refresh_failed status=" + std::to_string(response.status)
                          + " code=" + code);
            if (response.status == 401 || response.status == 409) {
                ClearSession(true);
                error = "session_expired";
                return finish(RefreshOutcome::SessionExpired);
            }
            if (response.status == 426 || code == "native_client_update_required"
                || code == "native_client_version_mismatch") {
                std::scoped_lock lock(mutex_);
                sessionState_ = PlatformSessionState::UpdateRequired;
                return finish(RefreshOutcome::UpdateRequired);
            }
            return finish(RefreshOutcome::TransientFailure);
        }
        const auto json = nlohmann::json::parse(response.body); std::string vaultError;
        if (!vault_.SaveRefreshToken(JsonStringOr(json, "refreshToken"), vaultError)) {
            error = vaultError;
            ClearSession(true);
            return finish(RefreshOutcome::SessionExpired);
        }
        const std::string deviceLicense = JsonStringOr(json, "deviceLicense");
        if (!vault_.SaveDeviceLicense(deviceLicense, vaultError)) {
            error = vaultError;
            ClearSession(true);
            return finish(RefreshOutcome::SessionExpired);
        }
        StoreAuthenticatedSession(JsonStringOr(json, "accessToken"), deviceLicense,
                                  JsonIntegerOr<int>(json, "expiresIn", 900));
        return finish(IsAuthenticated() ? RefreshOutcome::Success : RefreshOutcome::SessionExpired);
    } catch (const std::exception& exception) {
        error = exception.what();
        DiagnosticLog("session", "refresh_transport_failed");
        return finish(RefreshOutcome::TransientFailure);
    }
}

bool PlatformApi::RestoreSession(std::string& error) {
    if (Refresh(error, true) != RefreshOutcome::Success) return false;
    try {
        const HttpResponse response = Authorized(L"GET", "/api/v1/me");
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        const auto json = nlohmann::json::parse(response.body); std::scoped_lock lock(mutex_); user_ = ParseUser(json.at("user")); return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

HttpResponse PlatformApi::Authorized(
    const std::wstring& method,
    const std::string& path,
    const std::string_view body,
    const std::map<std::wstring, std::wstring>& additionalHeaders) {
    bool refreshDue = false;
    {
        std::scoped_lock lock(mutex_);
        if (sessionState_ == PlatformSessionState::Expired || accessToken_.empty()) {
            return RefreshFailureResponse(RefreshOutcome::SessionExpired);
        }
        if (sessionState_ == PlatformSessionState::UpdateRequired) {
            return RefreshFailureResponse(RefreshOutcome::UpdateRequired);
        }
        refreshDue = accessTokenExpiresAt_ <= std::chrono::steady_clock::now() + std::chrono::seconds(60);
    }
    if (refreshDue) {
        std::string refreshError;
        const RefreshOutcome outcome = Refresh(refreshError, false);
        if (outcome != RefreshOutcome::Success) return RefreshFailureResponse(outcome);
    }
    std::string token; std::string deviceLicense;
    { std::scoped_lock lock(mutex_); token = accessToken_; deviceLicense = deviceLicense_; }
    std::map<std::wstring, std::wstring> headers{{L"Authorization", L"Bearer " + Utf8ToWide(token)},
                                                 {L"X-Sonalis-Device-License", Utf8ToWide(deviceLicense)}};
    for (const auto& [name, value] : additionalHeaders) headers[name] = value;
    HttpResponse response = http_.Request(method, Origin() + path, body, headers);
    if (response.status != 401) return response;
    DiagnosticLog("platform_http", "route=" + RouteCategory(path) + " status=401 code="
                  + ErrorCodeFrom(response));
    std::string error;
    const RefreshOutcome outcome = Refresh(error, true);
    if (outcome != RefreshOutcome::Success) return RefreshFailureResponse(outcome);
    { std::scoped_lock lock(mutex_); token = accessToken_; deviceLicense = deviceLicense_; }
    headers[L"Authorization"] = L"Bearer " + Utf8ToWide(token);
    headers[L"X-Sonalis-Device-License"] = Utf8ToWide(deviceLicense);
    return http_.Request(method, Origin() + path, body, headers);
}

HttpResponse PlatformApi::RefreshFailureResponse(const RefreshOutcome outcome) {
    if (outcome == RefreshOutcome::SessionExpired) {
        return {401, R"({"error":"session_expired"})"};
    }
    if (outcome == RefreshOutcome::UpdateRequired) {
        return {426, R"({"error":"native_client_update_required"})"};
    }
    return {503, R"({"error":"session_refresh_failed"})"};
}

void PlatformApi::ClearSession(const bool clearVault) noexcept {
    if (clearVault) {
        vault_.ClearRefreshToken();
        vault_.ClearDeviceLicense();
    }
    std::scoped_lock lock(mutex_);
    accessToken_.clear();
    deviceLicense_.clear();
    user_ = {};
    accessTokenExpiresAt_ = {};
    sessionState_ = PlatformSessionState::Expired;
}

void PlatformApi::StoreAuthenticatedSession(std::string accessToken, std::string deviceLicense,
                                            const int expiresInSeconds) {
    std::scoped_lock lock(mutex_);
    accessToken_ = std::move(accessToken);
    deviceLicense_ = std::move(deviceLicense);
    accessTokenExpiresAt_ = std::chrono::steady_clock::now()
        + std::chrono::seconds(std::clamp(expiresInSeconds, 1, 86'400));
    sessionState_ = !accessToken_.empty() && !deviceLicense_.empty()
        ? PlatformSessionState::Active : PlatformSessionState::Expired;
}

void PlatformApi::Logout() noexcept {
    vault_.ClearRefreshToken();
    vault_.ClearDeviceLicense();
    std::scoped_lock lock(mutex_);
    accessToken_.clear();
    deviceLicense_.clear();
    user_ = {};
    accessTokenExpiresAt_ = {};
    sessionState_ = PlatformSessionState::SignedOut;
}

bool PlatformApi::IsAuthenticated() const noexcept {
    std::scoped_lock lock(mutex_);
    return sessionState_ == PlatformSessionState::Active && !accessToken_.empty();
}

PlatformSessionState PlatformApi::SessionState() const noexcept {
    std::scoped_lock lock(mutex_);
    return sessionState_;
}
PlatformUser PlatformApi::User() const { std::scoped_lock lock(mutex_); return user_; }

std::vector<PlatformRoom> PlatformApi::Rooms(std::string& error) {
    try {
        const HttpResponse response = Authorized(L"GET", "/api/v1/me/rooms");
        DiagnosticLog("rooms", "http=" + std::to_string(response.status) + " bytes=" + std::to_string(response.body.size()));
        if (response.status != 200) { error = ErrorFrom(response); DiagnosticLog("rooms", "error=" + error); return {}; }
        std::vector<PlatformRoom> rooms;
        const auto parsed = nlohmann::json::parse(response.body);
        for (const auto& room : parsed.at("rooms")) {
            const std::string nodeState = room.contains("nodeState") && room.at("nodeState").is_string()
                ? room.at("nodeState").get<std::string>()
                : std::string{};
            rooms.push_back({JsonStringOr(room, "id"), JsonStringOr(room, "name"),
                             JsonStringOr(room, "description"), JsonStringOr(room, "role", "member"),
                             nodeState, JsonBooleanOr(room, "serverDenoiseEnabled")});
        }
        DiagnosticLog("rooms", "parsed=" + std::to_string(rooms.size()));
        return rooms;
    } catch (const std::exception& exception) { error = exception.what(); DiagnosticLog("rooms", "exception=" + error); return {}; }
}

std::optional<PlatformRoomOverview> PlatformApi::RoomOverview(const std::string& roomId, std::string& error) {
    try {
        const HttpResponse response = Authorized(L"GET", "/api/v1/rooms/" + roomId + "/overview");
        if (response.status != 200) { error = ErrorFrom(response); return std::nullopt; }
        const auto parsed = nlohmann::json::parse(response.body);
        const auto& room = parsed.at("room");
        PlatformRoomOverview result;
        result.roomId = JsonStringOr(room, "id", roomId);
        result.name = JsonStringOr(room, "name");
        result.description = JsonStringOr(room, "description");
        result.role = JsonStringOr(room, "role", "member");
        for (const auto& value : parsed.at("categories")) {
            result.categories.push_back({JsonStringOr(value, "id"), JsonStringOr(value, "name"),
                                         JsonIntegerOr<int>(value, "position")});
        }
        for (const auto& value : parsed.at("channels")) {
            result.channels.push_back({
                JsonStringOr(value, "id"), JsonStringOr(value, "categoryId"), JsonStringOr(value, "type", "text"),
                JsonStringOr(value, "slug"), JsonStringOr(value, "name"), JsonStringOr(value, "topic"),
                JsonIntegerOr<int>(value, "position"), JsonIntegerOr<int>(value, "voiceUserLimit"),
                JsonIntegerOr<int>(value, "slowModeSeconds"),
                JsonIntegerOr<std::uint32_t>(value, "unreadCount"),
                JsonIntegerOr<std::uint32_t>(value, "mentionCount"),
                JsonIntegerOr<std::uint32_t>(value, "pinCount"),
                JsonStringOr(value, "notificationMode", "mentions"), JsonStringOr(value, "muteUntil"),
                JsonStringOr(value, "contentRating", "sfw"),
                JsonStringOr(value, "mediaPostingPolicy", "members"),
                JsonBooleanOr(value, "localMediaScanRequired", true),
            });
        }
        for (const auto& value : parsed.at("members")) {
            result.members.push_back({
                JsonStringOr(value, "userId"), JsonStringOr(value, "username"), JsonStringOr(value, "nickname"),
                JsonStringOr(value, "role", "member"),
            });
        }
        return result;
    } catch (const std::exception& exception) { error = exception.what(); return std::nullopt; }
}

bool PlatformApi::CreateChannelCategory(const std::string& roomId, const std::string& name, std::string& error) {
    try {
        const auto response = Authorized(L"POST", "/api/v1/rooms/" + roomId + "/categories",
                                         nlohmann::json{{"name", name}, {"position", 0}}.dump());
        if (response.status != 201) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::RenameChannelCategory(const std::string& roomId, const std::string& categoryId,
                                        const std::string& name, std::string& error) {
    try {
        const auto response = Authorized(L"PATCH", "/api/v1/rooms/" + roomId + "/categories/" + categoryId,
                                         nlohmann::json{{"name", name}}.dump());
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::DeleteChannelCategory(const std::string& roomId, const std::string& categoryId,
                                        std::string& error) {
    try {
        const auto response = Authorized(L"DELETE", "/api/v1/rooms/" + roomId + "/categories/" + categoryId);
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::CreateRoomChannel(const std::string& roomId, const std::string& categoryId,
                                    const std::string& type, const std::string& name,
                                    const std::string& contentRating,
                                    const std::string& mediaPostingPolicy,
                                    const bool localMediaScanRequired, std::string& error) {
    try {
        nlohmann::json body{
            {"type", type},
            {"name", name},
            {"topic", ""},
            {"position", 0},
            {"contentRating", contentRating},
            {"mediaPostingPolicy", mediaPostingPolicy},
            {"localMediaScanRequired", localMediaScanRequired},
        };
        if (!categoryId.empty()) body["categoryId"] = categoryId;
        const auto response = Authorized(L"POST", "/api/v1/rooms/" + roomId + "/channels", body.dump());
        if (response.status != 201) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::RenameRoomChannel(const std::string& roomId, const std::string& channelId,
                                    const std::string& name, std::string& error) {
    try {
        const auto response = Authorized(L"PATCH", "/api/v1/rooms/" + roomId + "/channels/" + channelId,
                                         nlohmann::json{{"name", name}}.dump());
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::UpdateRoomChannelSafety(const std::string& roomId, const std::string& channelId,
                                          const std::string& contentRating,
                                          const std::string& mediaPostingPolicy,
                                          const bool localMediaScanRequired, std::string& error) {
    try {
        const auto response = Authorized(
            L"PATCH", "/api/v1/rooms/" + roomId + "/channels/" + channelId,
            nlohmann::json{{"contentRating", contentRating},
                           {"mediaPostingPolicy", mediaPostingPolicy},
                           {"localMediaScanRequired", localMediaScanRequired}}.dump());
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::DeleteRoomChannel(const std::string& roomId, const std::string& channelId,
                                    std::string& error) {
    try {
        const auto response = Authorized(L"DELETE", "/api/v1/rooms/" + roomId + "/channels/" + channelId);
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

std::vector<PlatformMember> PlatformApi::RoomMembers(const std::string& roomId, std::string& error) {
    try {
        const HttpResponse response = Authorized(L"GET", "/api/v1/rooms/" + roomId); if (response.status != 200) { error = ErrorFrom(response); return {}; }
        std::vector<PlatformMember> members;
        const auto json = nlohmann::json::parse(response.body);
        for (const auto& member : json.at("members")) {
            members.push_back({JsonStringOr(member, "id"), JsonStringOr(member, "username"),
                               JsonStringOr(member, "nickname"), JsonStringOr(member, "role", "member")});
        }
        return members;
    } catch (const std::exception& exception) { error = exception.what(); return {}; }
}

bool PlatformApi::JoinRoomCode(const std::string& code, std::string& error) {
    try { const auto response = Authorized(L"POST", "/api/v1/rooms/join-code", nlohmann::json{{"code", code}}.dump()); if (response.status != 201) { error = ErrorFrom(response); return false; } return true; }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::CreateRoom(const std::string& name, const std::string& description,
                             std::string& createdRoomId, std::string& error) {
    try {
        const auto response = Authorized(L"POST", "/api/v1/rooms", nlohmann::json{
            {"name", name}, {"description", description}, {"region", "auto"}, {"messageRetentionDays", 365}}.dump());
        if (response.status != 201) { error = ErrorFrom(response); return false; }
        createdRoomId = JsonStringOr(nlohmann::json::parse(response.body), "id");
        if (createdRoomId.empty()) { error = "Sunucu olusturulan oda kimligini dondurmedi."; return false; }
        return true;
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::UpdateRoomDenoise(const std::string& roomId, const bool enabled, std::string& error) {
    try {
        const auto response = Authorized(L"PATCH", "/api/v1/rooms/" + roomId + "/settings",
                                         nlohmann::json{{"serverDenoiseEnabled", enabled}}.dump());
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::UpdateRoomMemberRole(const std::string& roomId, const std::string& userId,
                                       const std::string& role, std::string& error) {
    try {
        const auto response = Authorized(L"PATCH", "/api/v1/rooms/" + roomId + "/members/" + userId,
                                         nlohmann::json{{"role", role}}.dump());
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::BanRoomMember(const std::string& roomId, const std::string& userId,
                                const std::string& reason, std::string& error) {
    try {
        const auto response = Authorized(L"POST", "/api/v1/rooms/" + roomId + "/bans",
                                         nlohmann::json{{"userId", userId}, {"reason", reason}}.dump());
        if (response.status != 201) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

std::optional<RoomInvite> PlatformApi::CreateRoomInvite(const std::string& roomId,
                                                         const int expiresInHours,
                                                         const int maxUses,
                                                         std::string& error) {
    try {
        const auto response = Authorized(
            L"POST", "/api/v1/rooms/" + roomId + "/invites",
            nlohmann::json{{"expiresInHours", expiresInHours}, {"maxUses", maxUses}}.dump());
        if (response.status != 201) { error = ErrorFrom(response); return std::nullopt; }
        const auto json = nlohmann::json::parse(response.body);
        return RoomInvite{JsonStringOr(json, "code"), JsonStringOr(json, "url"), JsonStringOr(json, "deepLink")};
    } catch (const std::exception& exception) { error = exception.what(); return std::nullopt; }
}

std::optional<VoiceGrant> PlatformApi::RequestVoiceGrant(const std::string& roomId, const bool serverDenoise,
                                                         const bool p2pEnabled, std::string& error,
                                                         const std::string& channelId) {
    try {
        nlohmann::json body{{"roomId", roomId}, {"regionLatency", nlohmann::json::object()},
                            {"serverDenoiseRequested", serverDenoise}, {"p2pEnabled", p2pEnabled}};
        if (!channelId.empty()) body["channelId"] = channelId;
        const auto response = Authorized(L"POST", "/api/v1/voice/join-grant", body.dump());
        DiagnosticLog("voice-grant", "http=" + std::to_string(response.status));
        if (response.status != 200) { error = ErrorFrom(response); DiagnosticLog("voice-grant", "error=" + error); return std::nullopt; }
        const auto json = nlohmann::json::parse(response.body);
        const VoiceGrant result{JsonStringOr(json, "grant"), JsonStringOr(json, "host"),
                                JsonStringOr(json, "roomId", roomId), JsonStringOr(json, "channelId", channelId),
                                JsonIntegerOr<unsigned short>(json, "port", 25565),
                                JsonBooleanOr(json, "serverDenoise"), JsonIntegerOr<int>(json, "bitrate", 24000),
                                JsonStringOr(json, "certificateFingerprint"),
                                JsonStringOr(json, "serverDenoiseReason", "client_not_requested"),
                                JsonStringOr(json, "denoiseMode", JsonBooleanOr(json, "serverDenoise") ? "server" : "none"),
                                JsonStringOr(json, "routeType", "relay"),
                                JsonBooleanOr(json, "p2pEnabled"),
                                JsonBooleanOr(json, "canSpeak", true),
                                JsonStringOr(json, "stunHost"), JsonIntegerOr<unsigned short>(json, "stunPort", 3478)};
        DiagnosticLog("voice-grant", "endpoint-assigned port=" + std::to_string(result.port));
        return result;
    } catch (const std::exception& exception) { error = exception.what(); DiagnosticLog("voice-grant", "exception=" + error); return std::nullopt; }
}

std::vector<PlatformFriend> PlatformApi::Friends(std::string& error) {
    try {
        const auto response = Authorized(L"GET", "/api/v1/friends");
        if (response.status != 200) { error = ErrorFrom(response); return {}; }
        std::vector<PlatformFriend> result;
        const auto json = nlohmann::json::parse(response.body);
        for (const auto& value : json.at("friends")) {
            result.push_back({JsonStringOr(value, "id"), JsonStringOr(value, "username"),
                              JsonStringOr(value, "nickname"), JsonStringOr(value, "state"),
                              JsonStringOr(value, "requesterId")});
        }
        return result;
    } catch (const std::exception& exception) { error = exception.what(); return {}; }
}

std::vector<PlatformFriend> PlatformApi::SearchUsers(const std::string& query, std::string& error) {
    try {
        const auto response = Authorized(L"GET", "/api/v1/friends/search?q=" + UrlEncode(query));
        if (response.status != 200) { error = ErrorFrom(response); return {}; }
        std::vector<PlatformFriend> result;
        const auto json = nlohmann::json::parse(response.body);
        for (const auto& value : json.at("users")) {
            result.push_back({JsonStringOr(value, "id"), JsonStringOr(value, "username"),
                              JsonStringOr(value, "nickname"), "search", ""});
        }
        return result;
    } catch (const std::exception& exception) { error = exception.what(); return {}; }
}

bool PlatformApi::SendFriendRequest(const std::string& username, std::string& error) {
    try {
        const auto response = Authorized(L"POST", "/api/v1/friend-requests", nlohmann::json{{"username", username}}.dump());
        if (response.status != 201) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::AcceptFriendRequest(const std::string& userId, std::string& error) {
    try {
        const auto response = Authorized(L"POST", "/api/v1/friend-requests/" + userId + "/accept");
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::DismissFriendRequest(const std::string& userId, std::string& error) {
    try {
        const auto response = Authorized(L"DELETE", "/api/v1/friend-requests/" + userId);
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::RemoveFriend(const std::string& userId, std::string& error) {
    try {
        const auto response = Authorized(L"DELETE", "/api/v1/friends/" + userId);
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::BlockUser(const std::string& userId, std::string& error) {
    try {
        const auto response = Authorized(L"POST", "/api/v1/blocks", nlohmann::json{{"userId", userId}}.dump());
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

std::optional<std::string> PlatformApi::OpenDirectConversation(const std::string& userId, std::string& error) {
    try {
        const auto response = Authorized(L"POST", "/api/v1/conversations/direct", nlohmann::json{{"userId", userId}}.dump());
        if (response.status != 200 && response.status != 201) { error = ErrorFrom(response); return std::nullopt; }
        return JsonStringOr(nlohmann::json::parse(response.body), "conversationId");
    } catch (const std::exception& exception) { error = exception.what(); return std::nullopt; }
}

std::vector<PlatformNotification> PlatformApi::Notifications(std::string& error) {
    try {
        const auto response = Authorized(L"GET", "/api/v1/notifications");
        if (response.status != 200) { error = ErrorFrom(response); return {}; }
        std::vector<PlatformNotification> result;
        const auto json = nlohmann::json::parse(response.body);
        for (const auto& value : json.at("notifications")) {
            const bool read = value.contains("readAt") && !value.at("readAt").is_null();
            result.push_back({JsonStringOr(value, "id"), JsonStringOr(value, "kind"),
                              JsonStringOr(value, "title"), JsonStringOr(value, "body"),
                              read, JsonStringOr(value, "createdAt")});
        }
        return result;
    } catch (const std::exception& exception) { error = exception.what(); return {}; }
}

bool PlatformApi::MarkNotificationRead(const std::string& notificationId, std::string& error) {
    try {
        const auto response = Authorized(L"POST", "/api/v1/notifications/" + notificationId + "/read");
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

std::vector<PlatformSession> PlatformApi::Sessions(std::string& error) {
    try {
        const auto response = Authorized(L"GET", "/api/v1/sessions");
        if (response.status != 200) { error = ErrorFrom(response); return {}; }
        std::vector<PlatformSession> result;
        const auto json = nlohmann::json::parse(response.body);
        for (const auto& value : json.at("sessions")) {
            result.push_back({JsonStringOr(value, "id"), JsonStringOr(value, "deviceName"),
                              JsonStringOr(value, "userAgent"), JsonStringOr(value, "createdAt"),
                              JsonStringOr(value, "lastSeenAt")});
        }
        return result;
    } catch (const std::exception& exception) { error = exception.what(); return {}; }
}

bool PlatformApi::RevokeSession(const std::string& sessionId, std::string& error) {
    try {
        const auto response = Authorized(L"DELETE", "/api/v1/sessions/" + sessionId);
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

std::vector<PlatformClientDevice> PlatformApi::ClientDevices(std::string& error) {
    try {
        const auto response = Authorized(L"GET", "/api/v1/me/client-devices");
        if (response.status != 200) { error = ErrorFrom(response); return {}; }
        std::vector<PlatformClientDevice> result;
        const auto json = nlohmann::json::parse(response.body);
        for (const auto& value : json.at("devices")) {
            result.push_back({JsonStringOr(value, "deviceId"), JsonStringOr(value, "platform"),
                              JsonStringOr(value, "clientVersion"), JsonStringOr(value, "lastSeenAt"),
                              JsonStringOr(value, "certificateExpiresAt"),
                              JsonIntegerOr<std::uint32_t>(value, "activeSessions")});
        }
        return result;
    } catch (const std::exception& exception) { error = exception.what(); return {}; }
}

bool PlatformApi::RevokeClientDevice(const std::string& deviceId, std::string& error) {
    try {
        const auto response = Authorized(L"DELETE", "/api/v1/me/client-devices/" + deviceId);
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::EnsureMessageDevice(const MessageCrypto& crypto, std::string& error) {
    try {
        const nlohmann::json body{{"deviceId", crypto.DeviceId()}, {"name", "Windows Native Client"},
            {"encryptionPublicKey", crypto.EncryptionPublicKey()}, {"signingPublicKey", crypto.SigningPublicKey()},
            {"accountVaultEnvelope", crypto.AccountVaultEnvelope()}};
        const auto response = Authorized(L"POST", "/api/v1/message-keys/devices", body.dump());
        if (response.status != 201) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

std::optional<RealtimeGrant> PlatformApi::RequestRealtimeGrant(std::string& error) {
    try {
        const auto response = Authorized(L"POST", "/api/v1/realtime/grant");
        if (response.status != 200) { error = ErrorFrom(response); return std::nullopt; }
        const auto json = nlohmann::json::parse(response.body);
        return RealtimeGrant{JsonStringOr(json, "ticket"), JsonStringOr(json, "websocketUrl"),
                             JsonStringOr(json, "expiresAt")};
    } catch (const std::exception& exception) { error = exception.what(); return std::nullopt; }
}

std::vector<PlatformConversation> PlatformApi::Conversations(std::string& error) {
    try {
        const auto response = Authorized(L"GET", "/api/v1/conversations");
        if (response.status != 200) { error = ErrorFrom(response); return {}; }
        std::vector<PlatformConversation> result;
        const auto json = nlohmann::json::parse(response.body);
        for (const auto& value : json.at("conversations")) {
            result.push_back({JsonStringOr(value, "id"), JsonStringOr(value, "kind"), JsonStringOr(value, "roomId"),
                              JsonStringOr(value, "roomName"), JsonIntegerOr<int>(value, "currentEpoch", 1),
                              JsonIntegerOr<std::uint32_t>(value, "unreadCount"),
                              JsonStringOr(value, "lastMessageId"), JsonStringOr(value, "lastMessageAt"),
                              JsonStringOr(value, "directPeerId"), JsonStringOr(value, "directPeerUsername"),
                              JsonStringOr(value, "directPeerNickname")});
        }
        return result;
    } catch (const std::exception& exception) { error = exception.what(); return {}; }
}

std::vector<MessageDevice> PlatformApi::ConversationDevices(const std::string& conversationId, std::string& error) {
    try {
        const auto response = Authorized(L"GET", "/api/v1/conversations/" + conversationId + "/member-devices");
        if (response.status != 200) { error = ErrorFrom(response); return {}; }
        std::vector<MessageDevice> result;
        const auto json = nlohmann::json::parse(response.body);
        for (const auto& value : json.at("devices")) result.push_back({
            JsonStringOr(value, "deviceId"), JsonStringOr(value, "userId"),
            JsonStringOr(value, "encryptionPublicKey"), JsonStringOr(value, "signingPublicKey"),
            JsonBooleanOr(value, "activeRecipient", true)});
        return result;
    } catch (const std::exception& exception) { error = exception.what(); return {}; }
}

std::optional<std::string> PlatformApi::ConversationKeyEnvelope(const std::string& conversationId, const int epoch,
                                                                 const std::string& deviceId, std::string& error) {
    try {
        const auto path = "/api/v1/conversations/" + conversationId + "/key-envelope?epoch=" + std::to_string(epoch) + "&deviceId=" + deviceId;
        const auto response = Authorized(L"GET", path);
        if (response.status == 404) return std::nullopt;
        if (response.status != 200) { error = ErrorFrom(response); return std::nullopt; }
        return JsonStringOr(nlohmann::json::parse(response.body), "envelope");
    } catch (const std::exception& exception) { error = exception.what(); return std::nullopt; }
}

std::optional<std::string> PlatformApi::LegalEscrowPublicKey(std::string& error) {
    try {
        const auto response = Authorized(L"GET", "/api/v1/message-keys/config");
        if (response.status != 200) { error = ErrorFrom(response); return std::nullopt; }
        const auto json = nlohmann::json::parse(response.body);
        if (!json.contains("legalEscrowPublicKey") || json["legalEscrowPublicKey"].is_null()) return std::nullopt;
        return json["legalEscrowPublicKey"].get<std::string>();
    } catch (const std::exception& exception) { error = exception.what(); return std::nullopt; }
}

bool PlatformApi::UploadKeyEnvelopes(const std::string& conversationId, const int epoch, const bool initialize,
                                     const std::vector<KeyEnvelopeUpload>& envelopes, std::string& error) {
    try {
        nlohmann::json values = nlohmann::json::array();
        for (const auto& envelope : envelopes) values.push_back({{"recipientType", envelope.recipientType}, {"recipientId", envelope.recipientId}, {"envelope", envelope.envelope}});
        const auto response = Authorized(L"POST", "/api/v1/conversations/" + conversationId + "/key-envelopes",
                                         nlohmann::json{{"epoch", epoch}, {"initialize", initialize}, {"envelopes", values}}.dump());
        if (response.status != 201) {
            error = ErrorFrom(response);
            DiagnosticLog("messaging", "key_envelopes status=" + std::to_string(response.status)
                + " code=" + error.substr(0, 96));
            return false;
        }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::SendEncryptedMessage(const EncryptedPlatformMessage& message, std::string& error) {
    try {
        nlohmann::json body{{"id", message.id}, {"deviceId", message.deviceId}, {"epoch", message.epoch},
            {"clientSequence", message.clientSequence}, {"eventType", message.eventType}, {"ciphertext", message.ciphertext},
            {"nonce", message.nonce}, {"signature", message.signature}};
        if (!message.replyTo.empty()) body["replyTo"] = message.replyTo;
        if (!message.channelId.empty()) body["channelId"] = message.channelId;
        if (!message.targetMessageId.empty()) body["targetMessageId"] = message.targetMessageId;
        if (message.characterCount != 0) body["characterCount"] = message.characterCount;
        if (!message.reaction.empty()) body["reaction"] = message.reaction;
        if (!message.moderationReason.empty()) body["moderationReason"] = message.moderationReason;
        if (!message.mentions.empty()) body["mentions"] = message.mentions;
        if (!message.attachmentIds.empty()) body["attachmentIds"] = message.attachmentIds;
        const auto response = Authorized(L"POST", "/api/v1/conversations/" + message.conversationId + "/messages", body.dump());
        if (response.status != 200 && response.status != 201) {
            error = ErrorFrom(response);
            DiagnosticLog("messaging", "send status=" + std::to_string(response.status)
                + " code=" + error.substr(0, 96));
            return false;
        }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

std::optional<std::uint64_t> PlatformApi::DeviceMessageSequence(const std::string& conversationId,
                                                                 const std::string& deviceId, std::string& error) {
    try {
        const auto response = Authorized(L"GET", "/api/v1/conversations/" + conversationId +
            "/device-sequence?deviceId=" + deviceId);
        if (response.status != 200) { error = ErrorFrom(response); return std::nullopt; }
        return JsonIntegerOr<std::uint64_t>(nlohmann::json::parse(response.body), "sequence");
    } catch (const std::exception& exception) { error = exception.what(); return std::nullopt; }
}

std::vector<EncryptedPlatformMessage> PlatformApi::SyncMessages(const std::string& conversationId, std::string& error) {
    return SyncMessagePage(conversationId, {}, {}, error).messages;
}

MessageSyncPage PlatformApi::SyncMessagePage(const std::string& conversationId, const std::string& afterCursor,
                                              const std::string& beforeCursor, std::string& error,
                                              const std::string& channelId) {
    try {
        std::string path = "/api/v1/messages/sync?conversationId=" + conversationId + "&limit=50";
        if (!channelId.empty()) path += "&channelId=" + UrlEncode(channelId);
        if (!afterCursor.empty()) path += "&afterCursor=" + UrlEncode(afterCursor);
        if (!beforeCursor.empty()) path += "&beforeCursor=" + UrlEncode(beforeCursor);
        const auto response = Authorized(L"GET", path);
        if (response.status != 200) { error = ErrorFrom(response); return {}; }
        const auto json = nlohmann::json::parse(response.body);
        MessageSyncPage result;
        result.beforeCursor = JsonStringOr(json, "beforeCursor");
        result.afterCursor = JsonStringOr(json, "afterCursor");
        result.hasMore = JsonBooleanOr(json, "hasMore");
        for (const auto& value : json.at("messages")) result.messages.push_back({
            JsonStringOr(value, "id"), conversationId, JsonStringOr(value, "senderId"),
            JsonStringOr(value, "deviceId"), JsonIntegerOr<int>(value, "epoch", 1),
            JsonIntegerOr<std::uint64_t>(value, "clientSequence"), JsonStringOr(value, "eventType", "message"),
            JsonStringOr(value, "ciphertext"), JsonStringOr(value, "nonce"), JsonStringOr(value, "signature"),
            JsonStringOr(value, "replyTo"), JsonStringOr(value, "createdAt"),
            JsonStringOr(value, "targetMessageId"), JsonStringOr(value, "reaction"),
            JsonStringOr(value, "moderationReason"), JsonIntegerOr<std::uint32_t>(value, "characterCount"),
            JsonStringOr(value, "channelId"), JsonIntegerOr<int>(value, "signatureVersion"),
            JsonStringArrayOrEmpty(value, "mentions"), JsonStringArrayOrEmpty(value, "attachmentIds")});
        return result;
    } catch (const std::exception& exception) { error = exception.what(); return {}; }
}

bool PlatformApi::MarkChannelRead(const std::string& channelId, const std::string& messageId, std::string& error) {
    try {
        const auto response = Authorized(L"PUT", "/api/v1/channels/" + channelId + "/read",
                                         nlohmann::json{{"messageId", messageId}}.dump());
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

std::vector<std::string> PlatformApi::ChannelPinIds(const std::string& channelId, std::string& error) {
    try {
        const auto response = Authorized(L"GET", "/api/v1/channels/" + channelId + "/pins");
        if (response.status != 200) { error = ErrorFrom(response); return {}; }
        std::vector<std::string> ids;
        const auto json = nlohmann::json::parse(response.body);
        for (const auto& pin : json.at("pins")) {
            const std::string id = JsonStringOr(pin, "messageId");
            if (!id.empty()) ids.push_back(id);
        }
        return ids;
    } catch (const std::exception& exception) { error = exception.what(); return {}; }
}

bool PlatformApi::PinChannelMessage(const std::string& channelId, const std::string& messageId, std::string& error) {
    try {
        const auto response = Authorized(L"POST", "/api/v1/channels/" + channelId + "/pins/" + messageId);
        if (response.status != 201) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::UnpinChannelMessage(const std::string& channelId, const std::string& messageId, std::string& error) {
    try {
        const auto response = Authorized(L"DELETE", "/api/v1/channels/" + channelId + "/pins/" + messageId);
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::SetChannelNotifications(const std::string& channelId, const std::string& mode,
                                          std::string& error, const int muteMinutes) {
    try {
        const std::string muteUntil = mode == "muted" ? IsoAfterMinutes(muteMinutes) : std::string{};
        const auto response = Authorized(L"PUT", "/api/v1/channels/" + channelId + "/notifications",
                                         nlohmann::json{{"mode", mode},
                                             {"muteUntil", muteUntil.empty() ? nlohmann::json(nullptr)
                                                                             : nlohmann::json(muteUntil)}}.dump());
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

std::optional<MediaUploadGrant> PlatformApi::InitiateMediaAttachment(
    const MediaAttachmentDraft& draft, std::string& error) {
    try {
        nlohmann::json body{
            {"id", draft.id},
            {"conversationId", draft.conversationId},
            {"sizeBytes", draft.sizeBytes},
            {"mimeHint", draft.mimeHint},
            {"ciphertextSha256", draft.ciphertextSha256},
            {"metadataCiphertext", draft.metadataCiphertext},
            {"metadataNonce", draft.metadataNonce},
            {"localScan", {
                {"modelVersion", draft.localScanModel},
                {"verdict", draft.localScanVerdict},
                {"digest", draft.localScanDigest},
            }},
        };
        if (!draft.channelId.empty()) body["channelId"] = draft.channelId;
        const auto response = Authorized(L"POST", "/api/v1/media/attachments/initiate", body.dump());
        if (response.status != 201) { error = ErrorFrom(response); return std::nullopt; }
        const auto json = nlohmann::json::parse(response.body);
        MediaUploadGrant grant;
        grant.id = JsonStringOr(json, "id");
        grant.uploadUrl = JsonStringOr(json, "uploadUrl");
        grant.expiresAt = JsonStringOr(json, "expiresAt");
        grant.maximumObjectBytes = JsonIntegerOr<std::uint64_t>(json, "maximumObjectBytes");
        if (json.contains("requiredHeaders") && json["requiredHeaders"].is_object()) {
            for (const auto& [key, value] : json["requiredHeaders"].items()) {
                if (value.is_string()) grant.requiredHeaders.emplace(key, value.get<std::string>());
            }
        }
        if (grant.id.empty() || grant.uploadUrl.empty()) {
            error = "Medya upload izni gecersiz";
            return std::nullopt;
        }
        return grant;
    } catch (const std::exception& exception) { error = exception.what(); return std::nullopt; }
}

bool PlatformApi::UploadMediaAttachment(const MediaUploadGrant& grant, const std::wstring& sourcePath,
                                        std::string& error) {
    try {
        std::map<std::wstring, std::wstring> headers;
        for (const auto& [key, value] : grant.requiredHeaders) {
            headers.emplace(Utf8ToWide(key), Utf8ToWide(value));
        }
        const auto status = http_.UploadFile(grant.uploadUrl, sourcePath, headers,
            grant.maximumObjectBytes == 0 ? 100U * 1024U * 1024U : static_cast<std::size_t>(grant.maximumObjectBytes));
        if (status != 200 && status != 201 && status != 204) {
            error = "Medya nesne deposu HTTP " + std::to_string(status);
            return false;
        }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::CompleteMediaAttachment(const std::string& id, std::string& error) {
    try {
        const auto response = Authorized(L"POST", "/api/v1/media/attachments/" + id + "/complete", "{}");
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

std::optional<MediaDownloadGrant> PlatformApi::MediaAttachmentDownload(
    const std::string& id, std::string& error) {
    try {
        const auto response = Authorized(L"GET", "/api/v1/media/attachments/" + id + "/download");
        if (response.status != 200) { error = ErrorFrom(response); return std::nullopt; }
        const auto json = nlohmann::json::parse(response.body);
        return MediaDownloadGrant{
            JsonStringOr(json, "id"),
            JsonStringOr(json, "downloadUrl"),
            JsonStringOr(json, "expiresAt"),
            JsonIntegerOr<std::uint64_t>(json, "sizeBytes"),
            JsonStringOr(json, "ciphertextSha256"),
            JsonStringOr(json, "metadataCiphertext"),
            JsonStringOr(json, "metadataNonce"),
            JsonStringOr(json, "mimeHint"),
        };
    } catch (const std::exception& exception) { error = exception.what(); return std::nullopt; }
}

bool PlatformApi::DownloadMediaAttachment(const MediaDownloadGrant& grant, const std::wstring& targetPath,
                                          std::string& error) {
    try {
        const auto limit = grant.sizeBytes == 0 ? 100U * 1024U * 1024U
                                               : static_cast<std::size_t>(grant.sizeBytes);
        const auto status = http_.DownloadToFile(grant.downloadUrl, targetPath, limit);
        if (status != 200) {
            error = "Medya indirme HTTP " + std::to_string(status);
            return false;
        }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

std::optional<GuardianClientModel> PlatformApi::LatestGuardianClientModel(
    const std::string& channel, std::string& error) {
    try {
        const HttpResponse response = Authorized(
            L"GET", "/api/v1/media/client-model/latest?platform=windows&architecture=x64&channel="
                + UrlEncode(channel));
        if (response.status != 200) {
            error = ErrorFrom(response);
            return std::nullopt;
        }
        const auto root = nlohmann::json::parse(response.body);
        const auto& json = root.at("model");
        GuardianClientModel model;
        model.id = JsonStringOr(json, "id");
        model.name = JsonStringOr(json, "name");
        model.platform = JsonStringOr(json, "platform");
        model.architecture = JsonStringOr(json, "architecture");
        model.engine = JsonStringOr(json, "engine");
        model.channel = JsonStringOr(json, "channel");
        model.version = JsonStringOr(json, "version");
        model.artifactUrl = JsonStringOr(json, "artifactUrl");
        model.artifactSize = json.value("artifactSize", std::uint64_t{});
        model.sha256 = JsonStringOr(json, "sha256");
        model.signatureBase64 = JsonStringOr(json, "signatureBase64");
        model.minimumClientVersion = JsonStringOr(json, "minimumClientVersion");
        model.reviewThreshold = json.value("reviewThreshold", 0.45F);
        model.rejectThreshold = json.value("rejectThreshold", 0.85F);
        model.criticalThreshold = json.value("criticalThreshold", 0.95F);
        model.rolloutPercent = json.value("rolloutPercent", 0);
        model.required = json.value("required", false);
        if (json.contains("labelMap") && json["labelMap"].is_object()) {
            for (const auto& [key, value] : json["labelMap"].items()) {
                if (value.is_string()) model.labelMap.emplace(key, value.get<std::string>());
            }
        }
        if (model.id.empty() || model.platform != "windows" || model.architecture != "x64"
            || model.channel != channel || model.version.empty()
            || !model.artifactUrl.starts_with("https://") || model.artifactSize == 0
            || model.sha256.size() != 64U || model.signatureBase64.empty()
            || model.labelMap.size() < 2U
            || !(model.reviewThreshold < model.rejectThreshold
                 && model.rejectThreshold < model.criticalThreshold)) {
            error = "Guardian model manifesti gecersiz";
            return std::nullopt;
        }
        return model;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

bool PlatformApi::DownloadGuardianClientModel(const GuardianClientModel& model,
                                              const std::wstring& targetPath,
                                              std::string& error) {
    try {
        const unsigned long status = http_.DownloadToFile(
            model.artifactUrl, targetPath,
            static_cast<std::size_t>(std::min<std::uint64_t>(
                model.artifactSize + 1U, 2U * 1024U * 1024U * 1024U)));
        if (status != 200) {
            error = "Guardian modeli indirilemedi (HTTP " + std::to_string(status) + ")";
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

std::optional<ClientExperiencePolicy> PlatformApi::ClientExperience(std::string& error) {
    try {
        const HttpResponse response = Authorized(L"GET", "/api/v1/client-experience");
        if (response.status != 200) {
            error = ErrorFrom(response);
            return std::nullopt;
        }
        const auto root = nlohmann::json::parse(response.body);
        const auto& json = root.at("settings");
        ClientExperiencePolicy policy;
        policy.policyVersion = std::max(1, JsonIntegerOr<int>(json, "policyVersion", 1));
        policy.defaultTheme = ParseUiTheme(JsonStringOr(json, "defaultTheme", "aurora_dark"));
        policy.allowUserThemeChoice = JsonBooleanOr(json, "allowUserThemeChoice", true);
        policy.defaultResourceProfile =
            ParseResourceProfile(JsonStringOr(json, "defaultResourceProfile", "balanced"));
        policy.allowUserResourceProfileChoice =
            JsonBooleanOr(json, "allowUserResourceProfileChoice", true);
        policy.animationsEnabled = JsonBooleanOr(json, "animationsEnabled", true);
        policy.backgroundEffectsEnabled = JsonBooleanOr(json, "backgroundEffectsEnabled", true);
        policy.customAccentsEnabled = JsonBooleanOr(json, "customAccentsEnabled", true);
        policy.accentHex = JsonStringOr(json, "accentHex", "#1F8FFF");
        policy.focusedVoiceFps = std::clamp(JsonIntegerOr<int>(json, "focusedVoiceFps", 15), 5, 30);
        policy.unfocusedFps = std::clamp(JsonIntegerOr<int>(json, "unfocusedFps", 4), 1, 10);
        policy.maximumResolvedMessages = static_cast<std::size_t>(
            std::clamp(JsonIntegerOr<int>(json, "maximumResolvedMessages", 300), 50, 500));
        policy.maximumImageCacheMb = static_cast<std::size_t>(
            std::clamp(JsonIntegerOr<int>(json, "maximumImageCacheMb", 32), 0, 128));
        policy.mediaAttachmentsEnabled = JsonBooleanOr(json, "mediaAttachmentsEnabled", true);
        policy.enhancedPresenceEnabled = JsonBooleanOr(json, "enhancedPresenceEnabled", true);
        policy.quickSwitcherEnabled = JsonBooleanOr(json, "quickSwitcherEnabled", true);
        policy.experimentalAecEnabled = JsonBooleanOr(json, "experimentalAecEnabled");
        if (policy.accentHex.size() != 7U || policy.accentHex.front() != '#') {
            error = "Aurora deneyim politikasi gecersiz";
            return std::nullopt;
        }
        return policy;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

std::optional<MediaSafetyConfig> PlatformApi::MediaConfig(std::string& error) {
    try {
        const auto response = Authorized(L"GET", "/api/v1/media/config");
        if (response.status != 200) { error = ErrorFrom(response); return std::nullopt; }
        const auto json = nlohmann::json::parse(response.body);
        MediaSafetyConfig result;
        result.moderationPublicKey = JsonStringOr(json, "moderationPublicKey");
        result.maximumImageBytes = JsonIntegerOr<std::size_t>(json, "maximumImageBytes", 8U * 1024U * 1024U);
        result.privateMediaReportingAvailable = JsonBooleanOr(json, "privateMediaReportingAvailable");
        if (json.contains("sensitiveMediaModes") && json["sensitiveMediaModes"].is_array()) {
            result.mayShowSensitiveMedia = std::ranges::any_of(
                json["sensitiveMediaModes"], [](const nlohmann::json& value) {
                    return value.is_string() && value.get<std::string>() == "show";
                });
        }
        return result;
    } catch (const std::exception& exception) { error = exception.what(); return std::nullopt; }
}

std::optional<MediaPreferences> PlatformApi::GetMediaPreferences(std::string& error) {
    try {
        const auto response = Authorized(L"GET", "/api/v1/me/media-preferences");
        if (response.status != 200) { error = ErrorFrom(response); return std::nullopt; }
        const auto json = nlohmann::json::parse(response.body);
        return MediaPreferences{
            ParseSensitiveMediaMode(JsonStringOr(json, "sensitiveMediaMode", "block")),
            JsonBooleanOr(json, "allowNonFriendMedia"),
            JsonBooleanOr(json, "lockedForMinor"),
        };
    } catch (const std::exception& exception) { error = exception.what(); return std::nullopt; }
}

bool PlatformApi::UpdateMediaPreferences(const MediaPreferences& preferences, std::string& error) {
    try {
        const auto response = Authorized(L"PUT", "/api/v1/me/media-preferences",
            nlohmann::json{{"sensitiveMediaMode", SensitiveMediaModeName(preferences.sensitiveMediaMode)},
                           {"allowNonFriendMedia", preferences.allowNonFriendMedia}}.dump());
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

std::optional<MediaReportReceipt> PlatformApi::CreateMediaReport(
    const std::string& reportedUserId,
    const std::string& assetId,
    const std::string& targetType,
    const std::string& targetId,
    const std::string& reason,
    const std::string& note,
    const std::string& evidenceSha256,
    const std::string& idempotencyKey,
    std::string& error) {
    try {
        const nlohmann::json body{
            {"reportedUserId", reportedUserId.empty() ? nlohmann::json(nullptr) : nlohmann::json(reportedUserId)},
            {"assetId", assetId.empty() ? nlohmann::json(nullptr) : nlohmann::json(assetId)},
            {"targetType", targetType},
            {"targetId", targetId},
            {"reason", reason},
            {"note", note},
            {"evidenceSha256", evidenceSha256.empty() ? nlohmann::json(nullptr) : nlohmann::json(evidenceSha256)},
            {"idempotencyKey", idempotencyKey},
        };
        const auto response = Authorized(L"POST", "/api/v1/media-reports", body.dump());
        if (response.status != 200 && response.status != 201) {
            error = ErrorFrom(response);
            return std::nullopt;
        }
        const auto json = nlohmann::json::parse(response.body);
        return MediaReportReceipt{
            JsonStringOr(json, "id"),
            JsonStringOr(json, "status"),
            JsonBooleanOr(json, "evidenceUploadRequired"),
            JsonStringOr(json, "moderationPublicKey"),
        };
    } catch (const std::exception& exception) { error = exception.what(); return std::nullopt; }
}

bool PlatformApi::UploadMediaReportEvidence(
    const std::string& reportId,
    const std::span<const std::uint8_t> ciphertext,
    const std::string& ciphertextSha256,
    const std::string& signature,
    const std::string& deviceId,
    const std::string& contentMime,
    std::string& error) {
    try {
        const std::map<std::wstring, std::wstring> headers{
            {L"Content-Type", L"application/octet-stream"},
            {L"X-Sonalis-Device-Id", Utf8ToWide(deviceId)},
            {L"X-Sonalis-Evidence-Sha256", Utf8ToWide(ciphertextSha256)},
            {L"X-Sonalis-Evidence-Signature", Utf8ToWide(signature)},
            {L"X-Sonalis-Evidence-Mime", Utf8ToWide(contentMime)},
            {L"X-Sonalis-Evidence-Version", L"x25519-xchacha20poly1305-v1"},
        };
        const std::string_view body(reinterpret_cast<const char*>(ciphertext.data()), ciphertext.size());
        const auto response = Authorized(L"PUT", "/api/v1/media-reports/" + reportId + "/evidence", body, headers);
        if (response.status != 200 && response.status != 201) {
            error = ErrorFrom(response);
            return false;
        }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

std::vector<AccountRestriction> PlatformApi::Restrictions(std::string& error) {
    try {
        const auto response = Authorized(L"GET", "/api/v1/me/restrictions");
        if (response.status != 200) { error = ErrorFrom(response); return {}; }
        std::vector<AccountRestriction> result;
        const auto json = nlohmann::json::parse(response.body);
        for (const auto& row : json.at("restrictions")) {
            if (!JsonBooleanOr(row, "active")) continue;
            result.push_back({
                JsonStringOr(row, "id"), JsonStringOr(row, "scope"), JsonStringOr(row, "reasonCode"),
                JsonStringOr(row, "startsAt"), JsonStringOr(row, "expiresAt"), false,
            });
        }
        return result;
    } catch (const std::exception& exception) { error = exception.what(); return {}; }
}

bool PlatformApi::AppealRestriction(const std::string& restrictionId,
                                    const std::string& statement,
                                    std::string& error) {
    try {
        const auto response = Authorized(L"POST", "/api/v1/me/restrictions/" + restrictionId + "/appeal",
                                         nlohmann::json{{"statement", statement}}.dump());
        if (response.status != 201) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::WithdrawMediaReport(const std::string& reportId,
                                      const std::string& reason,
                                      std::string& error) {
    try {
        const auto response = Authorized(L"POST", "/api/v1/media-reports/" + reportId + "/withdraw",
                                         nlohmann::json{{"reason", reason}}.dump());
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PlatformApi::ReportDiagnosticErrors(const std::span<const DiagnosticErrorEvent> events,
                                         std::string& error) {
    if (events.empty()) return true;
    try {
        nlohmann::json payload = nlohmann::json::array();
        for (const DiagnosticErrorEvent& event : events) {
            payload.push_back({
                {"component", event.component},
                {"errorCode", event.errorCode},
                {"severity", event.severity},
                {"context", event.context},
                {"occurredAt", IsoTimestamp(event.timestampMs)},
                {"version", kClientVersion},
                {"osVersion", "Windows 10/11 x64"},
                {"occurrences", event.occurrences},
            });
        }
        const HttpResponse response = Authorized(
            L"POST", "/api/v1/diagnostics/errors",
            nlohmann::json{{"events", std::move(payload)}}.dump());
        if (response.status != 202) {
            error = ErrorFrom(response);
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool PlatformApi::MarkConversationRead(const std::string& conversationId, const std::string& messageId, std::string& error) {
    try {
        const auto response = Authorized(L"PUT", "/api/v1/conversations/" + conversationId + "/read",
                                         nlohmann::json{{"messageId", messageId}}.dump());
        if (response.status != 200) { error = ErrorFrom(response); return false; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

}  // namespace ss
