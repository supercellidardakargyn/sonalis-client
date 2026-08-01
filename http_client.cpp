#include "http_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

#include "settings.h"

namespace ss {
namespace {

struct InternetCloser { void operator()(void* value) const noexcept { if (value != nullptr) WinHttpCloseHandle(value); } };
using InternetHandle = std::unique_ptr<void, InternetCloser>;

std::runtime_error WinHttpError(const char* operation) {
    return std::runtime_error(std::string(operation) + " (WinHTTP " + std::to_string(GetLastError()) + ")");
}

std::array<std::uint8_t, 32> ParseSha256Fingerprint(const std::string& value) {
    if (value.size() != 64) throw std::runtime_error("Ses dugumu sertifika parmak izi gecersiz");
    std::array<std::uint8_t, 32> output{};
    const auto digit = [](const char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < output.size(); ++index) {
        const int high = digit(value[index * 2]);
        const int low = digit(value[index * 2 + 1]);
        if (high < 0 || low < 0) throw std::runtime_error("Ses dugumu sertifika parmak izi gecersiz");
        output[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return output;
}

void ConfigureSession(void* session, const int timeout) {
    if (session == nullptr) throw WinHttpError("HTTP oturumu acilamadi");
    DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    secureProtocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS,
                          &secureProtocols, sizeof(secureProtocols))) {
        throw WinHttpError("TLS protokolleri ayarlanamadi");
    }
    if (!WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout)) {
        throw WinHttpError("HTTP zaman asimi ayarlanamadi");
    }
}

}  // namespace

HttpClient::HttpClient() {
    const std::wstring userAgent = Utf8ToWide(std::string("Sonalis/") + SONALIS_VERSION);
    session_ = WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    try {
        ConfigureSession(session_, 10'000);
    } catch (...) {
        if (session_ != nullptr) WinHttpCloseHandle(session_);
        session_ = nullptr;
        throw;
    }
}

HttpClient::~HttpClient() {
    if (session_ != nullptr) WinHttpCloseHandle(session_);
}

HttpResponse HttpClient::Request(const std::wstring& method,
                                 const std::string& url,
                                 const std::string_view body,
                                 const std::map<std::wstring, std::wstring>& headers,
                                 const std::size_t maxResponseBytes) const {
    const std::wstring wideUrl = Utf8ToWide(url);
    if (wideUrl.empty()) throw std::runtime_error("Gecersiz URL");
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts)) throw WinHttpError("URL ayrisimi basarisiz");
    if (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP) throw std::runtime_error("Yalniz HTTP/HTTPS desteklenir");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring target(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (target.empty()) target = L"/";

    if (session_ == nullptr) throw WinHttpError("HTTP oturumu acilamadi");
    InternetHandle connection(WinHttpConnect(session_, host.c_str(), parts.nPort, 0));
    if (!connection) throw WinHttpError("HTTP baglantisi acilamadi");
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    InternetHandle request(WinHttpOpenRequest(connection.get(), method.c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) throw WinHttpError("HTTP istegi olusturulamadi");
    for (const auto& [name, value] : headers) {
        const std::wstring line = name + L": " + value;
        if (!WinHttpAddRequestHeaders(request.get(), line.c_str(), static_cast<DWORD>(line.size()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) throw WinHttpError("HTTP basligi eklenemedi");
    }
    if (!body.empty() && !headers.contains(L"Content-Type")) {
        constexpr wchar_t contentType[] = L"Content-Type: application/json";
        WinHttpAddRequestHeaders(request.get(), contentType, static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
    }
    void* upload = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    const DWORD uploadBytes = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, upload, uploadBytes, uploadBytes, 0)
        || !WinHttpReceiveResponse(request.get(), nullptr)) throw WinHttpError("HTTP istegi basarisiz");
    DWORD status = 0; DWORD statusBytes = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes, WINHTTP_NO_HEADER_INDEX)) throw WinHttpError("HTTP durum kodu okunamadi");
    std::string response;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) throw WinHttpError("HTTP govdesi okunamadi");
        if (available == 0) break;
        if (response.size() + available > maxResponseBytes) throw std::runtime_error("HTTP yaniti cok buyuk");
        const std::size_t offset = response.size(); response.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), response.data() + offset, available, &read)) throw WinHttpError("HTTP govdesi okunamadi");
        response.resize(offset + read);
    }
    return {status, std::move(response)};
}

HttpResponse HttpClient::RequestPinnedVoiceNode(const std::wstring& method,
                                                const std::string& url,
                                                const std::string& expectedLeafSha256,
                                                const std::string& body,
                                                const std::size_t maxResponseBytes) const {
    const auto expectedFingerprint = ParseSha256Fingerprint(expectedLeafSha256);
    const std::wstring wideUrl = Utf8ToWide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (wideUrl.empty() || !WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts)
        || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        throw std::runtime_error("Ses dugumu yalniz HTTPS kullanabilir");
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring target(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (target.empty()) target = L"/";

    const std::wstring userAgent = Utf8ToWide(std::string("Sonalis-Voice/") + SONALIS_VERSION);
    InternetHandle session(WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) throw WinHttpError("Ses HTTP oturumu acilamadi");
    DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    secureProtocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS,
                          &secureProtocols, sizeof(secureProtocols))) {
        throw WinHttpError("Ses TLS protokolleri ayarlanamadi");
    }
    const int timeout = 15'000;
    WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout);
    InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection) throw WinHttpError("Ses HTTP baglantisi acilamadi");
    InternetHandle request(WinHttpOpenRequest(connection.get(), method.c_str(), target.c_str(), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE));
    if (!request) throw WinHttpError("Ses HTTP istegi olusturulamadi");
    DWORD securityFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA
        | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
        | SECURITY_FLAG_IGNORE_CERT_CN_INVALID
        | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_SECURITY_FLAGS,
                          &securityFlags, sizeof(securityFlags))) {
        throw WinHttpError("Ses TLS pinleme modu ayarlanamadi");
    }
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY,
                          &redirectPolicy, sizeof(redirectPolicy))) {
        throw WinHttpError("Ses HTTP yonlendirme ilkesi ayarlanamadi");
    }
    constexpr wchar_t contentType[] = L"Content-Type: application/json";
    if (!WinHttpAddRequestHeaders(request.get(), contentType, static_cast<DWORD>(-1),
                                  WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        throw WinHttpError("Ses HTTP basligi eklenemedi");
    }
    const DWORD uploadBytes = static_cast<DWORD>(body.size());
    // Complete the TLS handshake with headers only. The one-time join grant is
    // not written to the wire until the peer leaf certificate is pinned.
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, uploadBytes, 0)) {
        throw WinHttpError("Ses HTTP istegi basarisiz");
    }

    PCCERT_CONTEXT certificate = nullptr;
    DWORD certificateBytes = sizeof(certificate);
    if (!WinHttpQueryOption(request.get(), WINHTTP_OPTION_SERVER_CERT_CONTEXT,
                            &certificate, &certificateBytes) || certificate == nullptr) {
        throw WinHttpError("Ses dugumu sertifikasi okunamadi");
    }
    const auto certificateGuard = std::unique_ptr<const CERT_CONTEXT, decltype(&CertFreeCertificateContext)>(
        certificate, &CertFreeCertificateContext);
    std::array<std::uint8_t, 32> actualFingerprint{};
    DWORD fingerprintBytes = static_cast<DWORD>(actualFingerprint.size());
    if (!CryptHashCertificate2(L"SHA256", 0, nullptr, certificateGuard->pbCertEncoded,
                               certificateGuard->cbCertEncoded, actualFingerprint.data(), &fingerprintBytes)
        || fingerprintBytes != static_cast<DWORD>(actualFingerprint.size())
        || !std::equal(actualFingerprint.begin(), actualFingerprint.end(), expectedFingerprint.begin())) {
        throw std::runtime_error("Ses dugumu TLS sertifika parmak izi eslesmedi");
    }
    if (uploadBytes > 0) {
        DWORD written = 0;
        if (!WinHttpWriteData(request.get(), body.data(), uploadBytes, &written)
            || written != uploadBytes) {
            throw WinHttpError("Ses HTTP govdesi gonderilemedi");
        }
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        throw WinHttpError("Ses HTTP yaniti alinamadi");
    }

    DWORD status = 0;
    DWORD statusBytes = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes,
                             WINHTTP_NO_HEADER_INDEX)) {
        throw WinHttpError("Ses HTTP durum kodu okunamadi");
    }
    std::string response;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) throw WinHttpError("Ses HTTP govdesi okunamadi");
        if (available == 0) break;
        if (response.size() + available > maxResponseBytes) throw std::runtime_error("Ses HTTP yaniti cok buyuk");
        const std::size_t offset = response.size();
        response.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), response.data() + offset, available, &read)) {
            throw WinHttpError("Ses HTTP govdesi okunamadi");
        }
        response.resize(offset + read);
    }
    return {status, std::move(response)};
}

unsigned long HttpClient::DownloadToFile(const std::string& url,
                                         const std::wstring& targetPath,
                                         const std::size_t maxResponseBytes) const {
    const std::wstring wideUrl = Utf8ToWide(url);
    if (wideUrl.empty()) throw std::runtime_error("Gecersiz URL");
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts)) {
        throw WinHttpError("URL ayrisimi basarisiz");
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP) {
        throw std::runtime_error("Yalniz HTTP/HTTPS desteklenir");
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring target(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (target.empty()) target = L"/";

    const std::wstring userAgent = Utf8ToWide(std::string("Sonalis/") + SONALIS_VERSION);
    InternetHandle session(WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) throw WinHttpError("HTTP oturumu acilamadi");
    DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    secureProtocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS,
                          &secureProtocols, sizeof(secureProtocols))) {
        throw WinHttpError("TLS protokolleri ayarlanamadi");
    }
    const int timeout = 30'000;
    WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout);
    InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection) throw WinHttpError("HTTP baglantisi acilamadi");
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET", target.c_str(), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) throw WinHttpError("HTTP istegi olusturulamadi");
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                            0, 0, 0) || !WinHttpReceiveResponse(request.get(), nullptr)) {
        throw WinHttpError("HTTP istegi basarisiz");
    }
    DWORD status = 0;
    DWORD statusBytes = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes,
                             WINHTTP_NO_HEADER_INDEX)) {
        throw WinHttpError("HTTP durum kodu okunamadi");
    }
    if (status != 200) return status;

    const std::filesystem::path path(targetPath);
    std::filesystem::create_directories(path.parent_path());
    try {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) throw std::runtime_error("Indirme dosyasi acilamadi");
        std::array<char, 64U * 1024U> buffer{};
        std::size_t total = 0;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.get(), &available)) throw WinHttpError("HTTP govdesi okunamadi");
            if (available == 0) break;
            while (available > 0) {
                const DWORD requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
                DWORD read = 0;
                if (!WinHttpReadData(request.get(), buffer.data(), requested, &read)) {
                    throw WinHttpError("HTTP govdesi okunamadi");
                }
                if (read == 0) break;
                total += read;
                if (total > maxResponseBytes) throw std::runtime_error("Indirme paketi cok buyuk");
                stream.write(buffer.data(), static_cast<std::streamsize>(read));
                if (!stream) throw std::runtime_error("Indirme diske yazilamadi");
                available -= read;
            }
        }
        stream.flush();
        if (!stream) throw std::runtime_error("Indirme tamamlanamadi");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
    return status;
}

unsigned long HttpClient::UploadFile(const std::string& url,
                                     const std::wstring& sourcePath,
                                     const std::map<std::wstring, std::wstring>& headers,
                                     const std::size_t maxRequestBytes) const {
    const std::wstring wideUrl = Utf8ToWide(url);
    if (wideUrl.empty()) throw std::runtime_error("Gecersiz upload URL");
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts)) {
        throw WinHttpError("Upload URL ayrisimi basarisiz");
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTPS) throw std::runtime_error("Medya upload yalniz HTTPS kullanir");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring target(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (target.empty()) target = L"/";

    const std::filesystem::path path(sourcePath);
    const std::uintmax_t fileSize = std::filesystem::file_size(path);
    if (fileSize == 0 || fileSize > maxRequestBytes || fileSize > MAXDWORD) {
        throw std::runtime_error("Upload dosya boyutu gecersiz");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Upload dosyasi acilamadi");

    const std::wstring userAgent = Utf8ToWide(std::string("Sonalis/") + SONALIS_VERSION);
    InternetHandle session(WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) throw WinHttpError("Upload HTTP oturumu acilamadi");
    ConfigureSession(session.get(), 60'000);
    InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection) throw WinHttpError("Upload HTTP baglantisi acilamadi");
    InternetHandle request(WinHttpOpenRequest(connection.get(), L"PUT", target.c_str(), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE));
    if (!request) throw WinHttpError("Upload HTTP istegi olusturulamadi");
    for (const auto& [name, value] : headers) {
        const std::wstring line = name + L": " + value;
        if (!WinHttpAddRequestHeaders(request.get(), line.c_str(), static_cast<DWORD>(-1),
                                      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
            throw WinHttpError("Upload HTTP basligi eklenemedi");
        }
    }
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, static_cast<DWORD>(fileSize), 0)) {
        throw WinHttpError("Upload HTTP istegi baslatilamadi");
    }
    std::array<char, 64U * 1024U> buffer{};
    std::uintmax_t writtenTotal = 0;
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = stream.gcount();
        if (read <= 0) break;
        DWORD written = 0;
        if (!WinHttpWriteData(request.get(), buffer.data(), static_cast<DWORD>(read), &written)
            || written != static_cast<DWORD>(read)) {
            throw WinHttpError("Upload govdesi gonderilemedi");
        }
        writtenTotal += written;
    }
    if (writtenTotal != fileSize || !WinHttpReceiveResponse(request.get(), nullptr)) {
        throw WinHttpError("Upload tamamlanamadi");
    }
    DWORD status = 0;
    DWORD statusBytes = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes,
                             WINHTTP_NO_HEADER_INDEX)) {
        throw WinHttpError("Upload durum kodu okunamadi");
    }
    return status;
}

}  // namespace ss
