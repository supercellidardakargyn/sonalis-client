#include "background_worker.h"

#include <exception>
#include <string>
#include <utility>

#include "performance.h"

namespace ss {

BackgroundWorker::BackgroundWorker() {
    for (auto& thread : threads_) {
        thread = std::jthread([this](const std::stop_token token) { Run(token); });
    }
}
BackgroundWorker::~BackgroundWorker() { Shutdown(); }
void BackgroundWorker::Shutdown() noexcept {
    for (auto& thread : threads_) if (thread.joinable()) thread.request_stop();
    wake_.notify_all();
    for (auto& thread : threads_) if (thread.joinable()) thread.join();
}

void BackgroundWorker::SetWakeEvent(const HANDLE event) noexcept {
    std::scoped_lock lock(mutex_);
    wakeEvent_ = event;
}

void BackgroundWorker::SetExceptionHandler(std::function<void(std::string)> handler) {
    std::scoped_lock lock(mutex_);
    exceptionHandler_ = std::move(handler);
}

bool BackgroundWorker::Submit(Task task) {
    if (!task) return false;
    {
        std::scoped_lock lock(mutex_);
        if (tasks_.size() >= 32) return false;
        tasks_.push_back(std::move(task));
        RecordPerformance(PerformanceMetric::WorkerQueue, static_cast<std::uint32_t>(tasks_.size()));
    }
    wake_.notify_one();
    return true;
}

void BackgroundWorker::DrainCompletions() {
    std::deque<Completion> ready;
    { std::scoped_lock lock(mutex_); ready.swap(completions_); }
    for (auto& completion : ready) if (completion) completion();
}

std::size_t BackgroundWorker::Pending() const noexcept {
    std::scoped_lock lock(mutex_);
    return tasks_.size();
}

void BackgroundWorker::Run(const std::stop_token stopToken) {
    SetThreadDescription(GetCurrentThread(), L"Sonalis Platform Worker");
    while (!stopToken.stop_requested()) {
        Task task;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, stopToken, [this] { return !tasks_.empty(); });
            if (stopToken.stop_requested()) break;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        Completion completion;
        try {
            completion = task();
        } catch (const std::exception& exception) {
            std::function<void(std::string)> handler;
            { std::scoped_lock lock(mutex_); handler = exceptionHandler_; }
            if (handler) {
                const std::string message = exception.what();
                completion = [handler = std::move(handler), message] { handler(message); };
            }
        } catch (...) {
            std::function<void(std::string)> handler;
            { std::scoped_lock lock(mutex_); handler = exceptionHandler_; }
            if (handler) completion = [handler = std::move(handler)] { handler("unknown_worker_exception"); };
        }
        HANDLE event = nullptr;
        {
            std::scoped_lock lock(mutex_);
            if (completion) {
                // The task queue is capped at 32 and at most three tasks are
                // running, so 64 slots preserve every state-clearing callback.
                // Dropping the oldest completion can otherwise leave a UI
                // operation permanently stuck in its pending state.
                completions_.push_back(std::move(completion));
            }
            event = wakeEvent_;
        }
        if (event != nullptr) SetEvent(event);
    }
}

}  // namespace ss
