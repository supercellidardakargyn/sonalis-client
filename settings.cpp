#include "settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <array>
#include <cwctype>
#include <string_view>

#include <nlohmann/json.hpp>

#include "win_helpers.h"

namespace ss {

namespace {

std::string SystemLanguageTag() {
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) == 0) return "en";
    std::wstring value(locale);
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    if (value.starts_with(L"tr")) return "tr";
    if (value.starts_with(L"de")) return "de";
    if (value.starts_with(L"fr")) return "fr";
    if (value.starts_with(L"es")) return "es";
    if (value.starts_with(L"pt")) return "pt-BR";
    if (value.starts_with(L"it")) return "it";
    if (value.starts_with(L"ru")) return "ru";
    if (value.starts_with(L"ar")) return "ar";
    if (value.starts_with(L"ja")) return "ja";
    if (value.starts_with(L"ko")) return "ko";
    if (value.starts_with(L"zh")) return "zh-Hans";
    return "en";
}

bool IsSupportedLanguageTag(const std::string& value) {
    constexpr std::array<std::string_view, 12> tags{
        "tr", "en", "de", "fr", "es", "pt-BR", "it", "ru", "ar", "ja", "ko", "zh-Hans",
    };
    return std::ranges::find(tags, value) != tags.end();
}

bool IsAllowedControlOrigin(const std::string& value) {
    if (value.starts_with("https://")) return true;
#if defined(_DEBUG)
    return value.starts_with("http://localhost") || value.starts_with("http://127.0.0.1")
        || value.starts_with("http://[::1]");
#else
    return false;
#endif
}

}  // namespace

std::string_view UiThemeName(const UiTheme value) noexcept {
    switch (value) {
    case UiTheme::Classic: return "classic";
    case UiTheme::AuroraLight: return "aurora_light";
    case UiTheme::Oled: return "oled";
    case UiTheme::Custom: return "custom";
    case UiTheme::AuroraDark:
    default: return "aurora_dark";
    }
}

std::string_view ResourceProfileName(const ResourceProfile value) noexcept {
    switch (value) {
    case ResourceProfile::Economy: return "economy";
    case ResourceProfile::Visual: return "visual";
    case ResourceProfile::Balanced:
    default: return "balanced";
    }
}

std::string_view SensitiveMediaModeName(const SensitiveMediaMode value) noexcept {
    switch (value) {
    case SensitiveMediaMode::Blur: return "blur";
    case SensitiveMediaMode::Show: return "show";
    case SensitiveMediaMode::Block:
    default: return "block";
    }
}

UiTheme ParseUiTheme(const std::string_view value) noexcept {
    if (value == "classic") return UiTheme::Classic;
    if (value == "aurora_light") return UiTheme::AuroraLight;
    if (value == "oled") return UiTheme::Oled;
    if (value == "custom") return UiTheme::Custom;
    return UiTheme::AuroraDark;
}

ResourceProfile ParseResourceProfile(const std::string_view value) noexcept {
    if (value == "economy") return ResourceProfile::Economy;
    if (value == "visual") return ResourceProfile::Visual;
    return ResourceProfile::Balanced;
}

SensitiveMediaMode ParseSensitiveMediaMode(const std::string_view value) noexcept {
    if (value == "blur") return SensitiveMediaMode::Blur;
    if (value == "show") return SensitiveMediaMode::Show;
    return SensitiveMediaMode::Block;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring SettingsStore::SettingsPath() const {
    const std::filesystem::path local = LocalAppDataPath();
    return ((local.empty() ? std::filesystem::path(L".") : local) / L"Sonalis" / L"settings.json").wstring();
}

AppSettings SettingsStore::Load() const {
    AppSettings settings;
    settings.language = SystemLanguageTag();
    try {
        const std::filesystem::path path(SettingsPath());
        if (std::filesystem::exists(path) && std::filesystem::file_size(path) > 256U * 1024U) return settings;
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return settings;
        const nlohmann::json json = nlohmann::json::parse(stream, nullptr, true, true);
        const bool legacySettings = !json.contains("uiTheme") || !json.contains("resourceProfile");
        settings.settingsSchema = std::max(1, json.value("settingsSchema", 1));
        settings.experiencePolicyVersion = std::max(0, json.value("experiencePolicyVersion", 0));
        // Eski surumlerde tema alanlari yoktu. Bu kullanicilari eski gorunume
        // kilitlemek yerine guncel, hafif Aurora varsayilanina tasiyoruz.
        settings.uiTheme = legacySettings ? UiTheme::AuroraDark
                                          : ParseUiTheme(json.value("uiTheme", "aurora_dark"));
        settings.resourceProfile = legacySettings ? ResourceProfile::Balanced
            : ParseResourceProfile(json.value("resourceProfile", "balanced"));
        settings.auroraPreviewShown = json.value("auroraPreviewShown", false);
        settings.sensitiveMediaMode = ParseSensitiveMediaMode(json.value("sensitiveMediaMode", "block"));
        settings.textScale = std::clamp(json.value("textScale", 1.0F), 0.85F, 1.35F);
        settings.uiDensity = std::clamp(json.value("uiDensity", 1), 0, 2);
        settings.highContrast = json.value("highContrast", false);
        settings.colorVisionMode = std::clamp(json.value("colorVisionMode", 0), 0, 3);
        settings.customAccentR = std::clamp(json.value("customAccentR", 0.12F), 0.0F, 1.0F);
        settings.customAccentG = std::clamp(json.value("customAccentG", 0.56F), 0.0F, 1.0F);
        settings.customAccentB = std::clamp(json.value("customAccentB", 0.95F), 0.0F, 1.0F);
        settings.language = json.value("language", settings.language);
        if (settings.language == "pt") settings.language = "pt-BR";
        if (settings.language == "zh-CN") settings.language = "zh-Hans";
        if (!IsSupportedLanguageTag(settings.language)) settings.language = "en";
        settings.controlOrigin = json.value("controlOrigin", settings.controlOrigin);
        // Onceki gelistirme alan adini kaydetmis kurulumlari canli adrese tasir.
        if (settings.controlOrigin == "https://sonalis.example.com" || settings.controlOrigin == "https://solaris.tr") {
            settings.controlOrigin = "https://sonalis.tr";
        }
        if (!IsAllowedControlOrigin(settings.controlOrigin)) settings.controlOrigin = "https://sonalis.tr";
        settings.server = json.value("server", settings.server);
        settings.nickname = json.value("nickname", settings.nickname);
        settings.room = json.value("room", settings.room);
        settings.inputDeviceId = json.value("inputDeviceId", settings.inputDeviceId);
        settings.outputDeviceId = json.value("outputDeviceId", settings.outputDeviceId);
        settings.voiceActivation = json.value("voiceActivation", settings.voiceActivation);
        settings.pushToTalk = json.value("pushToTalk", settings.pushToTalk);
        settings.pushToTalkVirtualKey = std::clamp(json.value("pushToTalkVirtualKey", settings.pushToTalkVirtualKey), 1, 255);
        settings.vadSensitivity = std::clamp(json.value("vadSensitivity", settings.vadSensitivity), 0.0F, 1.0F);
        settings.outputVolume = std::clamp(json.value("outputVolume", settings.outputVolume), 0.0F, 2.0F);
        settings.serverDenoise = json.value("serverDenoise", settings.serverDenoise);
        settings.localDenoise = json.value("localDenoise", settings.localDenoise);
        settings.p2pEnabled = json.value("p2pEnabled", settings.p2pEnabled);
        settings.notificationPreview = std::clamp(json.value("notificationPreview", settings.notificationPreview), 0, 2);
        settings.lastRoomId = json.value("lastRoomId", settings.lastRoomId);
        settings.hasWindowPlacement = json.value("hasWindowPlacement", settings.hasWindowPlacement);
        settings.windowX = json.value("windowX", settings.windowX);
        settings.windowY = json.value("windowY", settings.windowY);
        settings.windowWidth = std::clamp(json.value("windowWidth", settings.windowWidth), 960, 15360);
        settings.windowHeight = std::clamp(json.value("windowHeight", settings.windowHeight), 640, 8640);
        settings.windowMaximized = json.value("windowMaximized", settings.windowMaximized);
    } catch (...) { return settings; }
    return settings;
}

bool SettingsStore::Save(const AppSettings& settings, std::string& error) const {
    try {
        const std::filesystem::path target(SettingsPath());
        std::filesystem::create_directories(target.parent_path());
        const std::filesystem::path temporary = target.wstring() + L".tmp";
        const nlohmann::json json{
            {"settingsSchema", 6},
            {"experiencePolicyVersion", settings.experiencePolicyVersion},
            {"language", settings.language},
            {"controlOrigin", settings.controlOrigin},
            {"server", settings.server},
            {"nickname", settings.nickname},
            {"room", settings.room},
            {"inputDeviceId", settings.inputDeviceId},
            {"outputDeviceId", settings.outputDeviceId},
            {"voiceActivation", settings.voiceActivation},
            {"pushToTalk", settings.pushToTalk},
            {"pushToTalkVirtualKey", settings.pushToTalkVirtualKey},
            {"vadSensitivity", settings.vadSensitivity},
            {"outputVolume", settings.outputVolume},
            {"serverDenoise", settings.serverDenoise},
            {"localDenoise", settings.localDenoise},
            {"p2pEnabled", settings.p2pEnabled},
            {"notificationPreview", settings.notificationPreview},
            {"uiTheme", UiThemeName(settings.uiTheme)},
            {"resourceProfile", ResourceProfileName(settings.resourceProfile)},
            {"sensitiveMediaMode", SensitiveMediaModeName(settings.sensitiveMediaMode)},
            {"textScale", settings.textScale},
            {"uiDensity", settings.uiDensity},
            {"highContrast", settings.highContrast},
            {"colorVisionMode", settings.colorVisionMode},
            {"customAccentR", settings.customAccentR},
            {"customAccentG", settings.customAccentG},
            {"customAccentB", settings.customAccentB},
            {"auroraPreviewShown", settings.auroraPreviewShown},
            {"lastRoomId", settings.lastRoomId},
            {"hasWindowPlacement", settings.hasWindowPlacement},
            {"windowX", settings.windowX},
            {"windowY", settings.windowY},
            {"windowWidth", std::clamp(settings.windowWidth, 960, 15360)},
            {"windowHeight", std::clamp(settings.windowHeight, 640, 8640)},
            {"windowMaximized", settings.windowMaximized},
        };
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) throw std::runtime_error("Ayar dosyasi acilamadi");
            stream << json.dump(2);
            stream.flush();
            if (!stream) throw std::runtime_error("Ayar dosyasi yazilamadi");
        }
        if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::filesystem::remove(temporary);
            throw std::runtime_error("Ayar dosyasi degistirilemedi");
        }
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool SettingsStore::ResetUi(std::string& error) const {
    AppSettings settings = Load();
    settings.hasWindowPlacement = false;
    settings.windowX = 100;
    settings.windowY = 100;
    settings.windowWidth = 1280;
    settings.windowHeight = 800;
    settings.windowMaximized = false;
    return Save(settings, error);
}

}  // namespace ss
