#include "sonalis/linux/curl_http_client.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <curl/curl.h>

namespace sonalis::linux_platform {
namespace {

constexpr std::size_t kMaximumResponse = 2U * 1024U * 1024U;

size_t Receive(char* data, const size_t size, const size_t count, void* context) {
    auto* output = static_cast<std::vector<std::uint8_t>*>(context);
    const std::size_t bytes = size * count;
    if (bytes > kMaximumResponse || output->size() > kMaximumResponse - bytes) return 0;
    output->insert(output->end(), reinterpret_cast<std::uint8_t*>(data),
                   reinterpret_cast<std::uint8_t*>(data) + bytes);
    return bytes;
}

bool ValidOrigin(const std::string& value) {
    return value.starts_with("https://") && value.size() > 8 && value.find('@') == std::string::npos
        && value.find_first_of("\r\n") == std::string::npos;
}

}  // namespace

CurlHttpClient::CurlHttpClient(std::string origin) : origin_(std::move(origin)) {
    if (!ValidOrigin(origin_)) throw std::invalid_argument("invalid_https_origin");
    while (!origin_.empty() && origin_.back() == '/') origin_.pop_back();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    worker_ = std::jthread([this](const std::stop_token token) { Run(token); });
}

CurlHttpClient::~CurlHttpClient() {
    CancelAll();
    worker_.request_stop();
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    curl_global_cleanup();
}

bool CurlHttpClient::Submit(core::HttpRequest request, Completion completion) {
    if (cancelled_.load(std::memory_order_acquire) || !completion || request.path.empty()
        || request.path.front() != '/' || request.path.starts_with("//")
        || request.body.size() > kMaximumResponse) return false;
    if (!queue_.TryPush({std::move(request), std::move(completion)})) return false;
    wake_.notify_one();
    return true;
}

void CurlHttpClient::CancelAll() noexcept {
    cancelled_.store(true, std::memory_order_release);
    while (auto work = queue_.TryPop()) work->completion({499, {}, "cancelled"});
}

void CurlHttpClient::SetBearerToken(std::string token) {
    if (token.size() > 8'192 || token.find_first_of("\r\n") != std::string::npos) token.clear();
    std::lock_guard lock(tokenMutex_);
    bearerToken_ = std::move(token);
}

void CurlHttpClient::Run(const std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        auto work = queue_.TryPop();
        if (!work) {
            std::unique_lock lock(wakeMutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(250));
            continue;
        }
        if (cancelled_.load(std::memory_order_acquire)) work->completion({499, {}, "cancelled"});
        else work->completion(Execute(work->request));
    }
}

core::HttpResponse CurlHttpClient::Execute(const core::HttpRequest& request) const {
    core::HttpResponse response;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) { response.safeErrorCode = "curl_init_failed"; return response; }
    const std::string target = origin_ + request.path;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
    std::string authorization;
    if (request.authenticated) {
        std::lock_guard lock(tokenMutex_);
        if (!bearerToken_.empty()) authorization = "Authorization: Bearer " + bearerToken_;
    }
    if (!authorization.empty()) headers = curl_slist_append(headers, authorization.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, target.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SonalisLinux/5.1.0");
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5'000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    switch (request.method) {
    case core::HttpMethod::Get: break;
    case core::HttpMethod::Post: curl_easy_setopt(curl, CURLOPT_POST, 1L); break;
    case core::HttpMethod::Put: curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT"); break;
    case core::HttpMethod::Patch: curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH"); break;
    case core::HttpMethod::Delete: curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE"); break;
    }
    if (!request.body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    }
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    if (result == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response.status = static_cast<std::uint16_t>(std::clamp(status, 0L, 65'535L));
    if (result != CURLE_OK) {
        response.safeErrorCode = result == CURLE_OPERATION_TIMEDOUT ? "network_timeout" : "network_failed";
        response.body.clear();
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

}  // namespace sonalis::linux_platform
