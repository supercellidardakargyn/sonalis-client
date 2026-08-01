#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ss {

struct RealtimeGrant {
    std::string ticket;
    std::string websocketUrl;
    std::string expiresAt;
};

class RealtimeClient final {
public:
    using GrantProvider = std::function<std::optional<RealtimeGrant>(std::string&)>;

    RealtimeClient() = default;
    ~RealtimeClient();
    RealtimeClient(const RealtimeClient&) = delete;
    RealtimeClient& operator=(const RealtimeClient&) = delete;

    void Start(GrantProvider grantProvider, HANDLE wakeEvent);
    void Stop() noexcept;
    [[nodiscard]] bool IsConnected() const noexcept;
    [[nodiscard]] std::vector<std::string> DrainEvents();
    bool SendTyping(const std::string& conversationId, bool active, const std::string& channelId = {}) noexcept;
    bool SetPresence(const std::string& status, const std::string& customText = {}) noexcept;

private:
    void Run(std::stop_token stopToken);
    HINTERNET Connect(const RealtimeGrant& grant, std::string& error);
    void CloseTransport() noexcept;
    void QueueEvent(std::string event);

    std::jthread thread_;
    GrantProvider grantProvider_;
    HANDLE wakeEvent_{};
    std::atomic<HINTERNET> socket_{nullptr};
    std::atomic<HINTERNET> connection_{nullptr};
    std::atomic<HINTERNET> session_{nullptr};
    std::atomic<bool> connected_{false};
    std::mutex sendMutex_;
    std::mutex transportMutex_;
    std::mutex eventsMutex_;
    std::deque<std::string> events_;
};

}  // namespace ss
