#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "sonalis/core/bounded_spsc_queue.h"
#include "sonalis/core/platform_services.h"

namespace sonalis::linux_platform {

class CurlHttpClient final : public core::PlatformHttpClient {
public:
    explicit CurlHttpClient(std::string origin);
    ~CurlHttpClient() override;

    bool Submit(core::HttpRequest request, Completion completion) override;
    void CancelAll() noexcept override;
    void SetBearerToken(std::string token);

private:
    struct Work final { core::HttpRequest request; Completion completion; };
    void Run(std::stop_token stopToken);
    [[nodiscard]] core::HttpResponse Execute(const core::HttpRequest& request) const;

    std::string origin_;
    mutable std::mutex tokenMutex_;
    std::string bearerToken_;
    core::BoundedSpscQueue<Work, 64> queue_;
    std::mutex wakeMutex_;
    std::condition_variable_any wake_;
    std::atomic<bool> cancelled_{};
    std::jthread worker_;
};

}  // namespace sonalis::linux_platform
