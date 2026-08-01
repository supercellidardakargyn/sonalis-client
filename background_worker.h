#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <condition_variable>
#include <array>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

namespace ss {

class BackgroundWorker final {
public:
    using Completion = std::function<void()>;
    using Task = std::function<Completion()>;

    BackgroundWorker();
    ~BackgroundWorker();
    BackgroundWorker(const BackgroundWorker&) = delete;
    BackgroundWorker& operator=(const BackgroundWorker&) = delete;

    void SetWakeEvent(HANDLE event) noexcept;
    void SetExceptionHandler(std::function<void(std::string)> handler);
    void Shutdown() noexcept;
    bool Submit(Task task);
    void DrainCompletions();
    [[nodiscard]] std::size_t Pending() const noexcept;

private:
    void Run(std::stop_token stopToken);

    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<Task> tasks_;
    std::deque<Completion> completions_;
    std::function<void(std::string)> exceptionHandler_;
    static constexpr std::size_t kThreadCount = 3;
    std::array<std::jthread, kThreadCount> threads_{};
    HANDLE wakeEvent_{};
};

}  // namespace ss
