#include "diagnostics.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <mutex>

#include <windows.h>
#include "performance.h"
#include "win_helpers.h"

namespace ss {
namespace {

struct DiagnosticEntry {
    std::uint64_t timestampMs{};
    std::array<char, 32> area{};
    std::array<char, 224> message{};
};

std::array<DiagnosticEntry, 256> entries;
std::size_t nextEntry{};
std::size_t entryCount{};
std::mutex entriesMutex;
constexpr std::uintmax_t kPersistentLogLimit = 512U * 1024U;
std::array<DiagnosticErrorEvent, 64> pendingErrors;
std::size_t pendingErrorCount{};
std::mutex pendingErrorsMutex;

template <std::size_t Size>
void CopyTruncated(const std::string_view source, std::array<char, Size>& target) noexcept {
    target.fill('\0');
    const std::size_t count = std::min(source.size(), Size - 1);
    if (count != 0) std::memcpy(target.data(), source.data(), count);
}

bool PersistArea(const std::string_view area) noexcept {
    return area == "startup" || area == "startup-watchdog"
        || area == "renderer" || area == "renderer-state"
        || area == "audio" || area == "audio.counters"
        || area == "network.counters" || area == "voice-node"
        || area == "voice-p2p" || area == "connect" || area == "realtime";
}

std::string RedactSensitive(const std::string_view source) {
    std::string result;
    result.reserve(std::min<std::size_t>(source.size(), 223));
    for (std::size_t index = 0; index < source.size() && result.size() < 223;) {
        const std::size_t begin = index;
        unsigned dotCount = 0;
        while (index < source.size()
            && ((source[index] >= '0' && source[index] <= '9') || source[index] == '.')) {
            if (source[index] == '.') ++dotCount;
            ++index;
        }
        if (index - begin >= 7 && dotCount == 3) {
            result += "[ip-redacted]";
            continue;
        }
        index = begin;
        unsigned colonCount = 0;
        const auto isHex = [](const char value) noexcept {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')
                || (value >= 'A' && value <= 'F');
        };
        while (index < source.size() && (isHex(source[index]) || source[index] == ':')) {
            if (source[index] == ':') ++colonCount;
            ++index;
        }
        if (index - begin >= 5 && colonCount >= 2) {
            result += "[ip-redacted]";
            continue;
        }
        index = begin;
        result.push_back(source[index++]);
    }
    return result;
}

std::string LowerAscii(std::string_view source) {
    std::string result(source);
    std::ranges::transform(result, result.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return result;
}

bool IsErrorLike(const std::string_view area, const std::string_view message) {
    if (area == "telemetry" || area.ends_with(".counters")) return false;
    const std::string lower = LowerAscii(message);
    if (lower.find("status=401") != std::string::npos
        || lower.find("status=404") != std::string::npos
        || lower.find("errors=0") != std::string::npos
        || lower.find("error=0") != std::string::npos) return false;
    constexpr std::array<std::string_view, 12> markers{
        "failed", "failure", "error", "exception", "timeout", "timed_out",
        "device_lost", "device-lost", "rejected", "invalid", "unavailable", "fatal",
    };
    return std::ranges::any_of(markers, [&lower](const std::string_view marker) {
        return lower.find(marker) != std::string::npos;
    });
}

std::string SafeComponent(const std::string_view area) {
    std::string result;
    result.reserve(std::min<std::size_t>(area.size(), 64));
    for (const unsigned char value : area) {
        const char lower = static_cast<char>(std::tolower(value));
        if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9')
            || lower == '.' || lower == '_' || lower == '-') result.push_back(lower);
        else result.push_back('-');
        if (result.size() == 64) break;
    }
    if (result.empty() || !std::isalnum(static_cast<unsigned char>(result.front()))) {
        result.insert(result.begin(), 'x');
    }
    return result;
}

std::string ExtractErrorCode(const std::string_view message) {
    constexpr std::array<std::string_view, 5> keys{"code=", "error=", "reason=", "exception=", "status="};
    const std::string lower = LowerAscii(message);
    for (const std::string_view key : keys) {
        const std::size_t keyAt = lower.find(key);
        if (keyAt == std::string::npos) continue;
        std::string result;
        const std::size_t valueAt = keyAt + key.size();
        for (std::size_t index = valueAt; index < message.size() && result.size() < 120; ++index) {
            const unsigned char value = static_cast<unsigned char>(message[index]);
            if (std::isalnum(value) || value == '.' || value == '_' || value == ':' || value == '-') {
                result.push_back(static_cast<char>(value));
            } else {
                break;
            }
        }
        if (!result.empty() && std::isalnum(static_cast<unsigned char>(result.front()))) return result;
    }
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char value : message) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    char fallback[32]{};
    std::snprintf(fallback, sizeof(fallback), "diagnostic_%016llx",
                  static_cast<unsigned long long>(hash));
    return fallback;
}

std::string RedactTelemetryContext(const std::string_view message) {
    std::string result = RedactSensitive(message);
    constexpr std::array<std::string_view, 6> secretKeys{
        "password=", "secret=", "token=", "authorization=", "api_key=", "apikey=",
    };
    for (const std::string_view key : secretKeys) {
        std::size_t searchAt = 0;
        while (true) {
            const std::string lower = LowerAscii(result);
            searchAt = lower.find(key, searchAt);
            if (searchAt == std::string::npos) break;
            const std::size_t valueAt = searchAt + key.size();
            std::size_t valueEnd = result.find_first_of(" \t\r\n,;", valueAt);
            if (valueEnd == std::string::npos) valueEnd = result.size();
            result.replace(valueAt, valueEnd - valueAt, "[redacted]");
            searchAt = valueAt + 10;
        }
    }
    for (std::size_t at = 0; at < result.size();) {
        const std::size_t space = result.find_first_of(" \t\r\n,;", at);
        const std::size_t end = space == std::string::npos ? result.size() : space;
        const std::string_view token(result.data() + at, end - at);
        if (token.find('@') != std::string_view::npos
            || (token.size() >= 64 && std::ranges::all_of(token, [](const unsigned char value) {
                return std::isalnum(value) || value == '+' || value == '/' || value == '='
                    || value == '_' || value == '-';
            }))) {
            result.replace(at, end - at, "[opaque-redacted]");
            at += 17;
        } else {
            at = end + (space == std::string::npos ? 0 : 1);
        }
        if (space == std::string::npos) break;
    }
    return result.substr(0, 1000);
}

void QueueError(DiagnosticErrorEvent event) noexcept {
    try {
        std::scoped_lock lock(pendingErrorsMutex);
        for (std::size_t index = 0; index < pendingErrorCount; ++index) {
            DiagnosticErrorEvent& existing = pendingErrors[index];
            if (existing.component == event.component && existing.errorCode == event.errorCode) {
                existing.timestampMs = std::max(existing.timestampMs, event.timestampMs);
                existing.context = std::move(event.context);
                existing.severity = std::move(event.severity);
                existing.occurrences = static_cast<std::uint16_t>(
                    std::min<unsigned>(100U, existing.occurrences + event.occurrences));
                return;
            }
        }
        if (pendingErrorCount == pendingErrors.size()) {
            std::move(pendingErrors.begin() + 1, pendingErrors.end(), pendingErrors.begin());
            --pendingErrorCount;
        }
        pendingErrors[pendingErrorCount++] = std::move(event);
    } catch (...) {
        // Telemetry must never affect the application.
    }
}

void PersistEntry(const DiagnosticEntry& entry) noexcept {
    if (!PersistArea(entry.area.data())) return;
    try {
        const std::filesystem::path local = LocalAppDataPath();
        if (local.empty()) return;
        const std::filesystem::path directory = local / L"Sonalis" / L"logs";
        std::filesystem::create_directories(directory);
        const std::filesystem::path active = directory / L"client.log";
        const std::filesystem::path previous = directory / L"client.previous.log";
        std::error_code fileError;
        if (std::filesystem::exists(previous, fileError)) {
            const auto age = std::filesystem::file_time_type::clock::now()
                - std::filesystem::last_write_time(previous, fileError);
            if (!fileError && age > std::chrono::hours(48)) std::filesystem::remove(previous, fileError);
        }
        if (std::filesystem::exists(active, fileError)
            && (std::filesystem::file_size(active, fileError) >= kPersistentLogLimit
                || (!fileError && std::filesystem::file_time_type::clock::now()
                    - std::filesystem::last_write_time(active, fileError) > std::chrono::hours(24)))) {
            std::filesystem::remove(previous, fileError);
            std::filesystem::rename(active, previous, fileError);
        }
        std::ofstream stream(active, std::ios::binary | std::ios::app);
        if (stream) {
            stream << entry.timestampMs << ' ' << entry.area.data() << ": " << entry.message.data() << '\n';
        }
    } catch (...) {
        // Diagnostics must never affect the audio/UI path.
    }
}

}  // namespace

void DiagnosticLog(const std::string_view area, const std::string_view message) noexcept {
    try {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const std::string redacted = RedactSensitive(message);
        {
            std::scoped_lock lock(entriesMutex);
            DiagnosticEntry& entry = entries[nextEntry];
            entry.timestampMs = static_cast<std::uint64_t>(now);
            CopyTruncated(area, entry.area);
            CopyTruncated(redacted, entry.message);
            nextEntry = (nextEntry + 1) % entries.size();
            entryCount = std::min(entryCount + 1, entries.size());
            PersistEntry(entry);
        }
        if (IsErrorLike(area, message)) {
            const std::string lower = LowerAscii(message);
            QueueError(DiagnosticErrorEvent{
                .timestampMs = static_cast<std::uint64_t>(now),
                .component = SafeComponent(area),
                .errorCode = ExtractErrorCode(message),
                .severity = lower.find("fatal") != std::string::npos ? "fatal"
                    : (lower.find("timeout") != std::string::npos
                       || lower.find("unavailable") != std::string::npos ? "warning" : "error"),
                .context = RedactTelemetryContext(message),
                .occurrences = 1,
            });
        }
    } catch (...) {
        // Diagnostics must never fail a realtime path.
    }
}

std::vector<DiagnosticErrorEvent> TakePendingDiagnosticErrors(const std::size_t maximum) noexcept {
    try {
        std::scoped_lock lock(pendingErrorsMutex);
        const std::size_t count = std::min({maximum, pendingErrorCount, pendingErrors.size()});
        std::vector<DiagnosticErrorEvent> result;
        result.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            result.push_back(std::move(pendingErrors[index]));
        }
        std::move(pendingErrors.begin() + static_cast<std::ptrdiff_t>(count),
                  pendingErrors.begin() + static_cast<std::ptrdiff_t>(pendingErrorCount),
                  pendingErrors.begin());
        pendingErrorCount -= count;
        return result;
    } catch (...) {
        return {};
    }
}

void RequeueDiagnosticErrors(const std::span<const DiagnosticErrorEvent> events) noexcept {
    for (const DiagnosticErrorEvent& event : events) QueueError(event);
}

std::size_t PendingDiagnosticErrorCount() noexcept {
    try {
        std::scoped_lock lock(pendingErrorsMutex);
        return pendingErrorCount;
    } catch (...) {
        return 0;
    }
}

bool ExportDiagnostics(const std::wstring_view path) noexcept {
    try {
        std::array<DiagnosticEntry, 256> snapshot{};
        std::size_t count = 0;
        {
            std::scoped_lock lock(entriesMutex);
            count = entryCount;
            const std::size_t first = (nextEntry + entries.size() - entryCount) % entries.size();
            for (std::size_t index = 0; index < count; ++index) snapshot[index] = entries[(first + index) % entries.size()];
        }
        std::ofstream stream(std::wstring(path), std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        for (std::size_t index = 0; index < count; ++index) {
            const auto& entry = snapshot[index];
            stream << entry.timestampMs << ' ' << entry.area.data() << ": " << entry.message.data() << '\n';
        }
        const PerformanceSnapshot performance = ReadPerformanceSnapshot();
        constexpr std::array<const char*, 5> names{"ui_frame_us", "opus_encode_us", "rnnoise_us", "worker_queue", "websocket_reconnects"};
        stream << "performance latest/max\n";
        for (std::size_t index = 0; index < names.size(); ++index) {
            stream << names[index] << ' ' << performance.latest[index] << '/' << performance.maximum[index] << '\n';
        }
        return static_cast<bool>(stream);
    } catch (...) { return false; }
}

std::wstring DefaultDiagnosticsPath() noexcept {
    try {
        const std::filesystem::path local = LocalAppDataPath();
        if (local.empty()) return {};
        const std::filesystem::path directory = local / L"Sonalis";
        std::filesystem::create_directories(directory);
        return (directory / L"diagnostics.txt").wstring();
    } catch (...) { return {}; }
}

}  // namespace ss
