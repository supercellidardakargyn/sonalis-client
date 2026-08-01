#include "realtime_client.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iterator>
#include <mutex>
#include <nlohmann/json.hpp>
#include <utility>

#include "diagnostics.h"
#include "performance.h"
#include "settings.h"

namespace ss {
namespace {

class InternetHandle final {
public:
    explicit InternetHandle(const HINTERNET value = nullptr) : value_(value) {}
    ~InternetHandle() { if (value_ != nullptr) WinHttpCloseHandle(value_); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    [[nodiscard]] HINTERNET get() const noexcept { return value_; }
    [[nodiscard]] HINTERNET release() noexcept { return std::exchange(value_, nullptr); }
private:
    HINTERNET value_{};
};

std::string WindowsError(const char* prefix) {
    return std::string(prefix) + " (" + std::to_string(GetLastError()) + ")";
}

}  // namespace

RealtimeClient::~RealtimeClient() { Stop(); }

void RealtimeClient::Start(GrantProvider grantProvider, const HANDLE wakeEvent) {
    Stop();
    grantProvider_ = std::move(grantProvider);
    wakeEvent_ = wakeEvent;
    thread_ = std::jthread([this](const std::stop_token token) { Run(token); });
}

void RealtimeClient::Stop() noexcept {
    if (thread_.joinable()) thread_.request_stop();
    CloseTransport();
    if (thread_.joinable()) thread_.join();
    connected_.store(false);
}

bool RealtimeClient::IsConnected() const noexcept { return connected_.load(); }

std::vector<std::string> RealtimeClient::DrainEvents() {
    std::deque<std::string> pending;
    { std::scoped_lock lock(eventsMutex_); pending.swap(events_); }
    return {std::make_move_iterator(pending.begin()), std::make_move_iterator(pending.end())};
}

bool RealtimeClient::SendTyping(const std::string& conversationId, const bool active,
                                const std::string& channelId) noexcept {
    try {
        nlohmann::json value{{"type", "typing"}, {"conversationId", conversationId}, {"active", active}};
        if (!channelId.empty()) value["channelId"] = channelId;
        const std::string frame = value.dump();
        std::scoped_lock lock(sendMutex_);
        const HINTERNET socket = socket_.load();
        return socket != nullptr && WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            const_cast<char*>(frame.data()), static_cast<DWORD>(frame.size())) == NO_ERROR;
    } catch (...) { return false; }
}

bool RealtimeClient::SetPresence(const std::string& status, const std::string& customText) noexcept {
    try {
        const std::string frame = nlohmann::json{
            {"type", "presence.set"}, {"status", status}, {"customText", customText},
        }.dump();
        std::scoped_lock lock(sendMutex_);
        const HINTERNET socket = socket_.load();
        return socket != nullptr && WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            const_cast<char*>(frame.data()), static_cast<DWORD>(frame.size())) == NO_ERROR;
    } catch (...) { return false; }
}

void RealtimeClient::Run(const std::stop_token stopToken) {
    SetThreadDescription(GetCurrentThread(), L"Sonalis Realtime");
    unsigned reconnectAttempt = 0;
    while (!stopToken.stop_requested()) {
        std::string error;
        const auto grant = grantProvider_ ? grantProvider_(error) : std::nullopt;
        const bool terminalSessionError = error == "session_expired"
            || error == "native_client_update_required";
        if (!grant) {
            DiagnosticLog("realtime", "grant-failed=" + (error.empty() ? std::string("unknown") : error));
            QueueEvent(nlohmann::json{{"type", "realtime.error"}, {"error", error}}.dump());
        } else {
            HINTERNET socket = Connect(*grant, error);
            if (socket != nullptr) {
                DiagnosticLog("realtime", "connected");
                connected_.store(true);
                reconnectAttempt = 0;
                std::array<char, 32 * 1024> buffer{};
                std::string assembled;
                assembled.reserve(32 * 1024);
                while (!stopToken.stop_requested()) {
                    DWORD received = 0;
                    WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
                    const DWORD result = WinHttpWebSocketReceive(socket, buffer.data(), static_cast<DWORD>(buffer.size()), &received, &type);
                    if (result != NO_ERROR) {
                        if (!stopToken.stop_requested()) {
                            DiagnosticLog("realtime", "receive-failed=" + std::to_string(result));
                        }
                        break;
                    }
                    if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                        USHORT closeStatus = 0;
                        std::array<char, 128> reason{};
                        DWORD reasonBytes = static_cast<DWORD>(reason.size());
                        const DWORD closeResult = WinHttpWebSocketQueryCloseStatus(
                            socket, &closeStatus, reason.data(), reasonBytes, &reasonBytes);
                        DiagnosticLog("realtime", "closed status=" + std::to_string(closeStatus)
                            + " query=" + std::to_string(closeResult)
                            + " reason_bytes=" + std::to_string(reasonBytes));
                        break;
                    }
                    if (received != 0) assembled.append(buffer.data(), received);
                    if (assembled.size() > 32 * 1024) {
                        DiagnosticLog("realtime", "frame-too-large");
                        break;
                    }
                    if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
                        QueueEvent(std::move(assembled));
                        assembled.clear();
                        assembled.reserve(32 * 1024);
                    }
                }
                CloseTransport();
                connected_.store(false);
            } else {
                DiagnosticLog("realtime", "connect-failed=" + (error.empty() ? std::string("unknown") : error));
                QueueEvent(nlohmann::json{{"type", "realtime.error"}, {"error", error}}.dump());
            }
        }
        if (stopToken.stop_requested()) break;
        if (terminalSessionError) {
            std::mutex terminalMutex;
            std::condition_variable_any terminalWait;
            std::unique_lock terminalLock(terminalMutex);
            terminalWait.wait(terminalLock, stopToken, [] { return false; });
            break;
        }
        RecordPerformance(PerformanceMetric::WebSocketReconnect, ++reconnectAttempt);
        const unsigned baseDelay = std::min(30'000U, 1'000U << std::min(reconnectAttempt, 5U));
        const unsigned jitterWindow = std::max(1U, baseDelay / 4U);
        const unsigned jitter = static_cast<unsigned>(
            (GetTickCount64() ^ (static_cast<ULONGLONG>(reconnectAttempt) * 0x9E3779B9ULL)) % jitterWindow);
        const auto wait = std::chrono::milliseconds(std::min(30'000U, baseDelay + jitter));
        std::mutex delayMutex;
        std::condition_variable_any delay;
        std::unique_lock delayLock(delayMutex);
        delay.wait_for(delayLock, stopToken, wait, [] { return false; });
    }
}

HINTERNET RealtimeClient::Connect(const RealtimeGrant& grant, std::string& error) {
    std::string compatibleUrl = grant.websocketUrl;
    bool secure = false;
    if (compatibleUrl.starts_with("wss://")) {
        compatibleUrl.replace(0, 6, "https://");
        secure = true;
    } else if (compatibleUrl.starts_with("ws://")) {
        compatibleUrl.replace(0, 5, "http://");
    } else if (compatibleUrl.starts_with("https://")) {
        secure = true;
    } else if (!compatibleUrl.starts_with("http://")) {
        error = "Realtime URL semasi gecersiz";
        return nullptr;
    }
    const std::wstring url = Utf8ToWide(compatibleUrl
        + (compatibleUrl.find('?') != std::string::npos ? "&ticket=" : "?ticket=") + grant.ticket);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) { error = WindowsError("Realtime URL gecersiz"); return nullptr; }
    const std::wstring userAgent = Utf8ToWide(std::string("Sonalis-Realtime/") + SONALIS_VERSION);
    InternetHandle session(WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.get()) { error = WindowsError("Realtime oturumu acilamadi"); return nullptr; }
    WinHttpSetTimeouts(session.get(), 8'000, 8'000, 8'000, 70'000);
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection.get()) { error = WindowsError("Realtime sunucusuna baglanilamadi"); return nullptr; }
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request.get() || !WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)
        || !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(request.get(), nullptr)) {
        error = WindowsError("Realtime el sikisma basarisiz"); return nullptr;
    }
    DWORD status = 0; DWORD statusBytes = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes, WINHTTP_NO_HEADER_INDEX);
    if (status != 101) {
        error = "Realtime HTTP " + std::to_string(status);
        DiagnosticLog("realtime", "upgrade-http=" + std::to_string(status));
        return nullptr;
    }
    HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request.get(), 0);
    if (socket == nullptr) error = WindowsError("Realtime WebSocket acilamadi");
    else {
        std::scoped_lock lock(transportMutex_);
        session_.store(session.release());
        connection_.store(connection.release());
        socket_.store(socket);
    }
    return socket;
}

void RealtimeClient::CloseTransport() noexcept {
    std::scoped_lock lock(sendMutex_, transportMutex_);
    const HINTERNET socket = socket_.exchange(nullptr);
    if (socket != nullptr) {
        WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(socket);
    }
    const HINTERNET connection = connection_.exchange(nullptr);
    if (connection != nullptr) WinHttpCloseHandle(connection);
    const HINTERNET session = session_.exchange(nullptr);
    if (session != nullptr) WinHttpCloseHandle(session);
}

void RealtimeClient::QueueEvent(std::string event) {
    {
        std::scoped_lock lock(eventsMutex_);
        if (events_.size() >= 256) events_.pop_front();
        events_.push_back(std::move(event));
    }
    if (wakeEvent_ != nullptr) SetEvent(wakeEvent_);
}

}  // namespace ss
