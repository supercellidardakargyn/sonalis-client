#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ss {

struct DiagnosticErrorEvent {
    std::uint64_t timestampMs{};
    std::string component;
    std::string errorCode;
    std::string severity;
    std::string context;
    std::uint16_t occurrences{1};
};

void DiagnosticLog(std::string_view area, std::string_view message) noexcept;
[[nodiscard]] std::vector<DiagnosticErrorEvent> TakePendingDiagnosticErrors(
    std::size_t maximum = 16) noexcept;
void RequeueDiagnosticErrors(std::span<const DiagnosticErrorEvent> events) noexcept;
[[nodiscard]] std::size_t PendingDiagnosticErrorCount() noexcept;
bool ExportDiagnostics(std::wstring_view path) noexcept;
[[nodiscard]] std::wstring DefaultDiagnosticsPath() noexcept;

}  // namespace ss
