#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace ss {

inline constexpr wchar_t kAutomaticUpdateInstallerArguments[] =
    // /NORESTART prevents an operating-system reboot. /RESTARTSONALIS is a
    // private installer switch that relaunches the updated per-user client.
    L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /CLOSEAPPLICATIONS /RESTARTSONALIS";

inline std::optional<std::array<std::uint32_t, 3>> ParseReleaseVersion(
    const std::string_view value) noexcept {
    std::array<std::uint32_t, 3> result{};
    std::size_t cursor = 0;
    for (std::size_t component = 0; component < result.size(); ++component) {
        if (cursor >= value.size() || value[cursor] < '0' || value[cursor] > '9') return std::nullopt;
        std::uint64_t parsed = 0;
        while (cursor < value.size() && value[cursor] >= '0' && value[cursor] <= '9') {
            parsed = parsed * 10U + static_cast<unsigned>(value[cursor] - '0');
            if (parsed > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
            ++cursor;
        }
        result[component] = static_cast<std::uint32_t>(parsed);
        if (component + 1U < result.size()) {
            if (cursor >= value.size() || value[cursor] != '.') return std::nullopt;
            ++cursor;
        }
    }
    return cursor == value.size() ? std::optional{result} : std::nullopt;
}

inline bool IsNewerReleaseVersion(const std::string_view candidate,
                                  const std::string_view current) noexcept {
    const auto parsedCandidate = ParseReleaseVersion(candidate);
    const auto parsedCurrent = ParseReleaseVersion(current);
    return parsedCandidate && parsedCurrent && *parsedCandidate > *parsedCurrent;
}

struct UpdateInfo {
    std::string product;
    std::string channel;
    std::string version;
    std::string minimumVersion;
    std::string artifactUrl;
    std::uint64_t artifactSize{};
    std::string sha256;
    std::string signature;
    std::string authenticodeThumbprint;
    std::string publishedAt;
    int rolloutPercent{};
    bool required{};
    int manifestVersion{};
};
enum class UpdateState { Idle, Checking, Current, Available, Downloading, Ready, Error };

class UpdateService final {
public:
    UpdateService() = default;
    ~UpdateService();
    bool CheckAsync(std::string controlOrigin, bool force = false);
    bool DownloadAsync();
    bool Download(std::string& error);
    bool LaunchInstaller(std::string& error) const;
    void SetStateCallback(std::function<void()> callback);
    [[nodiscard]] UpdateState State() const noexcept;
    [[nodiscard]] std::string Status() const;
    [[nodiscard]] std::optional<UpdateInfo> Available() const;
    [[nodiscard]] bool CheckDue() const noexcept;
    [[nodiscard]] std::uint32_t MillisecondsUntilNextCheck() const noexcept;

private:
    void Check(std::string origin);
    void Set(UpdateState state, std::string status);
    mutable std::mutex mutex_;
    std::jthread thread_;
    UpdateState state_{UpdateState::Idle};
    std::string status_;
    std::optional<UpdateInfo> available_;
    std::wstring downloadedPath_;
    std::function<void()> stateCallback_;
    std::uint64_t nextCheckAtMs_{};
};

}  // namespace ss
