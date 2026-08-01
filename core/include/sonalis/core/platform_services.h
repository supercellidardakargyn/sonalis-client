#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sonalis::core {

enum class HttpMethod : std::uint8_t { Get, Post, Put, Patch, Delete };

struct HttpRequest final {
    HttpMethod method{HttpMethod::Get};
    std::string path;
    std::vector<std::uint8_t> body;
    std::chrono::milliseconds timeout{10'000};
    bool authenticated{true};
};

struct HttpResponse final {
    std::uint16_t status{};
    std::vector<std::uint8_t> body;
    std::string safeErrorCode;
};

class PlatformHttpClient {
public:
    using Completion = std::function<void(HttpResponse)>;
    virtual ~PlatformHttpClient() = default;
    virtual bool Submit(HttpRequest request, Completion completion) = 0;
    virtual void CancelAll() noexcept = 0;
};

class RealtimeBackend {
public:
    using EventCallback = std::function<void(std::span<const std::uint8_t>)>;
    virtual ~RealtimeBackend() = default;
    virtual bool Connect(std::string grant, EventCallback callback, std::string& error) = 0;
    virtual void Disconnect() noexcept = 0;
    [[nodiscard]] virtual bool Connected() const noexcept = 0;
};

enum class NotificationPrivacy : std::uint8_t { FullContent, SenderOnly, Hidden };

class PlatformNotifier {
public:
    virtual ~PlatformNotifier() = default;
    virtual void Show(std::string_view title, std::string_view body,
                      std::string_view deepLink, NotificationPrivacy privacy) = 0;
    virtual void ClearConversation(std::string_view conversationId) noexcept = 0;
};

}  // namespace sonalis::core
