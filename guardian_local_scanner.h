#pragma once

#include <string>
#include <vector>

#include "platform_api.h"

namespace ss {

enum class GuardianLocalDecision {
    Safe,
    Review,
    Blocked,
    Critical,
};

struct GuardianLocalScanResult {
    GuardianLocalDecision decision{GuardianLocalDecision::Review};
    std::string modelId;
    std::string modelVersion;
    std::string digest;
    float adultScore{};
    float maximumScore{};
    std::uint64_t durationMs{};
    std::vector<float> scores;
};

class GuardianLocalScanner final {
public:
    bool Scan(PlatformApi& platform,
              const std::wstring& sourcePath,
              const std::string& attachmentId,
              bool preferGpu,
              GuardianLocalScanResult& result,
              std::string& error) const;
};

[[nodiscard]] const char* GuardianDecisionName(GuardianLocalDecision decision) noexcept;

}  // namespace ss
