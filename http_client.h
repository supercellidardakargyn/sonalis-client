#pragma once

#include <map>
#include <cstddef>
#include <string>
#include <string_view>

namespace ss {

struct HttpResponse {
    unsigned long status{};
    std::string body;
};

class HttpClient final {
public:
    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpResponse Request(const std::wstring& method,
                         const std::string& url,
                         std::string_view body = {},
                         const std::map<std::wstring, std::wstring>& headers = {},
                         std::size_t maxResponseBytes = 2U * 1024U * 1024U) const;
    // Dedicated to the one-time voice-node /join request. Normal platform
    // HTTP always retains the Windows trust-store validation path above.
    HttpResponse RequestPinnedVoiceNode(const std::wstring& method,
                                        const std::string& url,
                                        const std::string& expectedLeafSha256,
                                        const std::string& body,
                                        std::size_t maxResponseBytes = 64U * 1024U) const;
    unsigned long DownloadToFile(const std::string& url,
                                 const std::wstring& targetPath,
                                 std::size_t maxResponseBytes = 200U * 1024U * 1024U) const;
    unsigned long UploadFile(const std::string& url,
                             const std::wstring& sourcePath,
                             const std::map<std::wstring, std::wstring>& headers = {},
                             std::size_t maxRequestBytes = 100U * 1024U * 1024U) const;

private:
    void* session_{};
};

}  // namespace ss
