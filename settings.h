#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ss {

enum class UiTheme : std::uint8_t {
    Classic,
    AuroraDark,
    AuroraLight,
    Oled,
    Custom,
};

enum class ResourceProfile : std::uint8_t {
    Economy,
    Balanced,
    Visual,
};

enum class SensitiveMediaMode : std::uint8_t {
    Block,
    Blur,
    Show,
};

struct AppSettings {
    int settingsSchema{6};
    int experiencePolicyVersion{};
    std::string language{"tr"};
    std::string controlOrigin{"https://sonalis.tr"};
    std::string server{"127.0.0.1:25565"};
    std::string nickname{"Kullanici"};
    std::string room{"Lobby"};
    std::string inputDeviceId;
    std::string outputDeviceId;
    bool voiceActivation{true};
    bool pushToTalk{false};
    int pushToTalkVirtualKey{0x56};
    float vadSensitivity{0.62F};
    float outputVolume{1.0F};
    bool serverDenoise{false};
    bool localDenoise{true};
    bool p2pEnabled{true};
    int notificationPreview{0};
    UiTheme uiTheme{UiTheme::AuroraDark};
    ResourceProfile resourceProfile{ResourceProfile::Balanced};
    SensitiveMediaMode sensitiveMediaMode{SensitiveMediaMode::Block};
    float textScale{1.0F};
    int uiDensity{1};
    bool highContrast{false};
    int colorVisionMode{0};
    float customAccentR{0.12F};
    float customAccentG{0.56F};
    float customAccentB{0.95F};
    bool auroraPreviewShown{false};
    std::string lastRoomId;
    bool hasWindowPlacement{false};
    int windowX{100};
    int windowY{100};
    int windowWidth{1280};
    int windowHeight{800};
    bool windowMaximized{false};
};

[[nodiscard]] std::string_view UiThemeName(UiTheme value) noexcept;
[[nodiscard]] std::string_view ResourceProfileName(ResourceProfile value) noexcept;
[[nodiscard]] std::string_view SensitiveMediaModeName(SensitiveMediaMode value) noexcept;
[[nodiscard]] UiTheme ParseUiTheme(std::string_view value) noexcept;
[[nodiscard]] ResourceProfile ParseResourceProfile(std::string_view value) noexcept;
[[nodiscard]] SensitiveMediaMode ParseSensitiveMediaMode(std::string_view value) noexcept;

class SettingsStore final {
public:
    AppSettings Load() const;
    bool Save(const AppSettings& settings, std::string& error) const;
    bool ResetUi(std::string& error) const;
    [[nodiscard]] std::wstring SettingsPath() const;
};

std::wstring Utf8ToWide(const std::string& value);
std::string WideToUtf8(const std::wstring& value);

}  // namespace ss
