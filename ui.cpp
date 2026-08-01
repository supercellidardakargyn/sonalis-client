#include "ui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <filesystem>
#include <fstream>

#include <commdlg.h>

#include <imgui.h>
#include <monocypher.h>
#include <nlohmann/json.hpp>

#include "protocol.h"
#include "application_messages.h"
#include "diagnostics.h"
#include "device_identity.h"
#include "resources.h"
#include "theme_engine.h"
#include "horizon_icons.h"
#include "performance.h"
#include "win_helpers.h"

#ifndef SONALIS_VERSION
#define SONALIS_VERSION "0.0.0"
#endif

namespace ss {
namespace {

float gRenderDpiScale = 1.0F;

float ResolveDpiScale(const HWND window) noexcept {
    const UINT dpi = window != nullptr ? GetDpiForWindow(window) : GetDpiForSystem();
    return std::clamp(static_cast<float>(dpi == 0U ? 96U : dpi) / 96.0F, 1.0F, 4.0F);
}

template <std::size_t Size>
void CopyToBuffer(const std::string& source, std::array<char, Size>& target) {
    target.fill('\0');
    const std::size_t count = std::min(source.size(), Size - 1);
    std::memcpy(target.data(), source.data(), count);
}

bool ContainsMention(const std::string_view text, const std::string_view username) {
    if (username.empty()) return false;
    const std::string marker = "@" + std::string(username);
    std::size_t position = text.find(marker);
    while (position != std::string_view::npos) {
        const std::size_t end = position + marker.size();
        if (end == text.size() || !((text[end] >= 'a' && text[end] <= 'z')
            || (text[end] >= 'A' && text[end] <= 'Z')
            || (text[end] >= '0' && text[end] <= '9') || text[end] == '_')) return true;
        position = text.find(marker, end);
    }
    return false;
}

std::string FitTextToWidth(const std::string_view text, const float maximumWidth) {
    if (text.empty() || maximumWidth <= 0.0F) return {};
    std::string fitted(text);
    if (ImGui::CalcTextSize(fitted.c_str()).x <= maximumWidth) return fitted;

    constexpr const char* ellipsis = "\xE2\x80\xA6";
    if (ImGui::CalcTextSize(ellipsis).x > maximumWidth) return {};
    std::size_t cut = fitted.size();
    while (cut > 0U) {
        do {
            --cut;
        } while (cut > 0U && (static_cast<unsigned char>(fitted[cut]) & 0xC0U) == 0x80U);
        std::string candidate = fitted.substr(0U, cut);
        candidate += ellipsis;
        if (ImGui::CalcTextSize(candidate.c_str()).x <= maximumWidth) return candidate;
    }
    return ellipsis;
}

std::string UserFacingError(const std::string& error) {
    if (error.empty()) return "İşlem tamamlanamadı. Lütfen yeniden deneyin.";
    if (error.find("json_contract:") != std::string::npos
        || error.find("json.exception") != std::string::npos) {
        return "Sunucudan gelen veri okunamadı. Sonalis'i güncelleyip yeniden deneyin.";
    }
    if (error.find("timeout") != std::string::npos || error.find("timed_out") != std::string::npos) {
        return "Sunucu yanıt vermedi. İnternet bağlantınızı kontrol edip yeniden deneyin.";
    }
    if (error == "invalid_credentials") return "Kullanıcı adı/e-posta veya parola hatalı.";
    if (error == "room_quota_exceeded") return "Oda oluşturma kotanıza ulaştınız.";
    if (error == "membership_quota_exceeded") return "Oda üyeliği kotanıza ulaştınız.";
    if (error == "invite_invalid" || error == "invite_expired") {
        return "Davet kodu geçersiz veya süresi dolmuş.";
    }
    if (error == "forbidden") return "Bu işlem için yetkiniz yok.";
    if (error == "unauthorized" || error == "session_expired"
        || error == "access_token_expired" || error == "access_token_invalid") {
        return "Oturumunuz sona erdi. Lütfen yeniden giriş yapın.";
    }
    if (error == "session_refresh_failed") {
        return "Oturum şu anda yenilenemedi. İnternet bağlantınızı kontrol edip biraz sonra yeniden deneyin.";
    }
    if (error == "internal_error") {
        return "Sunucuda geçici bir hata oluştu. Biraz sonra yeniden deneyin.";
    }
    if (error == "conversation_epoch_changed") {
        return "Mesaj güvenlik anahtarı yenilendi. Mesajınız korunuyor; yeniden gönderebilirsiniz.";
    }
    if (error == "legal_escrow_not_configured" || error == "key_epoch_legal_escrow_required") {
        return "Mesaj güvenliği sunucuda henüz yapılandırılmamış. Yönetici ayarlarını kontrol edin.";
    }
    if (error == "message_signature_invalid" || error == "device_not_registered") {
        return "Bu cihazın mesaj kimliği yenilenemedi. Oturumu yenileyip tekrar deneyin.";
    }
    if (error == "message_key_envelope_not_found" || error == "key_epoch_missing_device_envelopes") {
        return "Şifreli konuşma anahtarı bu cihaz için hazırlanıyor. Birkaç saniye sonra tekrar deneyin.";
    }
    if (error.size() > 180) return "Beklenmeyen bir hata oluştu. Tanılama kaydını destek ekibiyle paylaşın.";
    return error;
}

bool HasNonWhitespace(const std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](const unsigned char character) {
        return std::isspace(character) == 0;
    });
}

bool IsTransientMessageSendError(const std::string_view error) noexcept {
    return error == "internal_error" || error == "session_refresh_failed"
        || error.find("WinHTTP") != std::string_view::npos
        || error.find("timed out") != std::string_view::npos
        || error.find("timeout") != std::string_view::npos;
}

std::uint64_t IsoUtcFileTimeTicks(const std::string_view value) noexcept {
    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    if (sscanf_s(std::string(value).c_str(), "%4u-%2u-%2uT%2u:%2u:%2u",
                 &year, &month, &day, &hour, &minute, &second) != 6) return 0;
    SYSTEMTIME utc{};
    utc.wYear = static_cast<WORD>(year);
    utc.wMonth = static_cast<WORD>(month);
    utc.wDay = static_cast<WORD>(day);
    utc.wHour = static_cast<WORD>(hour);
    utc.wMinute = static_cast<WORD>(minute);
    utc.wSecond = static_cast<WORD>(second);
    FILETIME fileTime{};
    if (SystemTimeToFileTime(&utc, &fileTime) == FALSE) return 0;
    ULARGE_INTEGER ticks{};
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    return ticks.QuadPart;
}

std::string LocalMessageTime(const std::string_view value) {
    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    if (sscanf_s(std::string(value).c_str(), "%4u-%2u-%2uT%2u:%2u:%2u",
                 &year, &month, &day, &hour, &minute, &second) != 6) return "--:--";
    SYSTEMTIME utc{};
    utc.wYear = static_cast<WORD>(year);
    utc.wMonth = static_cast<WORD>(month);
    utc.wDay = static_cast<WORD>(day);
    utc.wHour = static_cast<WORD>(hour);
    utc.wMinute = static_cast<WORD>(minute);
    utc.wSecond = static_cast<WORD>(second);
    SYSTEMTIME local = utc;
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    char formatted[8]{};
    std::snprintf(formatted, sizeof(formatted), "%02u:%02u",
                  static_cast<unsigned>(local.wHour),
                  static_cast<unsigned>(local.wMinute));
    return formatted;
}

std::string CurrentUtcIsoTimestamp() {
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    char formatted[32]{};
    std::snprintf(formatted, sizeof(formatted),
                  "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                  static_cast<unsigned>(utc.wYear), static_cast<unsigned>(utc.wMonth),
                  static_cast<unsigned>(utc.wDay), static_cast<unsigned>(utc.wHour),
                  static_cast<unsigned>(utc.wMinute), static_cast<unsigned>(utc.wSecond),
                  static_cast<unsigned>(utc.wMilliseconds));
    return formatted;
}

bool CanGroupMessages(const std::string_view previousSender,
                      const std::string_view previousCreatedAt,
                      const std::string_view sender,
                      const std::string_view createdAt,
                      const std::string_view replyTo) noexcept {
    if (previousSender.empty() || previousSender != sender || !replyTo.empty()) return false;
    const std::uint64_t previous = IsoUtcFileTimeTicks(previousCreatedAt);
    const std::uint64_t current = IsoUtcFileTimeTicks(createdAt);
    constexpr std::uint64_t fiveMinutes = 5ULL * 60ULL * 10'000'000ULL;
    return previous != 0U && current >= previous && current - previous <= fiveMinutes;
}

ImFont* RegularUiFont() noexcept {
    ImGuiIO& io = ImGui::GetIO();
    return io.FontDefault != nullptr ? io.FontDefault : ImGui::GetFont();
}

ImFont* SemiboldUiFont() noexcept {
    ImGuiIO& io = ImGui::GetIO();
    return io.Fonts != nullptr && io.Fonts->Fonts.Size > 1
        ? io.Fonts->Fonts[1] : RegularUiFont();
}

void EyebrowLabel(const char* label) {
    ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    color.x = std::min(1.0F, color.x * 1.10F);
    color.y = std::min(1.0F, color.y * 1.10F);
    color.z = std::min(1.0F, color.z * 1.14F);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushFont(SemiboldUiFont(), ImGui::GetStyle().FontSizeBase * 0.82F);
    ImGui::TextUnformatted(label);
    ImGui::PopFont();
    ImGui::PopStyleColor();
}

void SectionTitle(const char* title, const char* subtitle) {
    ImGui::PushFont(SemiboldUiFont(), ImGui::GetStyle().FontSizeBase * 1.12F);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (subtitle != nullptr) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 2.0F * gRenderDpiScale));
        ImGui::PushFont(RegularUiFont(), ImGui::GetStyle().FontSizeBase * 0.88F);
        ImGui::TextDisabled("%s", subtitle);
        ImGui::PopFont();
        ImGui::PopStyleVar();
    }
    ImGui::Dummy(ImVec2(0.0F, 3.0F * gRenderDpiScale));
}

void FieldLabel(const char* label) {
    ImGui::PushFont(SemiboldUiFont(), ImGui::GetStyle().FontSizeBase * 0.82F);
    ImGui::TextDisabled("%s", label);
    ImGui::PopFont();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

void Meter(const float value, const float threshold = -1.0F) {
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::ProgressBar(std::clamp(value, 0.0F, 1.0F), ImVec2(-FLT_MIN, 12.0F), "");
    if (threshold >= 0.0F) {
        const float width = ImGui::GetItemRectSize().x;
        const float x = start.x + std::clamp(threshold, 0.0F, 1.0F) * width;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(x, start.y - 1.0F), ImVec2(x, start.y + 13.0F),
                                             IM_COL32(255, 255, 255, 180), 1.0F);
    }
}

void DrawNavigationIcon(ImDrawList* drawList, const ImVec2 origin, const ClientPage page,
                        const ImU32 color) {
    const float scale = gRenderDpiScale;
    const ImVec2 center(origin.x + 12.0F * scale, origin.y + 12.0F * scale);
    const horizon::Icon icon = page == ClientPage::Home ? horizon::Icon::Home
        : page == ClientPage::Voice ? horizon::Icon::VoiceChannel
        : page == ClientPage::Rooms ? horizon::Icon::Community
        : page == ClientPage::Messages ? horizon::Icon::Messages
        : horizon::Icon::Settings;
    horizon::DrawIcon(drawList, icon, center, 22.0F * scale, color);
}

bool NavigationButton(const char* id, const char* label, const ClientPage page,
                      const ClientPage activePage, const unsigned badge = 0) {
    ImGui::PushID(id);
    const float scale = gRenderDpiScale;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const ImVec2 size(availableWidth, 52.0F * scale);
    const bool pressed = ImGui::InvisibleButton("nav", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = page == activePage;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float tileSize = 44.0F * scale;
    const ImVec2 tileMin(start.x + (size.x - tileSize) * 0.5F,
                         start.y + (size.y - tileSize) * 0.5F);
    const ImVec2 tileMax(tileMin.x + tileSize, tileMin.y + tileSize);
    if (active || hovered) {
        ImVec4 background = ImGui::GetStyleColorVec4(active ? ImGuiCol_Header : ImGuiCol_ButtonHovered);
        background.w = active ? 0.88F : 0.62F;
        drawList->AddRectFilled(tileMin, tileMax, ImGui::GetColorU32(background),
                                (active ? 13.0F : 20.0F) * scale);
    }
    if (active) {
        drawList->AddRectFilled(ImVec2(start.x, start.y + 12.0F * scale),
                                ImVec2(start.x + 4.0F * scale, start.y + size.y - 12.0F * scale),
                                ImGui::GetColorU32(ImGuiCol_HeaderHovered), 3.0F * scale);
    }
    const ImU32 foreground = ImGui::GetColorU32(active ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const ImVec2 iconOrigin(tileMin.x + (tileSize - 24.0F * scale) * 0.5F,
                            tileMin.y + (tileSize - 24.0F * scale) * 0.5F);
    DrawNavigationIcon(drawList, iconOrigin, page, foreground);
    if (badge > 0) {
        char badgeText[12]{};
        std::snprintf(badgeText, sizeof(badgeText), badge > 99 ? "99+" : "%u", badge);
        const ImVec2 badgeSize = ImGui::CalcTextSize(badgeText);
        const float badgeHeight = 18.0F * scale;
        const float badgeWidth = std::max(badgeHeight, badgeSize.x + 10.0F * scale);
        const ImVec2 badgeStart(tileMax.x - badgeWidth + 4.0F * scale,
                                tileMin.y - 3.0F * scale);
        drawList->AddRectFilled(badgeStart, ImVec2(badgeStart.x + badgeWidth, badgeStart.y + badgeHeight),
                                ImGui::GetColorU32(ImGuiCol_HeaderHovered), 11.0F * scale);
        drawList->AddText(ImVec2(badgeStart.x + (badgeWidth - badgeSize.x) * 0.5F,
                                 badgeStart.y + (badgeHeight - badgeSize.y) * 0.5F),
                          IM_COL32_WHITE, badgeText);
    }
    if (hovered) ImGui::SetTooltip("%s", label);
    ImGui::PopID();
    return pressed;
}

bool SidebarMenuRow(const char* id, const char* label, const unsigned count,
                    const horizon::Icon icon, const bool selected) {
    ImGui::PushID(id);
    const float scale = gRenderDpiScale;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 38.0F * scale);
    const bool pressed = ImGui::InvisibleButton("row", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    if (selected || hovered) {
        ImVec4 surface = ImGui::GetStyleColorVec4(selected ? ImGuiCol_Header : ImGuiCol_ButtonHovered);
        surface.w = selected ? 0.78F : 0.58F;
        draw->AddRectFilled(start, ImVec2(start.x + size.x, start.y + size.y),
                            ImGui::GetColorU32(surface), 7.0F * scale);
    }

    const ImU32 foreground = ImGui::GetColorU32(selected ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    horizon::DrawIcon(draw, icon,
                      ImVec2(start.x + 17.0F * scale, start.y + size.y * 0.5F),
                      17.0F * scale, foreground);

    const std::string countText = count > 99U ? "99+" : std::to_string(count);
    const ImVec2 countSize = ImGui::CalcTextSize(countText.c_str());
    const float badgeHeight = 21.0F * scale;
    const float badgeWidth = std::max(badgeHeight, countSize.x + 10.0F * scale);
    const float badgeRight = start.x + size.x - 8.0F * scale;
    const ImVec2 badgeMin(badgeRight - badgeWidth, start.y + (size.y - badgeHeight) * 0.5F);
    ImVec4 badgeSurface = ImGui::GetStyleColorVec4(selected ? ImGuiCol_HeaderHovered : ImGuiCol_Button);
    badgeSurface.w = selected ? 0.95F : 0.78F;
    draw->AddRectFilled(badgeMin, ImVec2(badgeRight, badgeMin.y + badgeHeight),
                        ImGui::GetColorU32(badgeSurface), badgeHeight * 0.5F);
    draw->AddText(ImVec2(badgeMin.x + (badgeWidth - countSize.x) * 0.5F,
                             badgeMin.y + (badgeHeight - countSize.y) * 0.5F),
                  foreground, countText.c_str());

    const float labelStart = start.x + 34.0F * scale;
    const float labelWidth = std::max(0.0F, badgeMin.x - labelStart - 8.0F * scale);
    const std::string fittedLabel = FitTextToWidth(label, labelWidth);
    ImFont* const labelFont = selected ? SemiboldUiFont() : RegularUiFont();
    const float labelFontSize = ImGui::GetFontSize();
    const ImVec2 labelSize = labelFont->CalcTextSizeA(
        labelFontSize, FLT_MAX, 0.0F, fittedLabel.c_str());
    draw->AddText(labelFont, labelFontSize,
                  ImVec2(labelStart, start.y + (size.y - labelSize.y) * 0.5F),
                  foreground, fittedLabel.c_str());
    if (hovered && fittedLabel != label) ImGui::SetTooltip("%s", label);
    ImGui::PopID();
    return pressed;
}

bool LabeledIconButton(const char* id, const char* label, const horizon::Icon icon,
                       const ImVec2 size, const bool selected) {
    ImGui::PushID(id);
    const float scale = gRenderDpiScale;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("button", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    ImVec4 surface = ImGui::GetStyleColorVec4(
        held ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    if (selected) surface = ImGui::GetStyleColorVec4(ImGuiCol_Header);
    surface.w = selected ? 0.92F : (hovered ? 0.96F : 0.82F);
    draw->AddRectFilled(start, ImVec2(start.x + size.x, start.y + size.y),
                        ImGui::GetColorU32(surface), 9.0F * scale);
    if (selected) {
        draw->AddRectFilled(ImVec2(start.x, start.y + 7.0F * scale),
                            ImVec2(start.x + 3.0F * scale, start.y + size.y - 7.0F * scale),
                            ImGui::GetColorU32(ImGuiCol_HeaderHovered), 2.0F * scale);
    }

    const ImU32 foreground = ImGui::GetColorU32(selected ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    horizon::DrawIcon(draw, icon,
                      ImVec2(start.x + 18.0F * scale, start.y + size.y * 0.5F),
                      17.0F * scale, foreground);
    const float labelStart = start.x + 34.0F * scale;
    const std::string fitted = FitTextToWidth(label,
        std::max(0.0F, size.x - 44.0F * scale));
    ImFont* const labelFont = selected ? SemiboldUiFont() : RegularUiFont();
    const float labelFontSize = ImGui::GetFontSize();
    const ImVec2 textSize = labelFont->CalcTextSizeA(
        labelFontSize, FLT_MAX, 0.0F, fitted.c_str());
    draw->AddText(labelFont, labelFontSize,
                  ImVec2(labelStart, start.y + (size.y - textSize.y) * 0.5F),
                  foreground, fitted.c_str());
    if (hovered && fitted != label) ImGui::SetTooltip("%s", label);
    ImGui::PopID();
    return pressed;
}

bool PanelLinkRow(const char* id, const char* label, const horizon::Icon icon) {
    ImGui::PushID(id);
    const float scale = gRenderDpiScale;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 44.0F * scale);
    const bool pressed = ImGui::InvisibleButton("panel-link", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    ImVec4 surface = ImGui::GetStyleColorVec4(
        held ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    surface.w = held ? 0.92F : hovered ? 0.78F : 0.42F;
    draw->AddRectFilled(start, ImVec2(start.x + size.x, start.y + size.y),
                        ImGui::GetColorU32(surface), 9.0F * scale);
    draw->AddRect(start, ImVec2(start.x + size.x, start.y + size.y),
                  ImGui::GetColorU32(ImGuiCol_Border), 9.0F * scale);

    ImVec4 iconSurface = ImGui::GetStyleColorVec4(ImGuiCol_Header);
    iconSurface.w = hovered || held ? 0.92F : 0.62F;
    const ImVec2 iconMin(start.x + 7.0F * scale, start.y + 7.0F * scale);
    const ImVec2 iconMax(iconMin.x + 30.0F * scale, iconMin.y + 30.0F * scale);
    draw->AddRectFilled(iconMin, iconMax, ImGui::GetColorU32(iconSurface), 8.0F * scale);
    horizon::DrawIcon(draw, icon,
                      ImVec2(iconMin.x + 15.0F * scale, iconMin.y + 15.0F * scale),
                      16.0F * scale, ImGui::GetColorU32(ImGuiCol_Text));

    const float labelStart = start.x + 47.0F * scale;
    const float arrowReserve = 25.0F * scale;
    const std::string fitted = FitTextToWidth(
        label, std::max(0.0F, size.x - (labelStart - start.x) - arrowReserve));
    ImFont* const font = SemiboldUiFont();
    const float fontSize = ImGui::GetFontSize();
    const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0F, fitted.c_str());
    draw->AddText(font, fontSize,
                  ImVec2(labelStart, start.y + (size.y - textSize.y) * 0.5F),
                  ImGui::GetColorU32(ImGuiCol_Text), fitted.c_str());

    const ImU32 arrowColor = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const ImVec2 arrowCenter(start.x + size.x - 15.0F * scale, start.y + size.y * 0.5F);
    draw->AddTriangleFilled(ImVec2(arrowCenter.x - 3.0F * scale, arrowCenter.y - 5.0F * scale),
                            ImVec2(arrowCenter.x + 4.0F * scale, arrowCenter.y),
                            ImVec2(arrowCenter.x - 3.0F * scale, arrowCenter.y + 5.0F * scale),
                            arrowColor);
    if (hovered && fitted != label) ImGui::SetTooltip("%s", label);
    ImGui::PopID();
    return pressed;
}

bool ChannelMenuRow(const PlatformChannel& channel, const bool selected) {
    ImGui::PushID(channel.id.c_str());
    const float scale = gRenderDpiScale;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 40.0F * scale);
    const bool pressed = ImGui::InvisibleButton("channel", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    if (selected || hovered) {
        ImVec4 surface = ImGui::GetStyleColorVec4(selected ? ImGuiCol_FrameBgActive
                                                          : ImGuiCol_ButtonHovered);
        surface.w = selected ? 0.96F : 0.72F;
        draw->AddRectFilled(start, ImVec2(start.x + size.x, start.y + size.y),
                            ImGui::GetColorU32(surface), 8.0F * scale);
    }
    if (selected) {
        draw->AddRectFilled(ImVec2(start.x, start.y + 8.0F * scale),
                            ImVec2(start.x + 3.0F * scale, start.y + size.y - 8.0F * scale),
                            ImGui::GetColorU32(ImGuiCol_HeaderHovered), 2.0F * scale);
    }

    const ImU32 foreground = ImGui::GetColorU32(selected ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const horizon::Icon icon = channel.type == "voice"
        ? horizon::Icon::VoiceChannel : horizon::Icon::TextChannel;
    horizon::DrawIcon(draw, icon,
                      ImVec2(start.x + 19.0F * scale, start.y + size.y * 0.5F),
                      17.0F * scale, foreground);

    float right = start.x + size.x - 9.0F * scale;
    if (channel.mentionCount > 0U) {
        const std::string mentions = channel.mentionCount > 99U
            ? "@99+" : "@" + std::to_string(channel.mentionCount);
        const ImVec2 mentionSize = ImGui::CalcTextSize(mentions.c_str());
        const float pillHeight = 20.0F * scale;
        const float pillWidth = std::max(pillHeight, mentionSize.x + 10.0F * scale);
        const ImVec2 pillMin(right - pillWidth, start.y + (size.y - pillHeight) * 0.5F);
        ImVec4 mentionColor = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
        mentionColor.w = 1.0F;
        draw->AddRectFilled(pillMin, ImVec2(right, pillMin.y + pillHeight),
                            ImGui::GetColorU32(mentionColor), pillHeight * 0.5F);
        draw->AddText(ImVec2(pillMin.x + (pillWidth - mentionSize.x) * 0.5F,
                             pillMin.y + (pillHeight - mentionSize.y) * 0.5F),
                      ImGui::GetColorU32(ImGuiCol_Text), mentions.c_str());
        right = pillMin.x - 7.0F * scale;
    }
    if (channel.unreadCount > 0U) {
        const std::string unread = channel.unreadCount > 99U
            ? "99+" : std::to_string(channel.unreadCount);
        const ImVec2 unreadSize = ImGui::CalcTextSize(unread.c_str());
        const float pillHeight = 20.0F * scale;
        const float pillWidth = std::max(pillHeight, unreadSize.x + 10.0F * scale);
        const ImVec2 pillMin(right - pillWidth, start.y + (size.y - pillHeight) * 0.5F);
        draw->AddRectFilled(pillMin, ImVec2(right, pillMin.y + pillHeight),
                            ImGui::GetColorU32(ImGuiCol_Header), pillHeight * 0.5F);
        draw->AddText(ImVec2(pillMin.x + (pillWidth - unreadSize.x) * 0.5F,
                             pillMin.y + (pillHeight - unreadSize.y) * 0.5F),
                      ImGui::GetColorU32(ImGuiCol_Text), unread.c_str());
        right = pillMin.x - 7.0F * scale;
    }
    if (channel.contentRating == "adult") {
        constexpr const char* adult = "18+";
        const ImVec2 adultSize = ImGui::CalcTextSize(adult);
        const float pillHeight = 20.0F * scale;
        const float pillWidth = adultSize.x + 10.0F * scale;
        const ImVec2 pillMin(right - pillWidth, start.y + (size.y - pillHeight) * 0.5F);
        ImVec4 warning = ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogramHovered);
        warning.w = 0.82F;
        draw->AddRectFilled(pillMin, ImVec2(right, pillMin.y + pillHeight),
                            ImGui::GetColorU32(warning), pillHeight * 0.5F);
        draw->AddText(ImVec2(pillMin.x + 5.0F * scale,
                             pillMin.y + (pillHeight - adultSize.y) * 0.5F),
                      ImGui::GetColorU32(ImGuiCol_WindowBg), adult);
        right = pillMin.x - 7.0F * scale;
    }

    const float labelStart = start.x + 36.0F * scale;
    const std::string fitted = FitTextToWidth(channel.name,
        std::max(0.0F, right - labelStart));
    ImFont* const labelFont = selected ? SemiboldUiFont() : RegularUiFont();
    const float labelFontSize = ImGui::GetFontSize();
    const ImVec2 textSize = labelFont->CalcTextSizeA(
        labelFontSize, FLT_MAX, 0.0F, fitted.c_str());
    draw->AddText(labelFont, labelFontSize,
                  ImVec2(labelStart, start.y + (size.y - textSize.y) * 0.5F),
                  foreground, fitted.c_str());
    if (hovered && (!channel.topic.empty() || fitted != channel.name)) {
        ImGui::SetTooltip("%s%s%s", channel.name.c_str(),
                          channel.topic.empty() ? "" : "\n",
                          channel.topic.c_str());
    }
    ImGui::PopID();
    return pressed;
}

bool SurfaceCollapsingHeader(const char* label,
                             const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_None) {
    ImVec4 hovered = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
    hovered.w = 0.74F;
    ImVec4 active = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    active.w = 0.82F;
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, hovered);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, active);
    const bool open = ImGui::CollapsingHeader(label, flags);
    ImGui::PopStyleColor(3);
    return open;
}

bool CommunityButton(const PlatformRoom& room, const bool active, const unsigned ordinal) {
    ImGui::PushID(room.id.c_str());
    const float scale = gRenderDpiScale;
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 size(width, 52.0F * scale);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("community", size);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 center(start.x + size.x * 0.5F, start.y + 25.0F * scale);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec4 surface = ImGui::GetStyleColorVec4(active ? ImGuiCol_Header : ImGuiCol_Button);
    if (hovered) surface = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
    const float rounding = (active || hovered ? 11.0F : 18.0F) * scale;
    draw->AddRectFilled(ImVec2(center.x - 23.0F * scale, center.y - 23.0F * scale),
                        ImVec2(center.x + 23.0F * scale, center.y + 23.0F * scale),
                        ImGui::GetColorU32(surface), rounding);
    if (active) {
        draw->AddRectFilled(ImVec2(start.x - 3.0F * scale, center.y - 12.0F * scale),
                            ImVec2(start.x + 2.0F * scale, center.y + 12.0F * scale),
                            ImGui::GetColorU32(ImGuiCol_HeaderHovered), 3.0F * scale);
    }
    char monogram[3]{'S', '\0', '\0'};
    for (const unsigned char value : room.name) {
        if (std::isalnum(value) != 0) {
            monogram[0] = static_cast<char>(std::toupper(value));
            break;
        }
    }
    if (monogram[0] == 'S' && ordinal < 10U) monogram[1] = static_cast<char>('0' + ordinal);
    const ImVec2 labelSize = ImGui::CalcTextSize(monogram);
    draw->AddText(ImVec2(center.x - labelSize.x * 0.5F, center.y - labelSize.y * 0.5F),
                  IM_COL32_WHITE, monogram);
    if (hovered) ImGui::SetTooltip("%s\n%s", room.name.c_str(),
                                   room.role == "owner" ? "Sahibi olduğun topluluk" : "Üye olduğun topluluk");
    ImGui::PopID();
    return pressed;
}

std::uint64_t SteadyNowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char* ServerDenoiseReasonText(const std::string& reason) {
    if (reason == "enabled") return "Sunucu gurultu engelleme etkin.";
    if (reason == "account_not_entitled") return "Hesabinizda sunucu gurultu engelleme hakki yok.";
    if (reason == "room_disabled") return "Oda yonetiminde sunucu gurultu engelleme kapali.";
    if (reason == "node_dsp_unavailable") return "Dugumde RNNoise DSP motoru hazir degil.";
    if (reason == "node_memory_pressure") return "Dugum bellek korumasi nedeniyle DSP gecici kapali.";
    if (reason == "node_cpu_guard") return "Dugum CPU korumasi nedeniyle DSP gecici kapali.";
    if (reason == "node_dsp_capacity") return "Dugumdeki DSP akisi kapasitesi dolu.";
    return "Sunucu gurultu engelleme kullanici tarafindan istenmedi.";
}

std::size_t UnicodeCodepoints(const std::string& text) {
    const std::wstring wide = Utf8ToWide(text);
    if (!text.empty() && wide.empty()) return 0;
    std::size_t count = wide.size();
    for (std::size_t index = 1; index < wide.size(); ++index) {
        if (wide[index] >= 0xDC00 && wide[index] <= 0xDFFF && wide[index - 1] >= 0xD800 && wide[index - 1] <= 0xDBFF) --count;
    }
    return count;
}

}  // namespace

AppUi::AppUi() : redrawEvent_(CreateEventW(nullptr, TRUE, TRUE, nullptr)) {}
AppUi::~AppUi() { Shutdown(); worker_.Shutdown(); if (redrawEvent_ != nullptr) CloseHandle(redrawEvent_); }
void AppUi::SetWindowHandle(const HWND window) noexcept { windowHandle_ = window; }
void AppUi::SetWindowPlacement(const int x, const int y, const int width, const int height,
                               const bool maximized) noexcept {
    settings_.hasWindowPlacement = true;
    settings_.windowX = x;
    settings_.windowY = y;
    settings_.windowWidth = std::max(width, 960);
    settings_.windowHeight = std::max(height, 640);
    settings_.windowMaximized = maximized;
}
void AppUi::SetLogoTexture(const std::uint64_t textureId) noexcept { logoTexture_ = textureId; }
void AppUi::SetStartupWarning(std::string message) { uiMessage_ = std::move(message); }
void AppUi::SetPendingInvite(std::string code) { CopyToBuffer(code, inviteBuffer_); }
void AppUi::SetPendingRoom(std::string roomId) {
    pendingRoomId_ = std::move(roomId);
    pendingRoomAutoConnect_ = !pendingRoomId_.empty();
}
bool AppUi::IsMicrophoneMuted() const noexcept { return audio_.IsMicrophoneMuted(); }
bool AppUi::IsStartupUpdateGateActive() const noexcept { return initialized_ && startupUpdateGate_; }
void AppUi::ToggleMicrophoneMuted() noexcept {
    const bool microphoneMuted = audio_.IsMicrophoneMuted();
    if (microphoneMuted) {
        // Discord-style coupling: asking to speak while deafened restores the
        // output first, then opens the microphone. This avoids a short capture
        // leak while the user still cannot hear the room.
        if (audio_.IsOutputMuted()) audio_.SetOutputMuted(false);
        audio_.SetMicrophoneMuted(false);
        return;
    }
    audio_.SetMicrophoneMuted(true);
}

void AppUi::ToggleOutputMuted() noexcept {
    const bool outputMuted = audio_.IsOutputMuted();
    if (!outputMuted) {
        // Deafening always mutes capture first. Undeafening intentionally keeps
        // a manually muted microphone muted.
        audio_.SetMicrophoneMuted(true);
        audio_.SetOutputMuted(true);
        return;
    }
    audio_.SetOutputMuted(false);
}
std::string AppUi::ActiveRoomName() const {
    if (platformRooms_.empty()) return {};
    const int index = std::clamp(selectedRoomIndex_, 0, static_cast<int>(platformRooms_.size() - 1));
    return platformRooms_[static_cast<std::size_t>(index)].name;
}

void AppUi::Initialize() {
    if (initialized_) return;
    worker_.SetWakeEvent(redrawEvent_);
    worker_.SetExceptionHandler([this](std::string message) { HandleWorkerException(std::move(message)); });
    settings_ = store_.Load();
    language_ = ParseLanguage(settings_.language);
    mediaPreferences_.sensitiveMediaMode = settings_.sensitiveMediaMode;
    if (settings_.serverDenoise) settings_.localDenoise = false;
    platform_.SetOrigin(settings_.controlOrigin);
    updater_.SetStateCallback([this] { if (redrawEvent_ != nullptr) SetEvent(redrawEvent_); });
    initialized_ = true;
    startupUpdateGate_ = true;
    startupInstallerLaunchAttempted_ = false;
    startupUpdateFailure_.clear();
    if (!updater_.CheckAsync(settings_.controlOrigin, true)) {
        startupUpdateFailure_ = "Guncelleme denetimi baslatilamadi";
    }
}

void AppUi::BeginApplicationInitialization() {
    if (applicationInitializationStarted_) return;
    applicationInitializationStarted_ = true;
    audio_.SetMasterOutputVolume(settings_.outputVolume);
    audio_.SetLocalDenoise(settings_.localDenoise);
    audio_.SetPushToTalk(settings_.pushToTalk);
    audio_.SetPushToTalkKey(settings_.pushToTalkVirtualKey);
    CopySettingsToBuffers();
    inputDevices_ = {{"", "Windows varsayilan mikrofon"}};
    outputDevices_ = {{"", "Windows varsayilan cikis"}};
    inputIndex_ = 0;
    outputIndex_ = 0;
    network_.SetAudioCallback([this](const std::uint32_t peerId,
                                     const std::uint16_t sequence,
                                     const std::uint32_t timestamp,
                                     const std::uint8_t flags,
                                     const std::span<const std::uint8_t> opus) {
        audio_.SubmitPacket(peerId, sequence, timestamp, flags, opus);
    });
    network_.SetPeerRemovedCallback([this](const std::uint32_t peerId) { audio_.RemovePeer(peerId); });
    network_.SetStateCallback([this] { if (redrawEvent_ != nullptr) SetEvent(redrawEvent_); });
    audio_.SetActivityCallback([this] {
        if (!voiceMeterVisible_.load(std::memory_order_relaxed)
            || windowHandle_ == nullptr || IsWindowVisible(windowHandle_) == FALSE
            || IsIconic(windowHandle_) != FALSE) return;
        if (redrawEvent_ != nullptr) SetEvent(redrawEvent_);
    });
    loginPending_ = true;
    if (!worker_.Submit([this] {
        std::string identityError;
        const bool identityReady = messageCrypto_.Initialize(identityError);
        if (identityReady) {
            std::string bindingId = StableDeviceBindingId();
            if (bindingId.empty()) bindingId = messageCrypto_.DeviceId();
            platform_.SetClientDeviceId(std::move(bindingId));
        }
        auto inputs = AudioEngine::EnumerateInputDevices();
        auto outputs = AudioEngine::EnumerateOutputDevices();
        inputs.insert(inputs.begin(), {"", "Windows varsayilan mikrofon"});
        outputs.insert(outputs.begin(), {"", "Windows varsayilan cikis"});
        std::string error;
        const bool restored = platform_.RestoreSession(error);
        return BackgroundWorker::Completion([this, identityReady, identityError,
                                             inputs = std::move(inputs), outputs = std::move(outputs),
                                             restored, error]() mutable {
            inputDevices_ = std::move(inputs);
            outputDevices_ = std::move(outputs);
            inputIndex_ = FindDevice(inputDevices_, settings_.inputDeviceId);
            outputIndex_ = FindDevice(outputDevices_, settings_.outputDeviceId);
            loginPending_ = false;
            if (!identityReady) {
                uiMessage_ = "Cihaz kimligi hazirlanamadi: " + identityError;
                return;
            }
            if (restored) {
                uiMessage_.clear();
                StartRealtime();
                RefreshClientExperience();
                RefreshPlatformRooms();
                RefreshSocial();
                RefreshGuardian();
            }
            else if (error != "Kayitli oturum yok") uiMessage_ = error;
        });
    })) loginPending_ = false;
}

bool AppUi::HandleStartupUpdateGate() {
    if (!startupUpdateGate_) return false;
    const UpdateState state = updater_.State();
    observedUpdateState_ = state;
    if (state == UpdateState::Current) {
        startupUpdateGate_ = false;
        startupUpdateFailure_.clear();
        BeginApplicationInitialization();
        return false;
    }
    if (state == UpdateState::Available) {
        if (!updater_.DownloadAsync()) {
            startupUpdateFailure_ = "Zorunlu guncelleme indirmesi baslatilamadi";
        }
        return true;
    }
    if (state == UpdateState::Ready && !startupInstallerLaunchAttempted_) {
        startupInstallerLaunchAttempted_ = true;
        SaveSettings();
        std::string error;
        if (updater_.LaunchInstaller(error)) {
            PostMessageW(windowHandle_, kMessageExitApplication, 0, 0);
        } else {
            startupUpdateFailure_ = error.empty() ? "Kurulum baslatilamadi" : std::move(error);
        }
    }
    return true;
}

void AppUi::HandleWorkerException(std::string message) {
    loginPending_ = false;
    roomsRefreshPending_ = false;
    overviewRefreshPending_ = false;
    socialRefreshPending_ = false;
    accountSecurityRefreshPending_ = false;
    guardianRefreshPending_ = false;
    guardianPreferenceUpdatePending_ = false;
    clientExperienceRefreshPending_ = false;
    connectPending_ = false;
    audioRestartPending_ = false;
    platformActionPending_ = false;
    roomMembersRefreshPending_ = false;
    chatRefreshPending_ = false;
    chatRefreshGeneration_.Reset();
    chatSendPending_ = false;
    mediaUploadPending_ = false;
    mediaDownloadPending_ = false;
    mediaReportPending_ = false;
    pinsRefreshPending_ = false;
    updateDownloadPending_ = false;
    manualUpdateCheckPending_ = false;
    diagnosticUploadPending_ = false;
    DiagnosticLog("worker", "task_exception=" + message.substr(0, 160));
    uiMessage_ = "Arka plan islemi tamamlanamadi. Tekrar deneyin; ayrinti tanilama kaydina yazildi.";
}

void AppUi::WipeChatLines() noexcept {
    for (auto& line : chatLines_) {
        if (!line.text.empty()) crypto_wipe(line.text.data(), line.text.size());
    }
    chatLines_.clear();
}

const char* AppUi::T(const TextId id) const noexcept { return Translate(language_, id); }
Language AppUi::ActiveLanguage() const noexcept { return language_; }
UiTheme AppUi::ActiveTheme() const noexcept { return settings_.uiTheme; }
ResourceProfile AppUi::ActiveResourceProfile() const noexcept { return settings_.resourceProfile; }
float AppUi::ActiveTextScale() const noexcept { return settings_.textScale; }
int AppUi::ActiveUiDensity() const noexcept { return settings_.uiDensity; }
bool AppUi::ActiveHighContrast() const noexcept { return settings_.highContrast; }
int AppUi::ActiveColorVisionMode() const noexcept { return settings_.colorVisionMode; }
float AppUi::ActiveCustomAccentR() const noexcept { return settings_.customAccentR; }
float AppUi::ActiveCustomAccentG() const noexcept { return settings_.customAccentG; }
float AppUi::ActiveCustomAccentB() const noexcept { return settings_.customAccentB; }
bool AppUi::ConsumeFontReloadRequest() noexcept { return fontReloadRequested_.exchange(false); }

void AppUi::Shutdown() {
    if (!initialized_) return;
    voiceMeterVisible_.store(false, std::memory_order_relaxed);
    updater_.SetStateCallback({});
    DismissWindowsNotification();
    realtime_.Stop();
    audio_.Stop();
    network_.Disconnect();
    audio_.ClearPeerMixControls();
    for (auto& [draftId, draft] : chatDrafts_) {
        (void)draftId;
        if (!draft.empty()) crypto_wipe(draft.data(), draft.size());
    }
    chatDrafts_.clear();
    SaveSettings();
    initialized_ = false;
}

void AppUi::ShowWindowsNotification(const std::string& title, const std::string& body) {
    if (windowHandle_ == nullptr || GetForegroundWindow() == windowHandle_
        || ownPresence_ == "do_not_disturb") return;
    NOTIFYICONDATAW notification{};
    notification.cbSize = sizeof(notification);
    notification.hWnd = windowHandle_;
    notification.uID = 1;
    notification.uFlags = NIF_INFO;
    std::string safeTitle = title;
    std::string safeBody = body;
    if (settings_.notificationPreview == 2) {
        safeTitle = "Sonalis";
        safeBody = "Yeni bildirim";
    } else if (settings_.notificationPreview == 1) {
        const auto separator = safeBody.find(':');
        safeBody = separator == std::string::npos ? "Yeni mesaj" : safeBody.substr(0, separator);
    }
    const std::wstring wideTitle = Utf8ToWide(safeTitle);
    const std::wstring wideBody = Utf8ToWide(safeBody);
    lstrcpynW(notification.szInfoTitle, wideTitle.c_str(), ARRAYSIZE(notification.szInfoTitle));
    lstrcpynW(notification.szInfo, wideBody.c_str(), ARRAYSIZE(notification.szInfo));
    notification.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    notificationIconVisible_ = DynamicShellNotify(NIM_MODIFY, &notification);
    notificationIconExpiresMs_ = SteadyNowMs() + 15'000;
}

void AppUi::DismissWindowsNotification() noexcept {
    if (!notificationIconVisible_ || windowHandle_ == nullptr) return;
    NOTIFYICONDATAW notification{};
    notification.cbSize = sizeof(notification);
    notification.hWnd = windowHandle_;
    notification.uID = 1;
    notification.uFlags = NIF_INFO;
    notification.szInfo[0] = L'\0';
    DynamicShellNotify(NIM_MODIFY, &notification);
    notificationIconVisible_ = false;
}

void AppUi::CopySettingsToBuffers() {
    CopyToBuffer(settings_.controlOrigin, serverBuffer_);
    CopyToBuffer(settings_.nickname, nicknameBuffer_);
    CopyToBuffer(settings_.room, roomBuffer_);
}

void AppUi::RefreshPlatformRooms() {
    if (roomsRefreshPending_) return;
    roomsRefreshPending_ = true;
    if (!worker_.Submit([this] {
        std::string error;
        auto rooms = platform_.Rooms(error);
        return BackgroundWorker::Completion([this, rooms = std::move(rooms), error]() mutable {
            roomsRefreshPending_ = false;
            if (!error.empty()) {
                DiagnosticLog("rooms", "refresh_failed=" + error);
                uiMessage_ = "Oda listesi alınamadı: " + UserFacingError(error);
                return;
            }
            platformRooms_ = std::move(rooms);
            uiMessage_.clear();
            selectedRoomIndex_ = 0;
            for (int index = 0; index < static_cast<int>(platformRooms_.size()); ++index) {
                if (platformRooms_[static_cast<std::size_t>(index)].id == settings_.lastRoomId) {
                    selectedRoomIndex_ = index;
                    break;
                }
            }
            bool pendingRoomFound = pendingRoomId_.empty();
            for (int index = 0; index < static_cast<int>(platformRooms_.size()); ++index) {
                if (!pendingRoomId_.empty()
                    && platformRooms_[static_cast<std::size_t>(index)].id == pendingRoomId_) {
                    selectedRoomIndex_ = index;
                    pendingRoomFound = true;
                    break;
                }
            }
            if (!pendingRoomFound) {
                pendingRoomAutoConnect_ = false;
                uiMessage_ = "Oda baglantisi bulunamadi veya bu odanin uyesi degilsiniz.";
            }
            if (!platformRooms_.empty()) {
                const auto& selected = platformRooms_[static_cast<std::size_t>(selectedRoomIndex_)];
                settings_.lastRoomId = selected.id;
                CopyToBuffer(selected.name, roomBuffer_);
                SaveSettings();
                RefreshRoomOverview();
            }
            pendingRoomId_.clear();
        });
    })) roomsRefreshPending_ = false;
}

void AppUi::RefreshRoomOverview() {
    if (overviewRefreshPending_ || platformRooms_.empty()) return;
    const int roomIndex = std::clamp(selectedRoomIndex_, 0, static_cast<int>(platformRooms_.size() - 1));
    const std::string roomId = platformRooms_[static_cast<std::size_t>(roomIndex)].id;
    overviewRefreshPending_ = true;
    if (!worker_.Submit([this, roomId] {
        std::string error;
        auto overview = platform_.RoomOverview(roomId, error);
        return BackgroundWorker::Completion([this, roomId, overview = std::move(overview), error]() mutable {
            overviewRefreshPending_ = false;
            if (!error.empty() || !overview) {
                if (!error.empty()) DiagnosticLog("room_overview", "refresh_failed=" + error);
                uiMessage_ = error.empty() ? "Topluluk kanalları alınamadı." : UserFacingError(error);
                return;
            }
            if (platformRooms_.empty()) return;
            const int current = std::clamp(selectedRoomIndex_, 0, static_cast<int>(platformRooms_.size() - 1));
            if (platformRooms_[static_cast<std::size_t>(current)].id != roomId) return;
            roomOverview_ = std::move(overview);
            roomMembers_ = roomOverview_->members;
            const auto selected = std::find_if(roomOverview_->channels.begin(), roomOverview_->channels.end(),
                [this](const PlatformChannel& channel) { return channel.id == selectedChannelId_; });
            if (selected == roomOverview_->channels.end()) {
                const auto text = std::find_if(roomOverview_->channels.begin(), roomOverview_->channels.end(),
                    [](const PlatformChannel& channel) { return channel.type == "text"; });
                if (text != roomOverview_->channels.end()) {
                    selectedChannelId_ = text->id;
                    selectedChannelType_ = text->type;
                    chatChannelId_ = text->id;
                    const auto draft = chatDrafts_.find(chatChannelId_);
                    CopyToBuffer(draft == chatDrafts_.end() ? std::string{} : draft->second, chatBuffer_);
                }
            }
            uiMessage_.clear();
        });
    })) overviewRefreshPending_ = false;
}

void AppUi::RefreshSocial() {
    if (socialRefreshPending_) return;
    socialRefreshPending_ = true;
    if (!worker_.Submit([this] {
        std::string error;
        auto friends = platform_.Friends(error);
        std::vector<PlatformNotification> notifications;
        if (error.empty()) notifications = platform_.Notifications(error);
        std::vector<PlatformConversation> conversations;
        if (error.empty()) conversations = platform_.Conversations(error);
        return BackgroundWorker::Completion([this, friends = std::move(friends), notifications = std::move(notifications),
                                              conversations = std::move(conversations), error]() mutable {
            socialRefreshPending_ = false;
            if (!error.empty()) {
                DiagnosticLog("social", "refresh_failed=" + error);
                uiMessage_ = "Sosyal veriler alınamadı: " + UserFacingError(error);
                return;
            }
            friends_ = std::move(friends);
            notifications_ = std::move(notifications);
            conversations_ = std::move(conversations);
        });
    })) socialRefreshPending_ = false;
}

void AppUi::RefreshAccountSecurity() {
    if (accountSecurityRefreshPending_ || !platform_.IsAuthenticated()) return;
    accountSecurityRefreshPending_ = true;
    if (!worker_.Submit([this] {
        std::string error;
        auto sessions = platform_.Sessions(error);
        std::vector<PlatformClientDevice> devices;
        if (error.empty()) devices = platform_.ClientDevices(error);
        return BackgroundWorker::Completion([this, sessions = std::move(sessions),
                                              devices = std::move(devices), error]() mutable {
            accountSecurityRefreshPending_ = false;
            if (!error.empty()) {
                DiagnosticLog("account_security", "refresh_failed=" + error.substr(0, 96));
                uiMessage_ = UserFacingError(error);
                return;
            }
            accountSessions_ = std::move(sessions);
            accountDevices_ = std::move(devices);
            accountSecurityLoaded_ = true;
        });
    })) accountSecurityRefreshPending_ = false;
}

void AppUi::RefreshGuardian() {
    if (guardianRefreshPending_ || !platform_.IsAuthenticated()) return;
    guardianRefreshPending_ = true;
    if (!worker_.Submit([this] {
        std::string error;
        auto config = platform_.MediaConfig(error);
        auto preferences = error.empty() ? platform_.GetMediaPreferences(error)
                                         : std::optional<MediaPreferences>{};
        auto restrictions = error.empty() ? platform_.Restrictions(error)
                                          : std::vector<AccountRestriction>{};
        return BackgroundWorker::Completion([
            this,
            config = std::move(config),
            preferences = std::move(preferences),
            restrictions = std::move(restrictions),
            error]() mutable {
            guardianRefreshPending_ = false;
            if (!error.empty() || !config || !preferences) {
                DiagnosticLog("guardian", "refresh_failed=" + error);
                return;
            }
            mediaSafetyConfig_ = std::move(config);
            mediaPreferences_ = *preferences;
            restrictions_ = std::move(restrictions);
            settings_.sensitiveMediaMode = mediaPreferences_.sensitiveMediaMode;
            SaveSettings();
        });
    })) guardianRefreshPending_ = false;
}

void AppUi::RefreshClientExperience() {
    if (clientExperienceRefreshPending_ || !platform_.IsAuthenticated()) return;
    clientExperienceRefreshPending_ = true;
    if (!worker_.Submit([this] {
        std::string error;
        auto policy = platform_.ClientExperience(error);
        return BackgroundWorker::Completion([this, policy = std::move(policy), error]() mutable {
            clientExperienceRefreshPending_ = false;
            if (!policy) {
                DiagnosticLog("client_experience", "refresh_failed=" + error);
                return;
            }
            const UiTheme previousTheme = settings_.uiTheme;
            const ResourceProfile previousProfile = settings_.resourceProfile;
            const bool firstPolicy = settings_.experiencePolicyVersion == 0;
            experiencePolicy_ = *policy;
            if (firstPolicy || !experiencePolicy_.allowUserThemeChoice) {
                settings_.uiTheme = experiencePolicy_.defaultTheme;
            }
            if (firstPolicy || !experiencePolicy_.allowUserResourceProfileChoice) {
                settings_.resourceProfile = experiencePolicy_.defaultResourceProfile;
            }
            if (!experiencePolicy_.customAccentsEnabled && settings_.uiTheme == UiTheme::Custom) {
                settings_.uiTheme = experiencePolicy_.defaultTheme;
            }
            if (firstPolicy && experiencePolicy_.accentHex.size() == 7U) {
                try {
                    settings_.customAccentR = static_cast<float>(
                        std::stoi(experiencePolicy_.accentHex.substr(1, 2), nullptr, 16)) / 255.0F;
                    settings_.customAccentG = static_cast<float>(
                        std::stoi(experiencePolicy_.accentHex.substr(3, 2), nullptr, 16)) / 255.0F;
                    settings_.customAccentB = static_cast<float>(
                        std::stoi(experiencePolicy_.accentHex.substr(5, 2), nullptr, 16)) / 255.0F;
                } catch (...) {
                    DiagnosticLog("client_experience", "accent_parse_failed");
                }
            }
            settings_.experiencePolicyVersion = experiencePolicy_.policyVersion;
            const std::size_t maximumResolvedMessages = std::min(
                experiencePolicy_.maximumResolvedMessages,
                BudgetFor(settings_.resourceProfile).maximumResolvedMessages);
            while (chatLines_.size() > maximumResolvedMessages) {
                chatLines_.erase(chatLines_.begin());
            }
            SaveSettings();
            if (previousTheme != settings_.uiTheme || previousProfile != settings_.resourceProfile) {
                fontReloadRequested_.store(true);
            }
            if (redrawEvent_ != nullptr) SetEvent(redrawEvent_);
        });
    })) clientExperienceRefreshPending_ = false;
}

void AppUi::UpdateGuardianPreferences() {
    if (guardianPreferenceUpdatePending_ || !platform_.IsAuthenticated()) return;
    if (mediaPreferences_.lockedForMinor) {
        mediaPreferences_.sensitiveMediaMode = SensitiveMediaMode::Block;
        settings_.sensitiveMediaMode = SensitiveMediaMode::Block;
    }
    const MediaPreferences requested = mediaPreferences_;
    guardianPreferenceUpdatePending_ = true;
    if (!worker_.Submit([this, requested] {
        std::string error;
        const bool updated = platform_.UpdateMediaPreferences(requested, error);
        return BackgroundWorker::Completion([this, requested, updated, error] {
            guardianPreferenceUpdatePending_ = false;
            if (!updated) {
                uiMessage_ = UserFacingError(error);
                RefreshGuardian();
                return;
            }
            mediaPreferences_ = requested;
            settings_.sensitiveMediaMode = requested.sensitiveMediaMode;
            SaveSettings();
        });
    })) guardianPreferenceUpdatePending_ = false;
}

void AppUi::RefreshRoomMembers() {
    if (roomMembersRefreshPending_ || platformRooms_.empty()) return;
    const int roomIndex = std::clamp(selectedRoomIndex_, 0, static_cast<int>(platformRooms_.size() - 1));
    const std::string roomId = platformRooms_[static_cast<std::size_t>(roomIndex)].id;
    roomMembersRefreshPending_ = true;
    if (!worker_.Submit([this, roomId] {
        std::string error;
        auto members = platform_.RoomMembers(roomId, error);
        return BackgroundWorker::Completion([this, roomId, members = std::move(members), error]() mutable {
            roomMembersRefreshPending_ = false;
            if (!error.empty()) {
                DiagnosticLog("room_members", "refresh_failed=" + error);
                uiMessage_ = UserFacingError(error);
                return;
            }
            if (!platformRooms_.empty()) {
                const int current = std::clamp(selectedRoomIndex_, 0, static_cast<int>(platformRooms_.size() - 1));
                if (platformRooms_[static_cast<std::size_t>(current)].id == roomId) {
                    roomMembers_ = std::move(members);
                    if (roomOverview_ && roomOverview_->roomId == roomId) {
                        roomOverview_->members = roomMembers_;
                    }
                }
            }
        });
    })) roomMembersRefreshPending_ = false;
}

void AppUi::QueuePlatformAction(std::function<bool(std::string&)> action,
                                std::string successMessage,
                                const bool refreshRooms,
                                const bool refreshSocial,
                                const bool refreshMembers,
                                const bool refreshOverview,
                                std::function<void()> onSuccess) {
    if (platformActionPending_) return;
    platformActionPending_ = true;
    if (!worker_.Submit([this, action = std::move(action), successMessage = std::move(successMessage),
                         refreshRooms, refreshSocial, refreshMembers, refreshOverview,
                         onSuccess = std::move(onSuccess)]() mutable {
        std::string error;
        const bool succeeded = action(error);
        return BackgroundWorker::Completion([this, succeeded, error, successMessage = std::move(successMessage),
                                              refreshRooms, refreshSocial, refreshMembers, refreshOverview,
                                              onSuccess = std::move(onSuccess)] {
            platformActionPending_ = false;
            uiMessage_ = succeeded ? successMessage : UserFacingError(error);
            if (!succeeded) {
                DiagnosticLog("platform_action", "failed=" + error);
                return;
            }
            if (onSuccess) onSuccess();
            if (refreshRooms) RefreshPlatformRooms();
            if (refreshSocial) RefreshSocial();
            if (refreshMembers) RefreshRoomMembers();
            if (refreshOverview) RefreshRoomOverview();
        });
    })) {
        platformActionPending_ = false;
        uiMessage_ = "Arka plan is kuyrugu dolu";
    }
}

void AppUi::RenderLogin() {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float dpiScale = ResolveDpiScale(windowHandle_);
    const float logicalWidth = available.x / dpiScale;
    const bool splitLayout = logicalWidth >= 1120.0F;
    const float outerPadding = 30.0F * dpiScale;
    const float cardWidth = std::min(468.0F * dpiScale,
                                     std::max(320.0F * dpiScale, available.x - outerPadding * 2.0F));
    const float cardHeight = std::min(590.0F * dpiScale,
                                      std::max(520.0F * dpiScale, available.y - outerPadding * 2.0F));
    const float top = std::max(outerPadding, (available.y - cardHeight) * 0.5F);

    if (splitLayout) {
        const float heroWidth = std::max(420.0F * dpiScale,
            available.x - cardWidth - outerPadding * 3.0F);
        ImGui::SetCursorPos(ImVec2(outerPadding, top));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.025F, 0.045F, 0.085F, 1.0F));
        ImGui::BeginChild("loginHero", ImVec2(heroWidth, cardHeight),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 heroStart = ImGui::GetWindowPos();
        const ImVec2 heroEnd(heroStart.x + ImGui::GetWindowWidth(), heroStart.y + ImGui::GetWindowHeight());
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
            heroStart, heroEnd,
            IM_COL32(10, 31, 71, 255), IM_COL32(8, 54, 86, 255),
            IM_COL32(4, 19, 40, 255), IM_COL32(7, 20, 48, 255));
        ImGui::SetCursorPos(ImVec2(36.0F * dpiScale, 42.0F * dpiScale));
        if (logoTexture_ != 0) {
            ImGui::Image(ImTextureRef(static_cast<ImTextureID>(logoTexture_)),
                         ImVec2(82.0F * dpiScale, 82.0F * dpiScale));
        }
        ImGui::SetCursorPosX(36.0F * dpiScale);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.05F);
        ImGui::TextUnformatted("Sonalis Horizon");
        ImGui::PopFont();
        ImGui::SetCursorPosX(36.0F * dpiScale);
        ImGui::PushTextWrapPos(heroWidth - 42.0F * dpiScale);
        ImGui::TextColored(ImVec4(0.70F, 0.79F, 0.92F, 1.0F),
                           "Toplulukların, mesajların ve düşük gecikmeli sesin için sakin bir çalışma alanı.");
        ImGui::PopTextWrapPos();
        ImGui::SetCursorPosY(245.0F * dpiScale);
        constexpr std::array<std::pair<horizon::Icon, const char*>, 3> benefits{{
            {horizon::Icon::VoiceChannel, "Düşük gecikmeli, şifreli ses"},
            {horizon::Icon::Messages, "Güvenli topluluk ve özel mesajlar"},
            {horizon::Icon::Shield, "Guardian güvenlik denetimleri"},
        }};
        for (const auto& [icon, label] : benefits) {
            ImGui::SetCursorPosX(38.0F * dpiScale);
            const ImVec2 iconStart = ImGui::GetCursorScreenPos();
            horizon::DrawIcon(ImGui::GetWindowDrawList(), icon,
                              ImVec2(iconStart.x + 14.0F * dpiScale,
                                     iconStart.y + 14.0F * dpiScale),
                              22.0F * dpiScale, ImGui::GetColorU32(ImGuiCol_HeaderHovered));
            ImGui::Dummy(ImVec2(34.0F * dpiScale, 30.0F * dpiScale));
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0F * dpiScale);
            ImGui::TextUnformatted(label);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12.0F * dpiScale);
        }
        ImGui::SetCursorPos(ImVec2(38.0F * dpiScale, cardHeight - 56.0F * dpiScale));
        ImGui::TextDisabled("Native Windows · Electron ve WebView içermez");
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0F, outerPadding);
    } else {
        ImGui::SetCursorPos(ImVec2((available.x - cardWidth) * 0.5F, top));
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14.0F * dpiScale);
    ImGui::BeginChild("login", ImVec2(cardWidth, cardHeight),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    if (logoTexture_ != 0) {
        const float logoSize = 58.0F * dpiScale;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - logoSize) * 0.5F);
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(logoTexture_)), ImVec2(logoSize, logoSize));
    }
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25F);
    ImGui::TextUnformatted(T(TextId::AccountLogin));
    ImGui::PopFont();
    // This sentence is intentionally longer in several supported languages.
    // Wrap it to the login card instead of allowing it to be clipped by the
    // child window (or to overlap a compact/high-DPI layout).
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextDisabled("%s", T(TextId::PasswordNote));
    ImGui::PopTextWrapPos();
    ImGui::Spacing(); FieldLabel(T(TextId::ControlPlane)); ImGui::InputText("##origin", serverBuffer_.data(), serverBuffer_.size());
    FieldLabel(T(TextId::UsernameOrEmail)); ImGui::InputText("##login", loginBuffer_.data(), loginBuffer_.size());
    FieldLabel(T(TextId::Password)); ImGui::InputText("##password", passwordBuffer_.data(), passwordBuffer_.size(), ImGuiInputTextFlags_Password);
    if (loginPending_) ImGui::BeginDisabled();
    if (ImGui::Button(loginPending_ ? T(TextId::Connecting) : T(TextId::Login), ImVec2(-FLT_MIN, 40.0F))) {
        settings_.controlOrigin = serverBuffer_.data(); platform_.SetOrigin(settings_.controlOrigin); SaveSettings();
        const std::string login(loginBuffer_.data());
        const std::string password(passwordBuffer_.data());
        passwordBuffer_.fill('\0');
        loginPending_ = true;
        if (!worker_.Submit([this, login, password] {
            std::string error;
            const std::string installationId = StableDeviceBindingId();
            const bool identityReady = messageCrypto_.IsReady() || messageCrypto_.Initialize(error);
            std::string tpmError;
            const bool tpmReady = nativeDeviceSigner_.IsReady() || nativeDeviceSigner_.Initialize(installationId, tpmError);
            auto attempt = [this, &login, &password, &installationId, &error](
                               const std::string& publicKey, const std::string& keyAlgorithm,
                               const auto& signer) {
                if (publicKey.empty()) return false;
                const auto challenge = platform_.RequestNativeLoginChallenge(publicKey, keyAlgorithm, error);
                if (!challenge) return false;
                const std::string canonical = challenge->id + "\n" + challenge->challenge + "\n" + installationId;
                const std::string signature = signer(canonical, error);
                return !signature.empty()
                    && platform_.Login(login, password, publicKey, keyAlgorithm, *challenge, signature, error);
            };
            bool loggedIn = false;
            if (tpmReady) {
                loggedIn = attempt(nativeDeviceSigner_.PublicKey(), nativeDeviceSigner_.Algorithm(),
                    [this](const std::string& canonical, std::string& signError) {
                        return nativeDeviceSigner_.Sign(canonical, signError);
                    });
            }
            // TPM olmayan makinelerde ve TPM'nin gecici olarak imza veremedigi
            // durumlarda DPAPI korumali Ed25519 kimligi guvenli geri donustur.
            // Hatali kullanici parolasinda ikinci deneme yaparak rate limit
            // tuketmemek icin yalniz TPM imzasi bos kaldiginda fallback yapilir.
            if (!loggedIn && identityReady && (!tpmReady || error == "TPM native giris imzasi uretilemedi")) {
                error.clear();
                loggedIn = attempt(messageCrypto_.SigningPublicKey(), "ed25519-raw-v1",
                    [this](const std::string& canonical, std::string&) { return messageCrypto_.Sign(canonical); });
            }
            if (!loggedIn && error.empty()) error = tpmError.empty() ? "Native giris kaniti uretilemedi" : tpmError;
            return BackgroundWorker::Completion([this, loggedIn, error] {
                loginPending_ = false;
                if (!loggedIn) { uiMessage_ = error; return; }
                uiMessage_.clear();
                StartRealtime();
                RefreshClientExperience();
                RefreshPlatformRooms();
                RefreshSocial();
                RefreshGuardian();
            });
        })) { loginPending_ = false; uiMessage_ = "Arka plan is kuyrugu dolu"; }
    }
    if (loginPending_) ImGui::EndDisabled();
    if (ImGui::Button(T(TextId::CreateAccount), ImVec2(-FLT_MIN, 34.0F))) {
        const std::wstring url = Utf8ToWide(platform_.Origin() + "/register"); DynamicShellExecute(url);
    }
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const auto& languages = SupportedLanguages();
    const auto currentLanguage = std::find_if(languages.begin(), languages.end(), [this](const LanguageOption& option) {
        return option.language == language_;
    });
    const char* preview = currentLanguage == languages.end() ? "English" : LanguageDisplayName(currentLanguage->language);
    if (ImGui::BeginCombo("##loginLanguage", preview)) {
        for (const auto& option : languages) {
            const bool selected = option.language == language_;
            if (ImGui::Selectable(LanguageDisplayName(option.language), selected)) {
                language_ = option.language;
                settings_.language = std::string(option.code);
                SaveSettings();
                fontReloadRequested_.store(true);
                if (redrawEvent_ != nullptr) SetEvent(redrawEvent_);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (!uiMessage_.empty()) ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.35F, 1.0F), "%s", uiMessage_.c_str());
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

int AppUi::FindDevice(const std::vector<AudioDeviceInfo>& devices, const std::string& id) {
    const auto found = std::find_if(devices.begin(), devices.end(), [&id](const AudioDeviceInfo& device) {
        return device.id == id;
    });
    return found == devices.end() ? 0 : static_cast<int>(std::distance(devices.begin(), found));
}

void AppUi::RefreshDevices() {
    inputDevices_ = AudioEngine::EnumerateInputDevices();
    outputDevices_ = AudioEngine::EnumerateOutputDevices();
    inputDevices_.insert(inputDevices_.begin(), {"", "Windows varsayilan mikrofon"});
    outputDevices_.insert(outputDevices_.begin(), {"", "Windows varsayilan cikis"});
    inputIndex_ = FindDevice(inputDevices_, settings_.inputDeviceId);
    outputIndex_ = FindDevice(outputDevices_, settings_.outputDeviceId);
}

bool AppUi::DeviceCombo(const char* label,
                        const std::vector<AudioDeviceInfo>& devices,
                        int& selectedIndex,
                        const bool enabled) {
    if (devices.empty()) return false;
    selectedIndex = std::clamp(selectedIndex, 0, static_cast<int>(devices.size() - 1));
    bool changed = false;
    if (!enabled) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(label, devices[static_cast<std::size_t>(selectedIndex)].name.c_str())) {
        for (int index = 0; index < static_cast<int>(devices.size()); ++index) {
            const bool selected = index == selectedIndex;
            if (ImGui::Selectable(devices[static_cast<std::size_t>(index)].name.c_str(), selected)) {
                selectedIndex = index;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (!enabled) ImGui::EndDisabled();
    return changed;
}

void AppUi::Connect() {
    if (connectPending_) return;
    if (!platform_.IsAuthenticated() || platformRooms_.empty()) { uiMessage_ = "Once bir oda secin"; return; }
    selectedRoomIndex_ = std::clamp(selectedRoomIndex_, 0, static_cast<int>(platformRooms_.size() - 1));
    const PlatformRoom room = platformRooms_[static_cast<std::size_t>(selectedRoomIndex_)];
    settings_.lastRoomId = room.id; settings_.room = room.name;
    settings_.inputDeviceId = inputDevices_[static_cast<std::size_t>(inputIndex_)].id;
    settings_.outputDeviceId = outputDevices_[static_cast<std::size_t>(outputIndex_)].id;
    SaveSettings();
    audio_.ClearPeerMixControls();
    const std::string input = settings_.inputDeviceId;
    const std::string output = settings_.outputDeviceId;
    const bool voiceActivation = settings_.voiceActivation;
    const float sensitivity = settings_.vadSensitivity;
    const bool serverDenoise = settings_.serverDenoise;
    const bool p2pEnabled = settings_.p2pEnabled;
    std::string voiceChannelId = selectedChannelType_ == "voice" ? selectedChannelId_ : activeVoiceChannelId_;
    if (voiceChannelId.empty() && roomOverview_) {
        const auto voice = std::find_if(roomOverview_->channels.begin(), roomOverview_->channels.end(),
            [](const PlatformChannel& channel) { return channel.type == "voice"; });
        if (voice != roomOverview_->channels.end()) voiceChannelId = voice->id;
    }
    if (roomOverview_ && voiceChannelId.empty()) {
        uiMessage_ = "Bu toplulukta kullanilabilir bir ses kanali yok";
        return;
    }
    connectPending_ = true;
    voiceSleeping_ = false;
    if (!worker_.Submit([this, room, input, output, voiceActivation, sensitivity, serverDenoise, p2pEnabled,
                         voiceChannelId] {
        std::string error;
        const auto grant = platform_.RequestVoiceGrant(room.id, serverDenoise, p2pEnabled, error, voiceChannelId);
        std::vector<PlatformMember> members;
        bool connected = false;
        bool canSpeak = true;
        std::string denoiseReason = serverDenoise ? "node_dsp_unavailable" : "client_not_requested";
        if (grant) {
            denoiseReason = grant->serverDenoiseReason;
            canSpeak = grant->canSpeak;
        }
        if (grant) members = platform_.RoomMembers(room.id, error);
        if (grant && error.empty()) connected = network_.ConnectSecure(*grant, members, grant->p2pEnabled, error);
        bool microphoneAvailable = false;
        if (connected) {
            audio_.SetEncoderBitrate(grant->bitrate);
            audio_.SetTransmitAllowed(canSpeak);
            if (!audio_.Start(input, output, &network_, voiceActivation, sensitivity, error)) {
                network_.Disconnect(); connected = false;
            } else {
                microphoneAvailable = audio_.HasCaptureDevice();
            }
        }
        DiagnosticLog("connect", connected ? "ready" : "failed=" + error);
        return BackgroundWorker::Completion([this, connected, error, denoiseReason, serverDenoise, voiceChannelId,
                                             microphoneAvailable, canSpeak] {
            connectPending_ = false;
            if (connected) activeVoiceChannelId_ = voiceChannelId;
            voiceCanSpeak_ = connected ? canSpeak : true;
            serverDenoiseReason_ = denoiseReason;
            if (!connected) uiMessage_ = error;
            else if (!canSpeak) {
                uiMessage_ = "Hesap kısıtlaması nedeniyle bu ses kanalına yalnız dinleyici olarak bağlandınız.";
            }
            else if (!microphoneAvailable) {
                uiMessage_ = "Mikrofon bulunamadi; dinleme modunda baglandiniz. RDP mikrofon yonlendirmesini acip cihazlari yenileyin.";
            } else {
                uiMessage_ = serverDenoise && denoiseReason != "enabled"
                    ? ServerDenoiseReasonText(denoiseReason) : std::string{};
            }
        });
    })) { connectPending_ = false; uiMessage_ = "Arka plan is kuyrugu dolu"; }
}

void AppUi::Disconnect() {
    voiceSleeping_ = false;
    voiceCanSpeak_ = true;
    audio_.SetTransmitAllowed(true);
    audio_.Stop();
    network_.Disconnect();
    audio_.ClearPeerMixControls();
}

void AppUi::RefreshChat(const bool loadOlder) {
    chatLoadOlderRequested_ = chatLoadOlderRequested_ || loadOlder;
    (void)chatRefreshGeneration_.MarkDirty();
    std::uint64_t refreshToken = 0;
    if (!chatRefreshGeneration_.TryBegin(refreshToken)) return;
    const std::string directId = directConversationId_;
    const std::string channelId = directId.empty() ? chatChannelId_ : std::string{};
    const std::string roomId = platformRooms_.empty() ? std::string{} : platformRooms_[static_cast<std::size_t>(
        std::clamp(selectedRoomIndex_, 0, static_cast<int>(platformRooms_.size() - 1)))].id;
    const auto friendSnapshot = friends_;
    const PlatformUser account = platform_.User();
    const std::string existingConversationId = chatConversationId_;
    const int existingEpoch = chatEpoch_;
    const bool existingKeyReady = chatKeyReady_;
    const auto existingKey = chatKey_;
    const bool loadingOlder = chatLoadOlderRequested_;
    chatLoadOlderRequested_ = false;
    const bool sameConversation = existingKeyReady && !existingConversationId.empty();
    const std::string requestedAfterCursor = !loadingOlder && sameConversation ? chatAfterCursor_ : std::string{};
    const std::string requestedBeforeCursor = loadingOlder && sameConversation ? chatBeforeCursor_ : std::string{};
    const std::string existingBeforeCursor = chatBeforeCursor_;
    const std::string existingAfterCursor = chatAfterCursor_;
    const bool existingHasMore = chatHasMore_;
    const std::size_t maximumResolvedMessages = std::min(
        experiencePolicy_.maximumResolvedMessages,
        BudgetFor(settings_.resourceProfile).maximumResolvedMessages);
    const auto existingLines = sameConversation ? chatLines_ : std::vector<ChatLine>{};
    chatRefreshPending_ = true;
    if (!worker_.Submit([this, directId, channelId, roomId, friendSnapshot, account, existingConversationId,
                         existingEpoch, existingKeyReady, existingKey, loadingOlder, requestedAfterCursor,
                         requestedBeforeCursor, existingBeforeCursor, existingAfterCursor,
                         existingHasMore, maximumResolvedMessages, refreshToken,
                         existingLines = std::move(existingLines)]() mutable {
        std::string error;
        if (!messageCryptoReady_.load(std::memory_order_acquire)) {
            const bool identityReady = messageCrypto_.IsReady() || messageCrypto_.Initialize(error);
            if (identityReady && platform_.EnsureMessageDevice(messageCrypto_, error)) {
                messageCryptoReady_.store(true, std::memory_order_release);
            }
        }
        const auto conversations = error.empty() ? platform_.Conversations(error) : std::vector<PlatformConversation>{};
        PlatformConversation conversation;
        if (error.empty()) {
            const std::size_t roomConversationCount = static_cast<std::size_t>(std::count_if(
                conversations.begin(), conversations.end(),
                [](const PlatformConversation& item) { return item.kind == "room" && !item.roomId.empty(); }));
            DiagnosticLog("realtime", "chat-conversations total=" + std::to_string(conversations.size())
                + " room=" + std::to_string(roomConversationCount)
                + " direct_mode=" + std::to_string(!directId.empty()));
            const auto found = std::find_if(conversations.begin(), conversations.end(), [&](const PlatformConversation& item) {
                return directId.empty() ? item.roomId == roomId : (item.id == directId && item.kind == "direct");
            });
            if (found == conversations.end()) {
                DiagnosticLog("realtime", "chat-conversation-match-missing room_selected="
                    + std::to_string(!roomId.empty()) + " channel_selected=" + std::to_string(!channelId.empty()));
                error = "Mesaj konusmasi bulunamadi";
            }
            else conversation = *found;
        }
        std::array<std::uint8_t, 32> key{};
        bool keyReady = false;
        auto devices = error.empty() ? platform_.ConversationDevices(conversation.id, error) : std::vector<MessageDevice>{};
        if (error.empty() && existingKeyReady && existingConversationId == conversation.id && existingEpoch == conversation.currentEpoch) {
            key = existingKey; keyReady = true;
        }
        if (error.empty() && !keyReady) {
            const std::string ownContext = "SonalisKeyEnvelope\n" + conversation.id + "\n" +
                std::to_string(conversation.currentEpoch) + "\n" + messageCrypto_.DeviceId();
            auto envelope = platform_.ConversationKeyEnvelope(conversation.id, conversation.currentEpoch, messageCrypto_.DeviceId(), error);
            if (envelope) keyReady = messageCrypto_.OpenConversationKey(*envelope, ownContext, key, error);
            if (!envelope && error.empty()) {
                keyReady = messageCrypto_.RandomConversationKey(key, error);
                std::vector<KeyEnvelopeUpload> initial;
                if (keyReady) {
                    for (const auto& device : devices) {
                        if (!device.activeRecipient) continue;
                        std::string sealed;
                        const std::string context = "SonalisKeyEnvelope\n" + conversation.id + "\n" +
                            std::to_string(conversation.currentEpoch) + "\n" + device.deviceId;
                        if (!messageCrypto_.SealConversationKey(key, device.encryptionPublicKey, context, sealed, error)) { keyReady = false; break; }
                        initial.push_back({"device", device.deviceId, std::move(sealed)});
                    }
                }
                const auto legalKey = keyReady ? platform_.LegalEscrowPublicKey(error) : std::nullopt;
                if (keyReady && legalKey) {
                    std::string sealed;
                    const std::string context = "SonalisKeyEnvelope\n" + conversation.id + "\n" +
                        std::to_string(conversation.currentEpoch) + "\nLEGAL_ACCESS";
                    if (!messageCrypto_.SealConversationKey(key, *legalKey, context, sealed, error)) keyReady = false;
                    else initial.push_back({"legal_escrow", "LEGAL_ACCESS", std::move(sealed)});
                }
                if (keyReady && !platform_.UploadKeyEnvelopes(conversation.id, conversation.currentEpoch, true, initial, error)) {
                    if (error == "key_epoch_already_initialized") {
                        error.clear(); envelope = platform_.ConversationKeyEnvelope(conversation.id, conversation.currentEpoch, messageCrypto_.DeviceId(), error);
                        keyReady = envelope && messageCrypto_.OpenConversationKey(*envelope, ownContext, key, error);
                    } else keyReady = false;
                }
            }
        }
        if (error.empty() && keyReady) {
            std::vector<KeyEnvelopeUpload> repairs;
            for (const auto& device : devices) {
                if (!device.activeRecipient) continue;
                std::string sealed;
                const std::string context = "SonalisKeyEnvelope\n" + conversation.id + "\n" +
                    std::to_string(conversation.currentEpoch) + "\n" + device.deviceId;
                if (!messageCrypto_.SealConversationKey(key, device.encryptionPublicKey, context, sealed, error)) break;
                repairs.push_back({"device", device.deviceId, std::move(sealed)});
            }
            if (error.empty() && !repairs.empty() && !platform_.UploadKeyEnvelopes(
                conversation.id, conversation.currentEpoch, false, repairs, error)) keyReady = false;
        }
        MessageSyncPage page;
        if (error.empty() && keyReady) page = platform_.SyncMessagePage(
            conversation.id, requestedAfterCursor, requestedBeforeCursor, error, channelId);
        std::unordered_map<std::string, std::string> memberNames;
        if (error.empty()) {
            memberNames.emplace(account.id, account.nickname);
            if (!directId.empty()) {
                for (const auto& friendEntry : friendSnapshot) memberNames.emplace(friendEntry.id, friendEntry.nickname);
            } else {
                const auto members = platform_.RoomMembers(roomId, error);
                for (const auto& member : members) memberNames.emplace(member.id, member.nickname);
            }
        }
        std::unordered_map<std::string, MessageDevice> deviceMap;
        for (const auto& device : devices) deviceMap.emplace(device.deviceId, device);
        std::vector<ChatLine> lines = existingConversationId == conversation.id
            ? std::move(existingLines) : std::vector<ChatLine>{};
        std::uint64_t sequence = 0;
        const std::string associated = "SonalisMessage\n" + conversation.id + "\n" + std::to_string(conversation.currentEpoch)
            + (channelId.empty() ? std::string{} : "\nchannel:" + channelId);
        if (error.empty()) for (const auto& message : page.messages) {
            nlohmann::ordered_json canonical;
            canonical["id"] = message.id; canonical["conversationId"] = message.conversationId; canonical["senderId"] = message.senderId;
            canonical["deviceId"] = message.deviceId; canonical["epoch"] = message.epoch; canonical["clientSequence"] = message.clientSequence;
            canonical["eventType"] = message.eventType; canonical["ciphertext"] = message.ciphertext; canonical["nonce"] = message.nonce;
            canonical["replyTo"] = message.replyTo.empty() ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(message.replyTo);
            const bool v33 = !message.targetMessageId.empty() || message.characterCount != 0 || !message.reaction.empty() || !message.moderationReason.empty()
                || !message.attachmentIds.empty() || message.signatureVersion >= 35;
            if (v33) {
                canonical["targetMessageId"] = message.targetMessageId.empty() ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(message.targetMessageId);
                canonical["characterCount"] = message.characterCount == 0 ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(message.characterCount);
                canonical["reaction"] = message.reaction.empty() ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(message.reaction);
                canonical["moderationReason"] = message.moderationReason.empty() ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(message.moderationReason);
                if (message.signatureVersion >= 35) {
                    canonical["channelId"] = message.channelId.empty() ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(message.channelId);
                }
                if (message.signatureVersion >= 36) canonical["mentions"] = message.mentions;
                if (message.signatureVersion >= 37) canonical["attachmentIds"] = message.attachmentIds;
            }
            const auto signer = deviceMap.find(message.deviceId);
            if (signer == deviceMap.end() || !MessageCrypto::Verify(signer->second.signingPublicKey, canonical.dump(), message.signature)) continue;
            std::string clear;
            if (!messageCrypto_.DecryptMessage(key, associated, {message.ciphertext, message.nonce}, clear)) continue;
            try {
                const auto payload = nlohmann::json::parse(clear);
                const auto target = std::find_if(lines.begin(), lines.end(), [&](const ChatLine& line) { return line.id == message.targetMessageId; });
                if (message.eventType == "message") {
                    const auto named = memberNames.find(message.senderId);
                    const auto duplicate = std::find_if(lines.begin(), lines.end(), [&](const ChatLine& line) {
                        return line.id == message.id;
                    });
                    if (duplicate == lines.end()) {
                        lines.push_back({message.id, message.senderId,
                            named == memberNames.end() ? message.senderId : named->second,
                            payload.value("text", ""), message.createdAt, message.replyTo, false, {},
                            message.attachmentIds});
                    } else {
                        duplicate->createdAt = message.createdAt;
                        duplicate->delivery = ChatDeliveryState::Sent;
                        duplicate->attachmentIds = message.attachmentIds;
                    }
                } else if (message.eventType == "edit" && target != lines.end()) {
                    target->text = payload.value("text", target->text); target->edited = true;
                } else if (message.eventType == "delete" && target != lines.end()) {
                    lines.erase(target);
                } else if (message.eventType == "reaction" && target != lines.end()) {
                    constexpr std::array<const char*, 6> names{"like","love","laugh","wow","sad","angry"};
                    const std::string reaction = payload.value("reaction", message.reaction);
                    const auto found = std::find(names.begin(), names.end(), reaction);
                    if (found != names.end()) ++target->reactions[static_cast<std::size_t>(std::distance(names.begin(), found))];
                }
            } catch (...) { continue; }
            sequence = std::max(sequence, message.clientSequence);
        }
        std::sort(lines.begin(), lines.end(), [](const ChatLine& left, const ChatLine& right) {
            if (left.createdAt != right.createdAt) return left.createdAt < right.createdAt;
            return left.id < right.id;
        });
        while (lines.size() > maximumResolvedMessages) lines.erase(lines.begin());
        std::size_t arenaBytes = 0;
        std::size_t keepFrom = 0;
        for (std::size_t index = lines.size(); index > 0; --index) {
            const auto& line = lines[index - 1];
            const std::size_t bytes = line.id.size() + line.senderId.size() + line.sender.size() + line.text.size() + line.createdAt.size();
            if (arenaBytes + bytes > 1024 * 1024) { keepFrom = index; break; }
            arenaBytes += bytes;
        }
        if (keepFrom != 0) lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(keepFrom));
        return BackgroundWorker::Completion([this, conversation, channelId, key, keyReady, sequence, account,
                                              loadingOlder, existingBeforeCursor, existingAfterCursor,
                                              existingHasMore, refreshToken, page = std::move(page), lines = std::move(lines), error]() mutable {
            chatRefreshPending_ = false;
            const bool refreshAgain = chatRefreshGeneration_.Complete(refreshToken);
            if (!error.empty() || !keyReady) {
                const std::string safeError = error.empty() ? "message_key_envelope_not_found" : error;
                DiagnosticLog("messaging", "refresh_failed code=" + safeError.substr(0, 96));
                uiMessage_ = UserFacingError(safeError);
                if (refreshAgain) RefreshChat();
                return;
            }
            chatConversationId_ = conversation.id; chatEpoch_ = conversation.currentEpoch; chatKey_ = key; chatKeyReady_ = true;
            chatSequence_ = std::max(chatSequence_, sequence);
            chatBeforeCursor_ = loadingOlder
                ? page.beforeCursor
                : (existingBeforeCursor.empty() ? page.beforeCursor : existingBeforeCursor);
            chatAfterCursor_ = page.afterCursor.empty() ? existingAfterCursor : page.afterCursor;
            chatHasMore_ = loadingOlder || existingBeforeCursor.empty() ? page.hasMore : existingHasMore;
            for (auto& oldLine : chatLines_) {
                if (!oldLine.text.empty()) crypto_wipe(oldLine.text.data(), oldLine.text.size());
            }
            chatLines_ = std::move(lines); uiMessage_.clear();
            DiagnosticLog("messaging", "refresh_ready messages=" + std::to_string(chatLines_.size())
                + " epoch=" + std::to_string(chatEpoch_));
            if (pendingNotificationConversation_ == conversation.id && !chatLines_.empty()) {
                const ChatLine& newest = chatLines_.back();
                const bool mentioned = newest.text.find("@" + account.username) != std::string::npos;
                if (pendingNotificationDirect_ || pendingNotificationAll_ || mentioned) {
                    std::string preview = newest.sender + ": " + newest.text;
                    if (preview.size() > 180) preview.resize(180);
                    ShowWindowsNotification(pendingNotificationDirect_ ? "Yeni ozel mesaj"
                        : (mentioned ? "Odada senden bahsedildi" : "Yeni kanal mesaji"), preview);
                }
                pendingNotificationConversation_.clear();
                pendingNotificationDirect_ = false;
                pendingNotificationAll_ = false;
            }
            if (!chatLines_.empty()) {
                const std::string readConversation = chatConversationId_;
                const auto readable = std::find_if(chatLines_.rbegin(), chatLines_.rend(),
                    [](const ChatLine& line) { return line.delivery == ChatDeliveryState::Sent; });
                if (readable == chatLines_.rend()) {
                    if (refreshAgain) RefreshChat();
                    return;
                }
                const std::string readMessage = readable->id;
                if (!channelId.empty() && roomOverview_) {
                    const auto channel = std::find_if(roomOverview_->channels.begin(), roomOverview_->channels.end(),
                        [&channelId](const PlatformChannel& value) { return value.id == channelId; });
                    if (channel != roomOverview_->channels.end()) {
                        channel->unreadCount = 0U;
                        channel->mentionCount = 0U;
                    }
                }
                worker_.Submit([this, readConversation, readMessage, channelId] {
                    std::string ignored;
                    if (channelId.empty()) platform_.MarkConversationRead(readConversation, readMessage, ignored);
                    else platform_.MarkChannelRead(channelId, readMessage, ignored);
                    return BackgroundWorker::Completion{};
                });
            }
            if (refreshAgain) RefreshChat();
        });
    })) {
        chatRefreshPending_ = false;
        (void)chatRefreshGeneration_.Complete(refreshToken);
        uiMessage_ = "Arka plan is kuyrugu dolu";
    }
}

void AppUi::RefreshPins() {
    if (pinsRefreshPending_ || chatChannelId_.empty()) return;
    const std::string channelId = chatChannelId_;
    pinsRefreshPending_ = true;
    if (!worker_.Submit([this, channelId] {
        std::string error;
        auto ids = platform_.ChannelPinIds(channelId, error);
        return BackgroundWorker::Completion([this, channelId, ids = std::move(ids), error]() mutable {
            pinsRefreshPending_ = false;
            if (!error.empty()) {
                uiMessage_ = error;
                return;
            }
            if (channelId != chatChannelId_) return;
            pinnedMessageIds_ = std::move(ids);
            showPinnedMessages_ = true;
        });
    })) {
        pinsRefreshPending_ = false;
        uiMessage_ = "Sabitlenen mesajlar kuyruga alinamadi";
    }
}

void AppUi::OpenDirectChat(const PlatformFriend& friendEntry) {
    if (platformActionPending_) return;
    const std::string userId = friendEntry.id;
    const std::string label = friendEntry.nickname + " (@" + friendEntry.username + ")";
    platformActionPending_ = true;
    if (!worker_.Submit([this, userId, label] {
        std::string error;
        auto conversationId = platform_.OpenDirectConversation(userId, error);
        return BackgroundWorker::Completion([this, conversationId = std::move(conversationId), label, error] {
            platformActionPending_ = false;
            if (!conversationId || conversationId->empty()) {
                uiMessage_ = error.empty() ? "Ozel konusma acilamadi" : error;
                return;
            }
            directConversationId_ = *conversationId;
            directConversationLabel_ = label;
            chatChannelId_.clear();
            const auto draft = chatDrafts_.find(directConversationId_);
            CopyToBuffer(draft == chatDrafts_.end() ? std::string{} : draft->second, chatBuffer_);
            chatConversationId_.clear(); WipeChatLines();
            crypto_wipe(chatKey_.data(), chatKey_.size()); chatKeyReady_ = false;
            activePage_ = ClientPage::Messages;
            RefreshChat();
        });
    })) platformActionPending_ = false;
}

void AppUi::StartRealtime() {
    realtime_.Start([this](std::string& error) { return platform_.RequestRealtimeGrant(error); }, redrawEvent_);
    lastFallbackSyncMs_ = SteadyNowMs();
}

void AppUi::ResetSessionView(std::string message) {
    Disconnect();
    realtime_.Stop();
    platformRooms_.clear();
    roomOverview_.reset();
    roomMembers_.clear();
    friends_.clear();
    friendSearchResults_.clear();
    notifications_.clear();
    accountSessions_.clear();
    accountDevices_.clear();
    accountSecurityLoaded_ = false;
    conversations_.clear();
    mediaSafetyConfig_.reset();
    restrictions_.clear();
    appealRestrictionId_.clear();
    restrictionAppealBuffer_.fill('\0');
    reportAttachmentId_.clear();
    reportMessageId_.clear();
    reportUserId_.clear();
    lastSubmittedMediaReportId_.clear();
    mediaReportNoteBuffer_.fill('\0');
    showMediaReportReceipt_ = false;
    WipeChatLines();
    chatConversationId_.clear();
    directConversationId_.clear();
    directConversationLabel_.clear();
    presenceByUser_.clear();
    customStatusByUser_.clear();
    typingUntilMs_.clear();
    customStatusBuffer_.fill('\0');
    for (auto& [draftId, draft] : chatDrafts_) {
        (void)draftId;
        if (!draft.empty()) crypto_wipe(draft.data(), draft.size());
    }
    chatDrafts_.clear();
    chatBuffer_.fill('\0');
    chatSearchBuffer_.fill('\0');
    crypto_wipe(chatKey_.data(), chatKey_.size());
    chatKeyReady_ = false;
    chatRefreshGeneration_.Reset();
    uiMessage_ = std::move(message);
}

void AppUi::HandleRealtimeEvents() {
    for (const auto& raw : realtime_.DrainEvents()) {
        try {
            const auto frame = nlohmann::json::parse(raw);
            const std::string type = frame.value("type", "");
            if (type == "realtime.error") {
                const std::string error = frame.value("error", "unknown");
                DiagnosticLog("realtime", "event-error=" + error);
            } else if (type == "ready") {
                realtime_.SetPresence(ownPresence_, customStatusBuffer_.data());
            } else if (type == "presence.snapshot") {
                if (frame.contains("users") && frame["users"].is_array()) {
                    for (const auto& value : frame["users"]) {
                        const std::string userId = value.value("userId", "");
                        if (userId.empty()) continue;
                        presenceByUser_[userId] = value.value("status", "offline");
                        customStatusByUser_[userId] = value.value("customText", "");
                    }
                }
            } else if (type == "presence.updated") {
                const std::string userId = frame.value("userId", "");
                if (!userId.empty()) {
                    presenceByUser_[userId] = frame.value("status", "offline");
                    customStatusByUser_[userId] = frame.value("customText", "");
                }
            } else if (type == "message.event" || type == "conversation.updated") {
                const std::string conversationId = frame.value("conversationId", "");
                const std::string eventChannelId = frame.value("channelId", "");
                if (type == "message.event" && frame.value("eventType", "") == "message"
                    && frame.value("senderId", "") != platform_.User().id) {
                    const bool direct = frame.value("conversationKind", "") == "direct";
                    if (direct) ShowWindowsNotification("Yeni ozel mesaj", "Sonalis'te yeni bir ozel mesajiniz var.");
                    else {
                        bool mentioned = false;
                        if (frame.contains("mentions") && frame["mentions"].is_array()) {
                            for (const auto& userId : frame["mentions"]) {
                                if (userId.is_string() && userId.get<std::string>() == platform_.User().id) {
                                    mentioned = true;
                                    break;
                                }
                            }
                        }
                        if (roomOverview_ && !eventChannelId.empty() && eventChannelId != chatChannelId_) {
                            const auto channel = std::find_if(roomOverview_->channels.begin(), roomOverview_->channels.end(),
                                [&eventChannelId](const PlatformChannel& value) { return value.id == eventChannelId; });
                            if (channel != roomOverview_->channels.end()) {
                                channel->unreadCount = std::min<std::uint32_t>(999U, channel->unreadCount + 1U);
                                if (mentioned) channel->mentionCount = std::min<std::uint32_t>(999U, channel->mentionCount + 1U);
                            }
                        }
                        std::string notificationMode = "mentions";
                        if (roomOverview_) {
                            const auto channel = std::find_if(roomOverview_->channels.begin(), roomOverview_->channels.end(),
                                [&eventChannelId](const PlatformChannel& value) { return value.id == eventChannelId; });
                            if (channel != roomOverview_->channels.end()) notificationMode = channel->notificationMode;
                        }
                        const bool notifyAll = notificationMode == "all";
                        const bool shouldNotify = notificationMode != "muted" && (notifyAll || mentioned);
                        if (shouldNotify && eventChannelId == chatChannelId_) {
                            pendingNotificationConversation_ = conversationId;
                            pendingNotificationDirect_ = false;
                            pendingNotificationAll_ = notifyAll;
                        } else if (shouldNotify) {
                            ShowWindowsNotification(mentioned ? "Odada senden bahsedildi" : "Yeni kanal mesaji",
                                                    "Sonalis'te yeni bir kanal mesajiniz var.");
                        }
                    }
                }
                if (!chatConversationId_.empty() && conversationId == chatConversationId_
                    && (chatChannelId_.empty() || eventChannelId.empty() || eventChannelId == chatChannelId_)) RefreshChat();
                if (type == "conversation.updated") RefreshSocial();
            } else if (type == "conversation.epoch") {
                if (frame.value("conversationId", "") == chatConversationId_) {
                    chatKeyReady_ = false;
                    crypto_wipe(chatKey_.data(), chatKey_.size());
                    RefreshChat();
                }
            } else if (type == "notification") {
                RefreshSocial();
                ShowWindowsNotification("Sonalis bildirimi", "Hesabinizda yeni ve onemli bir olay var.");
            } else if (type == "room.channels_updated" || type == "channel.pins_updated") {
                if (frame.value("roomId", "") == settings_.lastRoomId) RefreshRoomOverview();
            } else if (type == "voice.wake" || type == "voice.channel_waking" || type == "voice.channel_ready") {
                if (voiceSleeping_ && frame.value("roomId", "") == settings_.lastRoomId
                    && (activeVoiceChannelId_.empty() || frame.value("channelId", activeVoiceChannelId_) == activeVoiceChannelId_)
                    && network_.State() != ConnectionState::Connected && !connectPending_) {
                    Connect();
                }
            } else if (type == "voice.channel_sleeping" || type == "voice.channel_closed") {
                if (frame.value("roomId", "") == settings_.lastRoomId
                    && (activeVoiceChannelId_.empty() || frame.value("channelId", activeVoiceChannelId_) == activeVoiceChannelId_)) {
                    voiceSleeping_ = true;
                    if (type == "voice.channel_closed" && audio_.IsRunning()) audio_.Stop();
                }
            } else if (type == "typing") {
                const std::string userId = frame.value("userId", "");
                if (!userId.empty()) {
                    if (frame.value("active", false)) typingUntilMs_[userId] = SteadyNowMs() + 5'000;
                    else typingUntilMs_.erase(userId);
                }
            }
        } catch (...) { continue; }
    }
    const std::uint64_t now = SteadyNowMs();
    if (audio_.IsRunning() && now - lastVoiceDiagnosticsLogMs_ >= 30'000) {
        lastVoiceDiagnosticsLogMs_ = now;
        const AudioDiagnosticsSnapshot audio = audio_.Diagnostics();
        const NetworkDiagnosticsSnapshot network = network_.Diagnostics();
        char summary[384]{};
        std::snprintf(summary, sizeof(summary),
                      "captured=%llu encoded=%llu encode_errors=%llu sent=%llu rejected=%llu received=%llu decoded=%llu rendered=%llu decrypt_rejected=%llu",
                      static_cast<unsigned long long>(audio.capturedPcmFrames),
                      static_cast<unsigned long long>(audio.opusEncodeSuccess),
                      static_cast<unsigned long long>(audio.opusEncodeErrors),
                      static_cast<unsigned long long>(network.udpAudioSent),
                      static_cast<unsigned long long>(network.udpAudioRejected),
                      static_cast<unsigned long long>(network.udpAudioReceived),
                      static_cast<unsigned long long>(audio.decodedOpusFrames),
                      static_cast<unsigned long long>(audio.wasapiRenderFrames),
                      static_cast<unsigned long long>(network.udpDecryptRejected));
        DiagnosticLog("voice.pipeline", summary);
    }
    if (notificationIconVisible_ && now >= notificationIconExpiresMs_) DismissWindowsNotification();
    std::erase_if(typingUntilMs_, [now](const auto& item) { return item.second <= now; });
    if (!realtime_.IsConnected() && platform_.IsAuthenticated()) {
        const std::uint64_t interval = chatConversationId_.empty() ? 60'000 : 15'000;
        if (now - lastFallbackSyncMs_ >= interval) {
            lastFallbackSyncMs_ = now;
            if (!chatConversationId_.empty()) RefreshChat();
            else { RefreshPlatformRooms(); RefreshSocial(); }
        }
    }
}

void AppUi::SendChatMessage() {
    const std::string text(chatBuffer_.data());
    if (text.empty() && pendingChatAttachments_.empty()) return;
    const std::size_t characters = UnicodeCodepoints(text);
    if (characters > 2'000) { uiMessage_ = "Mesaj en fazla 2000 Unicode karakter olmali"; return; }
    const std::string eventType = editTargetId_.empty() ? "message" : "edit";
    if (eventType == "edit" && !pendingChatAttachments_.empty()) {
        uiMessage_ = "Duzenleme isleminde yeni dosya eklenemez";
        return;
    }
    const std::string activeChannelId = directConversationId_.empty() ? chatChannelId_ : std::string{};
    if (std::ranges::any_of(pendingChatAttachments_,
        [this, &activeChannelId](const PendingChatAttachment& attachment) {
            return attachment.conversationId != chatConversationId_
                || attachment.channelId != activeChannelId;
        })) {
        uiMessage_ = "Dosya baska bir konusmaya ait; dosyayi kaldirip yeniden ekleyin";
        return;
    }
    const std::string target = editTargetId_.empty() ? replyTargetId_ : editTargetId_;
    std::vector<std::string> attachmentIds;
    if (eventType == "message") {
        attachmentIds.reserve(pendingChatAttachments_.size());
        for (const auto& attachment : pendingChatAttachments_) attachmentIds.push_back(attachment.id);
    }
    // Taslagi sunucu mesaji kabul edene kadar koru. Onceki davranis, ag veya
    // anahtar hatasinda kullanicinin yazdigi mesaji geri getirilemez bicimde
    // siliyor ve mesajlasma calismiyormus gibi gorunuyordu.
    SendChatEvent(eventType, target, text, {}, {}, std::move(attachmentIds));
}

void AppUi::SendChatEvent(std::string eventType, std::string targetMessageId, std::string text,
                          std::string reaction, std::string moderationReason,
                          std::vector<std::string> attachmentIds) {
    if (chatSendPending_) return;
    if (!chatKeyReady_ || chatConversationId_.empty()) { uiMessage_ = "Mesaj anahtari hazirlaniyor"; RefreshChat(); return; }
    const std::string conversationId = chatConversationId_;
    const int epoch = chatEpoch_;
    const auto key = chatKey_;
    const PlatformUser account = platform_.User();
    const std::string channelId = directConversationId_.empty() ? chatChannelId_ : std::string{};
    std::vector<std::string> mentions;
    if (!channelId.empty() && (eventType == "message" || eventType == "edit")) {
        for (const auto& member : roomMembers_) {
            if (ContainsMention(text, member.username)) mentions.push_back(member.id);
        }
    }
    const std::string localMessageId = MessageCrypto::NewId();
    if (eventType == "message") {
        for (auto& line : chatLines_) {
            if (line.delivery == ChatDeliveryState::Failed && line.senderId == account.id
                && line.text == text && !line.text.empty()) {
                crypto_wipe(line.text.data(), line.text.size());
            }
        }
        std::erase_if(chatLines_, [&account, &text](const ChatLine& line) {
            return line.delivery == ChatDeliveryState::Failed && line.senderId == account.id
                && (line.text.empty() || line.text == text);
        });
        ChatLine optimistic;
        optimistic.id = localMessageId;
        optimistic.senderId = account.id;
        optimistic.sender = account.nickname.empty() ? account.username : account.nickname;
        optimistic.text = text;
        optimistic.createdAt = CurrentUtcIsoTimestamp();
        optimistic.replyTo = targetMessageId;
        optimistic.attachmentIds = attachmentIds;
        optimistic.delivery = ChatDeliveryState::Pending;
        chatLines_.push_back(std::move(optimistic));
    }
    chatSendPending_ = true;
    if (!worker_.Submit([this, eventType = std::move(eventType), targetMessageId = std::move(targetMessageId),
                         text = std::move(text), reaction = std::move(reaction), moderationReason = std::move(moderationReason),
                         mentions = std::move(mentions), attachmentIds = std::move(attachmentIds),
                         conversationId, channelId, epoch, key, account, localMessageId] {
        std::string error;
        const auto remoteSequence = platform_.DeviceMessageSequence(conversationId, messageCrypto_.DeviceId(), error);
        EncryptedPlatformMessage message;
        if (remoteSequence) {
            const std::uint64_t sequence = *remoteSequence + 1;
            const std::string associated = "SonalisMessage\n" + conversationId + "\n" + std::to_string(epoch)
                + (channelId.empty() ? std::string{} : "\nchannel:" + channelId);
            nlohmann::json clear;
            if (eventType == "reaction") clear["reaction"] = reaction;
            else if (eventType == "delete") clear["deleted"] = true;
            else clear["text"] = text;
            EncryptedMessagePayload encrypted;
            if (messageCrypto_.EncryptMessage(key, associated, clear.dump(), encrypted, error)) {
                message.id = localMessageId; message.conversationId = conversationId;
                message.senderId = account.id; message.deviceId = messageCrypto_.DeviceId(); message.epoch = epoch;
                message.clientSequence = sequence; message.eventType = eventType; message.ciphertext = encrypted.ciphertext;
                message.nonce = encrypted.nonce; message.targetMessageId = targetMessageId; message.reaction = reaction;
                message.moderationReason = moderationReason;
                message.channelId = channelId;
                message.mentions = mentions;
                message.attachmentIds = attachmentIds;
                message.signatureVersion = !message.attachmentIds.empty() ? 37
                    : !message.mentions.empty() ? 36 : (channelId.empty() ? 0 : 35);
                if (eventType == "message" || eventType == "edit") message.characterCount = static_cast<std::uint32_t>(UnicodeCodepoints(text));
                if (eventType == "message" && !targetMessageId.empty()) message.replyTo = targetMessageId;
                nlohmann::ordered_json canonical;
                canonical["id"] = message.id; canonical["conversationId"] = message.conversationId; canonical["senderId"] = message.senderId;
                canonical["deviceId"] = message.deviceId; canonical["epoch"] = message.epoch; canonical["clientSequence"] = message.clientSequence;
                canonical["eventType"] = message.eventType; canonical["ciphertext"] = message.ciphertext; canonical["nonce"] = message.nonce;
                canonical["replyTo"] = message.replyTo.empty() ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(message.replyTo);
                canonical["targetMessageId"] = message.targetMessageId.empty() ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(message.targetMessageId);
                canonical["characterCount"] = message.characterCount == 0 ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(message.characterCount);
                canonical["reaction"] = message.reaction.empty() ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(message.reaction);
                canonical["moderationReason"] = message.moderationReason.empty() ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(message.moderationReason);
                if (!message.channelId.empty()) canonical["channelId"] = message.channelId;
                if (message.signatureVersion >= 36) canonical["mentions"] = message.mentions;
                if (message.signatureVersion >= 37) canonical["attachmentIds"] = message.attachmentIds;
                message.signature = messageCrypto_.Sign(canonical.dump());
                if (!platform_.SendEncryptedMessage(message, error)) {
                    if (IsTransientMessageSendError(error)) {
                        DiagnosticLog("messaging", "send_retry_same_id");
                        error.clear();
                        if (!platform_.SendEncryptedMessage(message, error)) message.id.clear();
                    } else {
                        message.id.clear();
                    }
                }
            }
        }
        return BackgroundWorker::Completion([this, error, sent = !message.id.empty(),
                                             composerText = std::move(text),
                                             composerEvent = std::move(eventType),
                                             attachmentIds = std::move(attachmentIds), localMessageId] {
            chatSendPending_ = false;
            const auto localLine = std::find_if(chatLines_.begin(), chatLines_.end(),
                [&localMessageId](const ChatLine& line) { return line.id == localMessageId; });
            if (localLine != chatLines_.end()) {
                localLine->delivery = sent ? ChatDeliveryState::Sent : ChatDeliveryState::Failed;
            }
            if (!sent) {
                DiagnosticLog("messaging", "send_failed code=" + (error.empty() ? std::string("unknown") : error.substr(0, 96)));
                uiMessage_ = UserFacingError(error.empty() ? "Mesaj gönderilemedi." : error);
                if (error == "conversation_epoch_changed") {
                    crypto_wipe(chatKey_.data(), chatKey_.size());
                    chatKeyReady_ = false;
                    RefreshChat();
                }
            }
            else {
                DiagnosticLog("messaging", "send_accepted");
                if ((composerEvent == "message" || composerEvent == "edit")
                    && std::string_view(chatBuffer_.data()) == composerText) {
                    chatBuffer_.fill('\0');
                    const std::string draftKey = directConversationId_.empty() ? chatChannelId_ : directConversationId_;
                    if (!draftKey.empty()) chatDrafts_.erase(draftKey);
                    replyTargetId_.clear();
                    editTargetId_.clear();
                }
                if (!attachmentIds.empty()) {
                    std::erase_if(pendingChatAttachments_, [&attachmentIds](const PendingChatAttachment& pending) {
                        return std::ranges::find(attachmentIds, pending.id) != attachmentIds.end();
                    });
                }
                uiMessage_.clear();
                RefreshChat();
            }
        });
    })) {
        chatSendPending_ = false;
        const auto localLine = std::find_if(chatLines_.begin(), chatLines_.end(),
            [&localMessageId](const ChatLine& line) { return line.id == localMessageId; });
        if (localLine != chatLines_.end()) localLine->delivery = ChatDeliveryState::Failed;
        uiMessage_ = "Arka plan is kuyrugu dolu";
    }
}

void AppUi::SelectChatAttachment() {
    if (mediaUploadPending_ || chatSendPending_) return;
    if (!experiencePolicy_.mediaAttachmentsEnabled) {
        uiMessage_ = "Medya ekleri platform yoneticisi tarafindan kapatildi";
        return;
    }
    if (!chatKeyReady_ || chatConversationId_.empty()) {
        uiMessage_ = "Mesaj sifreleme anahtari henuz hazir degil";
        return;
    }
    if (pendingChatAttachments_.size() >= 10U) {
        uiMessage_ = "Bir mesajda en fazla 10 dosya olabilir";
        return;
    }
    const bool directMessage = !directConversationId_.empty();
    bool localScanRequired = directMessage;
    const PlatformChannel* channel = nullptr;
    if (!directMessage && roomOverview_) {
        const auto found = std::ranges::find_if(roomOverview_->channels, [this](const PlatformChannel& value) {
            return value.id == chatChannelId_;
        });
        if (found != roomOverview_->channels.end()) channel = &*found;
    }
    if (!directMessage && channel == nullptr) {
        uiMessage_ = "Metin kanali secilmedi";
        return;
    }
    if (channel != nullptr && channel->mediaPostingPolicy == "disabled") {
        uiMessage_ = "Bu kanalda medya paylasimi kapali";
        return;
    }
    if (channel != nullptr) localScanRequired = channel->localMediaScanRequired;

    std::array<wchar_t, 32'768> selectedPath{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = windowHandle_;
    dialog.lpstrFile = selectedPath.data();
    dialog.nMaxFile = static_cast<DWORD>(selectedPath.size());
    dialog.lpstrFilter = L"Desteklenen dosyalar\0*.png;*.jpg;*.jpeg;*.webp;*.gif;*.pdf;*.txt;*.zip\0Tum dosyalar\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&dialog)) return;
    const std::filesystem::path source(selectedPath.data());
    std::error_code fileError;
    const std::uint64_t bytes = std::filesystem::file_size(source, fileError);
    if (fileError || bytes == 0 || bytes > 24U * 1024U * 1024U) {
        uiMessage_ = "Dosya 1 bayt ile 24 MB arasinda olmali";
        return;
    }
    const std::string attachmentId = MessageCrypto::NewId();
    const std::string conversationId = chatConversationId_;
    const std::string channelId = directMessage ? std::string{} : chatChannelId_;
    const auto key = chatKey_;
    const std::string displayName = WideToUtf8(source.filename().wstring());
    const bool preferGuardianGpu = settings_.resourceProfile != ResourceProfile::Economy;
    const std::filesystem::path encryptedPath =
        std::filesystem::temp_directory_path() / "Sonalis" / "media-upload" / (Utf8ToWide(attachmentId) + L".enc");
    mediaUploadPending_ = true;
    if (!worker_.Submit([this, attachmentId, conversationId, channelId, key,
                         sourcePath = source.wstring(), encryptedPath = encryptedPath.wstring(), displayName,
                         localScanRequired, preferGuardianGpu] {
        std::string error;
        GuardianLocalScanResult scanResult;
        bool ok = true;
        if (localScanRequired) {
            ok = guardianLocalScanner_.Scan(platform_, sourcePath, attachmentId,
                                             preferGuardianGpu, scanResult, error);
            if (ok && scanResult.decision != GuardianLocalDecision::Safe) {
                error = scanResult.decision == GuardianLocalDecision::Review
                    ? "Guardian bu gorseli guvenle onaylayamadi"
                    : "Guardian gorsel paylasimini guvenlik nedeniyle engelledi";
                ok = false;
            }
        }
        PreparedMediaFile prepared;
        if (ok) ok = MediaCrypto::PrepareFile(messageCrypto_, key, attachmentId, sourcePath,
                                               encryptedPath, prepared, error);
        std::optional<MediaUploadGrant> grant;
        if (ok) {
            MediaAttachmentDraft draft;
            draft.id = attachmentId;
            draft.conversationId = conversationId;
            draft.channelId = channelId;
            draft.sizeBytes = prepared.encryptedSize;
            draft.mimeHint = prepared.mimeHint;
            draft.ciphertextSha256 = prepared.ciphertextSha256;
            draft.metadataCiphertext = prepared.metadataCiphertext;
            draft.metadataNonce = prepared.metadataNonce;
            draft.localScanModel = localScanRequired
                ? scanResult.modelId + "/" + scanResult.modelVersion : "not-required/v1";
            draft.localScanVerdict = localScanRequired
                ? GuardianDecisionName(scanResult.decision) : "review";
            draft.localScanDigest = localScanRequired
                ? scanResult.digest : prepared.ciphertextSha256;
            grant = platform_.InitiateMediaAttachment(draft, error);
            ok = grant.has_value();
        }
        if (ok) ok = platform_.UploadMediaAttachment(*grant, encryptedPath, error);
        if (ok) ok = platform_.CompleteMediaAttachment(attachmentId, error);
        std::error_code ignored;
        std::filesystem::remove(std::filesystem::path(encryptedPath), ignored);
        return BackgroundWorker::Completion([this, ok, error, attachmentId, displayName,
                                             conversationId, channelId,
                                             encryptedBytes = prepared.encryptedSize] {
            mediaUploadPending_ = false;
            if (!ok) uiMessage_ = error.empty() ? "Medya yuklenemedi" : error;
            else {
                pendingChatAttachments_.push_back(
                    {attachmentId, displayName, conversationId, channelId, encryptedBytes});
                uiMessage_ = "Dosya sifrelendi ve mesaja eklendi";
            }
        });
    })) {
        mediaUploadPending_ = false;
        uiMessage_ = "Medya islemi icin arka plan kuyrugu dolu";
    }
}

void AppUi::DownloadChatAttachment(std::string attachmentId) {
    if (mediaDownloadPending_ || !chatKeyReady_) return;
    const auto key = chatKey_;
    mediaDownloadPending_ = true;
    if (!worker_.Submit([this, attachmentId = std::move(attachmentId), key] {
        std::string error;
        const auto grant = platform_.MediaAttachmentDownload(attachmentId, error);
        const std::filesystem::path encryptedPath =
            std::filesystem::temp_directory_path() / "Sonalis" / "media-download" / (Utf8ToWide(attachmentId) + L".enc");
        bool ok = grant.has_value();
        if (ok) ok = platform_.DownloadMediaAttachment(*grant, encryptedPath.wstring(), error);
        MediaFileMetadata metadata;
        if (ok) {
            ok = MediaCrypto::DecryptMetadata(messageCrypto_, key, attachmentId,
                grant->metadataCiphertext, grant->metadataNonce, metadata, error);
        }
        std::filesystem::path outputPath;
        if (ok) {
            std::array<wchar_t, 32'768> profile{};
            const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", profile.data(), static_cast<DWORD>(profile.size()));
            const std::filesystem::path root = length > 0 && length < profile.size()
                ? std::filesystem::path(profile.data()) / "Downloads"
                : std::filesystem::temp_directory_path();
            const std::wstring prefix = L"Sonalis-" + Utf8ToWide(attachmentId.substr(0, 8)) + L"-";
            outputPath = root / (prefix + Utf8ToWide(metadata.name));
            ok = MediaCrypto::DecryptPreparedFile(messageCrypto_, key, attachmentId, encryptedPath.wstring(),
                                                   outputPath.wstring(), grant->ciphertextSha256, error);
        }
        std::error_code ignored;
        std::filesystem::remove(encryptedPath, ignored);
        return BackgroundWorker::Completion([this, ok, error, path = outputPath.wstring()] {
            mediaDownloadPending_ = false;
            uiMessage_ = ok ? "Dosya indirildi: " + WideToUtf8(path)
                            : (error.empty() ? "Dosya indirilemedi" : error);
        });
    })) {
        mediaDownloadPending_ = false;
        uiMessage_ = "Medya indirme kuyrugu dolu";
    }
}

void AppUi::SubmitMediaReport() {
    if (mediaReportPending_ || reportAttachmentId_.empty() || reportMessageId_.empty()
        || reportUserId_.empty() || !chatKeyReady_) return;
    if (!mediaSafetyConfig_ || !mediaSafetyConfig_->privateMediaReportingAvailable
        || mediaSafetyConfig_->moderationPublicKey.empty()) {
        uiMessage_ = "Guardian özel medya raporlama şu anda kullanılamıyor.";
        return;
    }
    static constexpr std::array<const char*, 9> reasonCodes{
        "adult_content", "unsolicited_sensitive_media", "minor_safety", "graphic_violence",
        "hate_symbol", "harassment", "impersonation", "spam", "other",
    };
    const std::string attachmentId = reportAttachmentId_;
    const std::string messageId = reportMessageId_;
    const std::string reportedUserId = reportUserId_;
    const std::string note(mediaReportNoteBuffer_.data());
    const std::string reason = reasonCodes[static_cast<std::size_t>(
        std::clamp(mediaReportReasonIndex_, 0, static_cast<int>(reasonCodes.size() - 1U)))];
    const std::string moderationPublicKey = mediaSafetyConfig_->moderationPublicKey;
    const std::size_t maximumBytes = mediaSafetyConfig_->maximumImageBytes;
    const auto key = chatKey_;
    mediaReportPending_ = true;
    if (!worker_.Submit([this, attachmentId, messageId, reportedUserId, note, reason,
                         moderationPublicKey, maximumBytes, key] {
        std::string error;
        const auto grant = platform_.MediaAttachmentDownload(attachmentId, error);
        const std::filesystem::path reportRoot = std::filesystem::temp_directory_path()
            / "Sonalis" / "guardian-report";
        const std::filesystem::path encryptedPath = reportRoot / (Utf8ToWide(attachmentId) + L".enc");
        const std::filesystem::path plaintextPath = reportRoot / (Utf8ToWide(attachmentId) + L".bin");
        bool ok = grant.has_value();
        if (ok) ok = platform_.DownloadMediaAttachment(*grant, encryptedPath.wstring(), error);
        MediaFileMetadata metadata;
        if (ok) ok = MediaCrypto::DecryptMetadata(messageCrypto_, key, attachmentId,
            grant->metadataCiphertext, grant->metadataNonce, metadata, error);
        if (ok && !metadata.mime.starts_with("image/")) {
            error = "guardian_evidence_image_required";
            ok = false;
        }
        if (ok) ok = MediaCrypto::DecryptPreparedFile(messageCrypto_, key, attachmentId,
            encryptedPath.wstring(), plaintextPath.wstring(), grant->ciphertextSha256, error);

        std::vector<std::uint8_t> plaintext;
        if (ok) {
            std::ifstream input(plaintextPath, std::ios::binary | std::ios::ate);
            std::streamoff length = -1;
            if (input) length = static_cast<std::streamoff>(input.tellg());
            if (length <= 0 || static_cast<std::uint64_t>(length) > maximumBytes) {
                error = "guardian_evidence_size_invalid";
                ok = false;
            } else {
                plaintext.resize(static_cast<std::size_t>(length));
                input.seekg(0, std::ios::beg);
                ok = static_cast<bool>(input.read(reinterpret_cast<char*>(plaintext.data()),
                                                  static_cast<std::streamsize>(length)));
                if (!ok) error = "guardian_evidence_read_failed";
            }
        }

        std::vector<std::uint8_t> evidence;
        if (ok) ok = messageCrypto_.SealModerationEvidence(
            plaintext, moderationPublicKey, evidence, error);
        if (!plaintext.empty()) crypto_wipe(plaintext.data(), plaintext.size());
        const std::string digest = ok ? MessageCrypto::Sha256Hex(evidence) : std::string{};
        const std::string idempotencyKey = "native-report-" + MessageCrypto::NewId();
        auto receipt = ok ? platform_.CreateMediaReport(reportedUserId, {}, "message_attachment",
            messageId, reason, note, digest, idempotencyKey, error) : std::nullopt;
        ok = ok && receipt.has_value();
        if (ok && receipt->evidenceUploadRequired) {
            if (receipt->moderationPublicKey.empty() || receipt->moderationPublicKey != moderationPublicKey) {
                error = "guardian_moderation_key_rotated";
                ok = false;
            } else {
                const std::string canonical = "sonalis-guardian-evidence-v1\n" + receipt->id + "\n"
                    + digest + "\nmessage_attachment\n" + messageId + "\n" + reportedUserId
                    + "\n" + reason;
                const std::string signature = messageCrypto_.Sign(canonical);
                ok = platform_.UploadMediaReportEvidence(receipt->id, evidence, digest, signature,
                    messageCrypto_.DeviceId(), metadata.mime, error);
            }
        }
        if (!evidence.empty()) crypto_wipe(evidence.data(), evidence.size());
        std::error_code ignored;
        std::filesystem::remove(encryptedPath, ignored);
        std::filesystem::remove(plaintextPath, ignored);
        const std::string reportId = receipt ? receipt->id : std::string{};
        return BackgroundWorker::Completion([this, ok, error, reportId] {
            mediaReportPending_ = false;
            if (!ok) {
                DiagnosticLog("guardian", "private_report_failed=" + error.substr(0, 96));
                uiMessage_ = UserFacingError(error.empty() ? "Medya raporu gönderilemedi." : error);
                return;
            }
            lastSubmittedMediaReportId_ = reportId;
            showMediaReportReceipt_ = true;
            reportAttachmentId_.clear();
            reportMessageId_.clear();
            reportUserId_.clear();
            mediaReportNoteBuffer_.fill('\0');
            uiMessage_.clear();
            DiagnosticLog("guardian", "private_report_submitted");
        });
    })) {
        mediaReportPending_ = false;
        uiMessage_ = "Guardian raporu arka plan kuyruğuna alınamadı.";
    }
}

void AppUi::WithdrawLastMediaReport() {
    if (mediaReportPending_ || lastSubmittedMediaReportId_.empty()) return;
    const std::string reportId = lastSubmittedMediaReportId_;
    mediaReportPending_ = true;
    if (!worker_.Submit([this, reportId] {
        std::string error;
        const bool ok = platform_.WithdrawMediaReport(reportId, "reporter_requested_withdrawal", error);
        return BackgroundWorker::Completion([this, ok, error] {
            mediaReportPending_ = false;
            if (!ok) {
                uiMessage_ = UserFacingError(error.empty() ? "Rapor geri çekilemedi." : error);
                return;
            }
            lastSubmittedMediaReportId_.clear();
            showMediaReportReceipt_ = false;
            uiMessage_ = "Medya raporu geri çekildi.";
        });
    })) {
        mediaReportPending_ = false;
        uiMessage_ = "Rapor geri çekme işlemi kuyruğa alınamadı.";
    }
}

void AppUi::RestartAudioDevices() {
    const ConnectionState state = network_.State();
    if ((state != ConnectionState::Connected && state != ConnectionState::Connecting) || audioRestartPending_) return;
    const std::string inputDeviceId = settings_.inputDeviceId;
    const std::string outputDeviceId = settings_.outputDeviceId;
    const bool voiceActivation = settings_.voiceActivation;
    const float vadSensitivity = settings_.vadSensitivity;
    audioRestartPending_ = true;
    if (!worker_.Submit([this, inputDeviceId, outputDeviceId, voiceActivation, vadSensitivity] {
        std::string error;
        const bool restarted = audio_.Start(inputDeviceId, outputDeviceId, &network_,
                                            voiceActivation, vadSensitivity, error);
        const bool microphoneAvailable = restarted && audio_.HasCaptureDevice();
        return BackgroundWorker::Completion([this, restarted, error, microphoneAvailable] {
            audioRestartPending_ = false;
            if (!restarted) uiMessage_ = "Ses cihazi degistirilemedi: " + error;
            else if (!microphoneAvailable) {
                uiMessage_ = "Mikrofon hala bulunamadi; dinleme modu devam ediyor.";
            } else uiMessage_ = "Ses cihazlari baglanti kesilmeden degistirildi";
        });
    })) {
        audioRestartPending_ = false;
        uiMessage_ = "Ses cihazi islemi kuyruga alinamadi";
    }
}

void AppUi::SaveSettings() {
    std::string ignored;
    store_.Save(settings_, ignored);
}

void AppUi::FlushDiagnosticTelemetry() {
    if (diagnosticUploadPending_ || !platform_.IsAuthenticated()) return;
    const std::uint64_t now = SteadyNowMs();
    if (now < nextDiagnosticUploadAttemptMs_ || PendingDiagnosticErrorCount() == 0) return;
    std::vector<DiagnosticErrorEvent> batch = TakePendingDiagnosticErrors(16);
    if (batch.empty()) return;
    const std::vector<DiagnosticErrorEvent> submitFallback = batch;
    diagnosticUploadPending_ = true;
    if (!worker_.Submit([this, batch = std::move(batch)]() mutable {
        std::string error;
        const bool sent = platform_.ReportDiagnosticErrors(batch, error);
        return BackgroundWorker::Completion(
            [this, batch = std::move(batch), sent]() mutable {
                diagnosticUploadPending_ = false;
                const std::uint64_t completedAt = SteadyNowMs();
                if (sent) {
                    diagnosticUploadFailures_ = 0;
                    nextDiagnosticUploadAttemptMs_ = completedAt + 15'000;
                } else {
                    RequeueDiagnosticErrors(batch);
                    diagnosticUploadFailures_ = static_cast<std::uint8_t>(
                        std::min<unsigned>(diagnosticUploadFailures_ + 1U, 6U));
                    const std::uint64_t retryDelay = std::min<std::uint64_t>(
                        15ULL * 60ULL * 1000ULL,
                        30'000ULL << std::min<unsigned>(diagnosticUploadFailures_ - 1U, 4U));
                    nextDiagnosticUploadAttemptMs_ = completedAt + retryDelay;
                }
            });
    })) {
        diagnosticUploadPending_ = false;
        RequeueDiagnosticErrors(submitFallback);
        nextDiagnosticUploadAttemptMs_ = now + 30'000;
    }
}

void AppUi::Tick() {
    if (!initialized_) return;
    worker_.DrainCompletions();
    if (HandleStartupUpdateGate()) return;
    const PlatformSessionState sessionState = platform_.SessionState();
    if ((sessionState == PlatformSessionState::Expired
         || sessionState == PlatformSessionState::UpdateRequired)
        && !terminalSessionHandled_) {
        terminalSessionHandled_ = true;
        ResetSessionView(sessionState == PlatformSessionState::UpdateRequired
            ? "Bu istemci sürümü artık desteklenmiyor. Sonalis'i güncelleyip yeniden deneyin."
            : "Oturumunuz sona erdi. Lütfen yeniden giriş yapın.");
    } else if (sessionState == PlatformSessionState::Active) {
        terminalSessionHandled_ = false;
    }
    if (network_.State() != ConnectionState::Connected
        && network_.StatusText().find("tek kullanici") != std::string::npos) voiceSleeping_ = true;
    HandleRealtimeEvents();
    const std::uint64_t now = SteadyNowMs();
    FlushDiagnosticTelemetry();
    if (now - lastMemorySampleMs_ >= 5'000U) {
        lastMemorySampleMs_ = now;
        processMemory_ = ReadProcessMemorySnapshot();
        const ResourceBudget budget = BudgetFor(settings_.resourceProfile);
        const bool overBudget = processMemory_.available
            && processMemory_.workingSetBytes > budget.workingSetWarningBytes;
        if (overBudget && !memoryBudgetWarning_) {
            char detail[160]{};
            std::snprintf(detail, sizeof(detail),
                          "profile=%u working_set_mb=%llu private_mb=%llu warning_mb=%llu",
                          static_cast<unsigned>(settings_.resourceProfile),
                          static_cast<unsigned long long>(processMemory_.workingSetBytes / (1024U * 1024U)),
                          static_cast<unsigned long long>(processMemory_.privateBytes / (1024U * 1024U)),
                          static_cast<unsigned long long>(budget.workingSetWarningBytes / (1024U * 1024U)));
            DiagnosticLog("performance.memory", detail);
        }
        memoryBudgetWarning_ = overBudget;
    }
    if (realtime_.IsConnected() && now - lastPresenceCheckMs_ >= 60'000
        && (ownPresence_ == "online" || ownPresence_ == "idle")) {
        lastPresenceCheckMs_ = now;
        LASTINPUTINFO input{sizeof(input)};
        const bool idle = GetLastInputInfo(&input)
            && static_cast<DWORD>(GetTickCount() - input.dwTime) >= 10U * 60U * 1000U
            && !audio_.IsTransmitting() && !audio_.HasActivePeerAudio();
        const std::string next = idle ? "idle" : "online";
        if (next != ownPresence_) {
            ownPresence_ = next;
            realtime_.SetPresence(ownPresence_, customStatusBuffer_.data());
        }
    }
    if (updater_.CheckDue()) updater_.CheckAsync(settings_.controlOrigin);
    const UpdateState currentUpdateState = updater_.State();
    if (currentUpdateState != observedUpdateState_) {
        if (observedUpdateState_ == UpdateState::Checking && manualUpdateCheckPending_) {
            if (currentUpdateState == UpdateState::Error) {
                uiMessage_ = "Guncelleme kontrol edilemedi: " + updater_.Status();
            }
            manualUpdateCheckPending_ = false;
        }
        if (updateDownloadPending_
            && (currentUpdateState == UpdateState::Ready || currentUpdateState == UpdateState::Error)) {
            updateDownloadPending_ = false;
            if (currentUpdateState == UpdateState::Ready) updateInstallPromptPending_ = true;
            else uiMessage_ = "Güncelleme indirilemedi: " + updater_.Status();
        }
        observedUpdateState_ = currentUpdateState;
    }
    if (pendingRoomAutoConnect_ && platform_.IsAuthenticated() && !platformRooms_.empty()
        && network_.State() == ConnectionState::Disconnected) {
        pendingRoomAutoConnect_ = false;
        Connect();
    }
    if (network_.State() == ConnectionState::Error && audio_.IsRunning()) audio_.Stop();
}

void AppUi::Render() {
    if (!initialized_) return;

    const float dpiScale = ResolveDpiScale(windowHandle_);
    gRenderDpiScale = dpiScale;

    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    ImGui::Begin("Sonalis", nullptr, flags);
    ImGui::PopStyleVar();

    if (startupUpdateGate_) {
        RenderStartupUpdateGate(dpiScale);
        ImGui::End();
        return;
    }

    if (!platform_.IsAuthenticated()) {
        voiceMeterVisible_.store(false, std::memory_order_relaxed);
        RenderLogin();
        ImGui::End();
        return;
    }

    const bool connectedOrConnecting = network_.State() == ConnectionState::Connected
        || network_.State() == ConnectionState::Connecting;
    const bool memberPanelContext = activePage_ == ClientPage::Voice
        || activePage_ == ClientPage::Rooms
        || (activePage_ == ClientPage::Messages && directConversationId_.empty());
    const ClientPage pageAtFrameStart = activePage_;
    voiceMeterVisible_.store(activePage_ == ClientPage::Voice, std::memory_order_relaxed);
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_M, false)) {
        ToggleMicrophoneMuted();
    }
    if (!io.WantTextInput && io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_D, false)) {
        ToggleOutputMuted();
    }
    if (experiencePolicy_.quickSwitcherEnabled && !io.WantTextInput
        && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false)) {
        ImGui::OpenPopup("Hizli gecis");
    }
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)
        && activePage_ == ClientPage::Messages) {
        focusChatSearch_ = true;
    }
    if (!io.WantTextInput && io.KeyAlt && roomOverview_
        && (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) || ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))) {
        const auto& channels = roomOverview_->channels;
        if (!channels.empty()) {
            auto current = std::find_if(channels.begin(), channels.end(),
                [this](const PlatformChannel& channel) { return channel.id == selectedChannelId_; });
            std::ptrdiff_t index = current == channels.end() ? 0 : std::distance(channels.begin(), current);
            index += ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ? 1 : -1;
            if (index < 0) index = static_cast<std::ptrdiff_t>(channels.size()) - 1;
            if (index >= static_cast<std::ptrdiff_t>(channels.size())) index = 0;
            const PlatformChannel& channel = channels[static_cast<std::size_t>(index)];
            if (connectedOrConnecting && channel.type == "voice" && activeVoiceChannelId_ != channel.id) Disconnect();
            selectedChannelId_ = channel.id;
            selectedChannelType_ = channel.type;
            directConversationId_.clear();
            directConversationLabel_.clear();
            if (channel.type == "text") {
                chatChannelId_ = channel.id;
                const auto draft = chatDrafts_.find(chatChannelId_);
                CopyToBuffer(draft == chatDrafts_.end() ? std::string{} : draft->second, chatBuffer_);
                activePage_ = ClientPage::Messages;
                WipeChatLines();
                RefreshChat();
            } else {
                activeVoiceChannelId_ = channel.id;
                activePage_ = ClientPage::Voice;
                if (!connectedOrConnecting) Connect();
            }
        }
    }
    ImGui::SetNextWindowSize(ImVec2(520.0F, 430.0F), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Hizli gecis", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::TextUnformatted("Oda veya kanal sec");
        ImGui::TextDisabled("Ctrl+K ile acilir, Esc ile kapanir");
        ImGui::Separator();
        if (ImGui::BeginChild("quickSwitchList", ImVec2(0.0F, -40.0F), ImGuiChildFlags_Borders)) {
            for (int index = 0; index < static_cast<int>(platformRooms_.size()); ++index) {
                const PlatformRoom& room = platformRooms_[static_cast<std::size_t>(index)];
                const std::string label = "Oda  " + room.name + "##quick-room-" + room.id;
                if (ImGui::Selectable(label.c_str(), room.id == settings_.lastRoomId)) {
                    selectedRoomIndex_ = index;
                    settings_.lastRoomId = room.id;
                    CopyToBuffer(room.name, roomBuffer_);
                    SaveSettings();
                    WipeChatLines();
                    chatConversationId_.clear();
                    directConversationId_.clear();
                    directConversationLabel_.clear();
                    crypto_wipe(chatKey_.data(), chatKey_.size());
                    chatKeyReady_ = false;
                    roomMembers_.clear();
                    roomOverview_.reset();
                    selectedChannelId_.clear();
                    selectedChannelType_.clear();
                    chatChannelId_.clear();
                    activeVoiceChannelId_.clear();
                    RefreshRoomOverview();
                    ImGui::CloseCurrentPopup();
                }
            }
            if (roomOverview_) {
                ImGui::SeparatorText("KANALLAR");
                for (const auto& channel : roomOverview_->channels) {
                    const std::string label = (channel.type == "voice" ? "Ses  " : "#  ")
                        + channel.name + "##quick-channel-" + channel.id;
                    if (!ImGui::Selectable(label.c_str(), channel.id == selectedChannelId_)) continue;
                    if (connectedOrConnecting && channel.type == "voice" && activeVoiceChannelId_ != channel.id) Disconnect();
                    selectedChannelId_ = channel.id;
                    selectedChannelType_ = channel.type;
                    directConversationId_.clear();
                    directConversationLabel_.clear();
                    if (channel.type == "text") {
                        chatChannelId_ = channel.id;
                        const auto draft = chatDrafts_.find(chatChannelId_);
                        CopyToBuffer(draft == chatDrafts_.end() ? std::string{} : draft->second, chatBuffer_);
                        activePage_ = ClientPage::Messages;
                        WipeChatLines();
                        RefreshChat();
                    } else {
                        activeVoiceChannelId_ = channel.id;
                        activePage_ = ClientPage::Voice;
                        if (!connectedOrConnecting) Connect();
                    }
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndChild();
    if (ImGui::Button("Kapat", ImVec2(-FLT_MIN, 30.0F * dpiScale))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    const ImVec2 workspaceAvailable = ImGui::GetContentRegionAvail();
    const float contentWidth = workspaceAvailable.x;
    const HorizonLayoutRequest layoutRequest{
        .viewportWidthPixels = workspaceAvailable.x,
        .viewportHeightPixels = workspaceAvailable.y,
        .dpiScale = dpiScale,
        .density = HorizonDensityFromSetting(settings_.uiDensity),
        .resourceProfile = settings_.resourceProfile,
        .channelPanelRequested = true,
        .memberPanelRequested = showMemberPanel_ && memberPanelContext,
        .utilityPanelRequested = false,
    };
    horizonLayout_ = CalculateHorizonLayout(layoutRequest);
    if (!horizonLayoutInitialized_ || previousHorizonLayoutClass_ != horizonLayout_.layoutClass) {
        showChannelPanel_ = horizonLayout_.layoutClass != HorizonLayoutClass::Compact
            || activePage_ == ClientPage::Voice || activePage_ == ClientPage::Rooms;
        showMemberPanel_ = horizonLayout_.layoutClass == HorizonLayoutClass::Standard
            || horizonLayout_.layoutClass == HorizonLayoutClass::Wide
            || horizonLayout_.layoutClass == HorizonLayoutClass::Studio
            || horizonLayout_.layoutClass == HorizonLayoutClass::Ultra;
        previousHorizonLayoutClass_ = horizonLayout_.layoutClass;
        horizonLayoutInitialized_ = true;
    }
    const float gap = horizonLayout_.panelGap * dpiScale;
    const float railWidth = horizonLayout_.communityRail.bounds.width * dpiScale;
    const float leftWidth = horizonLayout_.channelPanel.bounds.width * dpiScale;
    const float bottomBarHeight = horizonLayout_.bottomBarHeight * dpiScale;
    layout_ = horizonLayout_.layoutClass == HorizonLayoutClass::Wide
            || horizonLayout_.layoutClass == HorizonLayoutClass::Studio
            || horizonLayout_.layoutClass == HorizonLayoutClass::Ultra
        ? ClientLayout::Wide : ClientLayout::Compact;
    ImGui::BeginChild("content", ImVec2(contentWidth, -bottomBarHeight - gap), ImGuiChildFlags_None);

    const std::string status = voiceSleeping_ ? "Ses uyuyor" : network_.StatusText();
    const ImVec4 statusColor = network_.State() == ConnectionState::Connected
        ? ImVec4(0.25F, 0.90F, 0.62F, 1.0F)
        : (network_.State() == ConnectionState::Error ? ImVec4(1.0F, 0.42F, 0.36F, 1.0F)
                                                       : ImVec4(0.55F, 0.67F, 0.82F, 1.0F));
    const PlatformUser account = platform_.User();
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0F);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::BeginChild("navigation", ImVec2(railWidth, -1.0F),
                      ImGuiChildFlags_AlwaysUseWindowPadding);
    unsigned unreadMessages = 0;
    for (const auto& conversation : conversations_) unreadMessages += conversation.unreadCount;
    const auto selectPage = [this](const ClientPage page) {
            const bool changed = activePage_ != page;
            activePage_ = page;
            if (horizonLayout_.channelPanel.IsDrawer()
                && (page == ClientPage::Voice || page == ClientPage::Rooms)) {
                showChannelPanel_ = true;
                showMemberPanel_ = false;
            }
            if (changed && page == ClientPage::Settings) {
                resetSettingsNavigationScroll_ = true;
            }
            if (changed && page == ClientPage::Home) RefreshSocial();
            if (changed && page == ClientPage::Messages && chatLines_.empty()) RefreshChat();
    };
    if (NavigationButton("home", "Ana Sayfa", ClientPage::Home, activePage_)) selectPage(ClientPage::Home);
    if (NavigationButton("messages", T(TextId::Messages), ClientPage::Messages, activePage_, unreadMessages)) selectPage(ClientPage::Messages);
    ImGui::Separator();
    const float railFooterReserve = 188.0F * dpiScale;
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 3.0F * dpiScale);
    ImGui::BeginChild("communityScroller", ImVec2(0.0F, -railFooterReserve),
                      ImGuiChildFlags_None);
    for (std::size_t index = 0; index < platformRooms_.size(); ++index) {
        if (!CommunityButton(platformRooms_[index], static_cast<int>(index) == selectedRoomIndex_,
                             static_cast<unsigned>(index + 1U))) continue;
        selectedRoomIndex_ = static_cast<int>(index);
        const PlatformRoom& selectedCommunity = platformRooms_[index];
        settings_.lastRoomId = selectedCommunity.id;
        CopyToBuffer(selectedCommunity.name, roomBuffer_);
        SaveSettings();
        WipeChatLines();
        chatConversationId_.clear();
        directConversationId_.clear();
        directConversationLabel_.clear();
        crypto_wipe(chatKey_.data(), chatKey_.size());
        chatKeyReady_ = false;
        roomMembers_.clear();
        roomOverview_.reset();
        selectedChannelId_.clear();
        selectedChannelType_.clear();
        chatChannelId_.clear();
        activeVoiceChannelId_.clear();
        activePage_ = ClientPage::Rooms;
        if (horizonLayout_.channelPanel.IsDrawer()) {
            showChannelPanel_ = true;
            showMemberPanel_ = false;
        }
        RefreshRoomOverview();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::Separator();
    if (NavigationButton("rooms", T(TextId::Rooms), ClientPage::Rooms, activePage_)) selectPage(ClientPage::Rooms);
    if (NavigationButton("voice", T(TextId::Voice), ClientPage::Voice, activePage_)) selectPage(ClientPage::Voice);
    if (NavigationButton("settings", T(TextId::Settings), ClientPage::Settings, activePage_)) selectPage(ClientPage::Settings);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImVec4 sidebarSurface = ImGui::GetStyleColorVec4(ImGuiCol_ChildBg);
    sidebarSurface.x = std::min(1.0F, sidebarSurface.x * 1.14F);
    sidebarSurface.y = std::min(1.0F, sidebarSurface.y * 1.12F);
    sidebarSurface.z = std::min(1.0F, sidebarSurface.z * 1.10F);
    const bool renderChannelPanel = horizonLayout_.channelPanel.IsInline() || showChannelPanel_;
    if (renderChannelPanel) {
    ImGui::SameLine(0.0F, gap);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, sidebarSurface);
    ImGui::BeginChild("connection", ImVec2(leftWidth, -1.0F),
                      ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::BeginChild("connectionScroll", ImVec2(0.0F, -54.0F * dpiScale), ImGuiChildFlags_None);
    if (activePage_ == ClientPage::Settings && resetSettingsNavigationScroll_) {
        ImGui::SetScrollY(0.0F);
        resetSettingsNavigationScroll_ = false;
    }
    const char* sideTitle = activePage_ == ClientPage::Voice ? T(TextId::VoiceRooms)
        : activePage_ == ClientPage::Rooms ? T(TextId::RoomManagement)
        : activePage_ == ClientPage::Messages ? T(TextId::Conversations)
        : activePage_ == ClientPage::Home ? "Ana sayfa" : T(TextId::Settings);
    const bool communityContextPage = activePage_ == ClientPage::Voice || activePage_ == ClientPage::Rooms
        || (activePage_ == ClientPage::Messages && directConversationId_.empty());
    const char* sidebarHeading = roomOverview_ && communityContextPage
        ? roomOverview_->name.c_str() : sideTitle;
    SectionTitle(sidebarHeading, nullptr);
    if (roomOverview_ && communityContextPage) {
        ImGui::TextDisabled("%s", roomOverview_->description.empty() ? sideTitle : roomOverview_->description.c_str());
    } else if (activePage_ == ClientPage::Home) {
        ImGui::TextDisabled("Arkadaşlar, istekler ve bildirimler");
    } else if (activePage_ == ClientPage::Settings) {
        ImGui::TextDisabled("Cihaz, görünüm ve gizlilik tercihleri");
    } else {
        ImGui::TextDisabled("Topluluklarını ve konuşmalarını yönet");
    }
    ImGui::Spacing();
    if (activePage_ == ClientPage::Voice || activePage_ == ClientPage::Rooms) {
    if (connectedOrConnecting) ImGui::BeginDisabled();
    FieldLabel(T(TextId::Room));
    if (platformRooms_.empty()) {
        ImGui::TextDisabled("%s", T(TextId::NoRooms));
    } else {
        selectedRoomIndex_ = std::clamp(selectedRoomIndex_, 0, static_cast<int>(platformRooms_.size() - 1));
        if (ImGui::BeginCombo("##platformRoom", platformRooms_[static_cast<std::size_t>(selectedRoomIndex_)].name.c_str())) {
            for (int index = 0; index < static_cast<int>(platformRooms_.size()); ++index) {
                const bool selected = index == selectedRoomIndex_;
                const PlatformRoom& listedRoom = platformRooms_[static_cast<std::size_t>(index)];
                const char* roleLabel = listedRoom.role == "owner" ? "Sahip" : listedRoom.role == "admin" ? "Yonetici" : listedRoom.role == "mod" ? "Moderator" : "Uye";
                const std::string roomLabel = "[" + std::string(roleLabel) + "] " + listedRoom.name;
                if (ImGui::Selectable(roomLabel.c_str(), selected)) {
                    selectedRoomIndex_ = index; settings_.lastRoomId = platformRooms_[static_cast<std::size_t>(index)].id;
                    CopyToBuffer(platformRooms_[static_cast<std::size_t>(index)].name, roomBuffer_); SaveSettings();
                    WipeChatLines(); chatConversationId_.clear(); directConversationId_.clear(); directConversationLabel_.clear();
                    crypto_wipe(chatKey_.data(), chatKey_.size()); chatKeyReady_ = false; roomMembers_.clear();
                    roomOverview_.reset(); selectedChannelId_.clear(); selectedChannelType_.clear();
                    chatChannelId_.clear(); activeVoiceChannelId_.clear();
                    generatedInviteBuffer_.fill('\0');
                    RefreshRoomOverview();
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        const PlatformRoom& selectedRoom = platformRooms_[static_cast<std::size_t>(selectedRoomIndex_)];
        const char* selectedRole = selectedRoom.role == "owner" ? "Sahip" : selectedRoom.role == "admin" ? "Yonetici" : selectedRoom.role == "mod" ? "Moderator" : "Uye";
        const char* nodeLabel = connectedOrConnecting
            ? "aktif"
            : (selectedRoom.nodeState.empty() ? "oda etkin degil" : selectedRoom.nodeState.c_str());
        ImGui::TextDisabled("Rol: %s  |  Dugum: %s", selectedRole, nodeLabel);
        ImGui::Spacing();
        ImGui::SeparatorText("KANALLAR");
        if (overviewRefreshPending_) {
            ImGui::TextDisabled("Kanallar yukleniyor...");
        } else if (roomOverview_) {
            const auto selectChannel = [this, connectedOrConnecting](const PlatformChannel& channel) {
                const bool changed = selectedChannelId_ != channel.id;
                if (!changed) return;
                if (connectedOrConnecting && channel.type == "voice" && activeVoiceChannelId_ != channel.id) Disconnect();
                selectedChannelId_ = channel.id;
                selectedChannelType_ = channel.type;
                directConversationId_.clear();
                directConversationLabel_.clear();
                if (channel.type == "text") {
                    chatChannelId_ = channel.id;
                    const auto draft = chatDrafts_.find(chatChannelId_);
                    CopyToBuffer(draft == chatDrafts_.end() ? std::string{} : draft->second, chatBuffer_);
                    activePage_ = ClientPage::Messages;
                    WipeChatLines();
                    chatBeforeCursor_.clear();
                    chatAfterCursor_.clear();
                    RefreshChat();
                } else {
                    activeVoiceChannelId_ = channel.id;
                    activePage_ = ClientPage::Voice;
                    if (!connectedOrConnecting) Connect();
                }
            };
            const auto drawChannels = [&](const std::string& categoryId) {
                for (const auto& channel : roomOverview_->channels) {
                    if (channel.categoryId != categoryId) continue;
                    ImGui::PushID(channel.id.c_str());
                    const bool active = selectedChannelId_ == channel.id;
                    if (ChannelMenuRow(channel, active)) {
                        selectChannel(channel);
                        if (horizonLayout_.channelPanel.IsDrawer()) showChannelPanel_ = false;
                    }
                    if ((selectedRoom.role == "owner" || selectedRoom.role == "admin")
                        && ImGui::BeginPopupContextItem("channelManageMenu")) {
                        if (ImGui::MenuItem("Adini degistir")) {
                            channelManageAction_ = "channel_rename";
                            channelManageTargetId_ = channel.id;
                            channelManageTargetLabel_ = channel.name;
                            CopyToBuffer(channel.name, manageChannelNameBuffer_);
                        }
                        if (ImGui::MenuItem("Guvenlik ve medya")) {
                            channelManageAction_ = "channel_safety";
                            channelManageTargetId_ = channel.id;
                            channelManageTargetLabel_ = channel.name;
                            channelManageContentRating_ = channel.contentRating == "adult" ? 1 : 0;
                            channelManageMediaPolicy_ = channel.mediaPostingPolicy == "disabled" ? 0
                                : channel.mediaPostingPolicy == "moderators" ? 2 : 1;
                            channelManageLocalScan_ = channel.localMediaScanRequired;
                        }
                        if (ImGui::MenuItem("Kanali arsivle")) {
                            channelManageAction_ = "channel_delete";
                            channelManageTargetId_ = channel.id;
                            channelManageTargetLabel_ = channel.name;
                        }
                        ImGui::EndPopup();
                    }
                    if (channel.type == "voice" && activeVoiceChannelId_ == channel.id && connectedOrConnecting) {
                        ImGui::Indent(14.0F);
                        ImGui::TextDisabled("%s", account.nickname.c_str());
                        const auto activePeers = network_.PeersSnapshot();
                        if (activePeers) {
                            for (const auto& peer : *activePeers) ImGui::TextDisabled("%s", peer.nickname.c_str());
                        }
                        ImGui::Unindent(14.0F);
                    }
                    ImGui::PopID();
                }
            };
            drawChannels({});
            for (const auto& category : roomOverview_->categories) {
                ImVec4 categoryHover = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
                categoryHover.w = 0.64F;
                ImVec4 categoryActive = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
                categoryActive.w = 0.74F;
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, categoryHover);
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, categoryActive);
                const bool categoryOpen = ImGui::CollapsingHeader(category.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                ImGui::PopStyleColor(3);
                if ((selectedRoom.role == "owner" || selectedRoom.role == "admin")
                    && ImGui::BeginPopupContextItem(("categoryManageMenu-" + category.id).c_str())) {
                    if (ImGui::MenuItem("Adini degistir")) {
                        channelManageAction_ = "category_rename";
                        channelManageTargetId_ = category.id;
                        channelManageTargetLabel_ = category.name;
                        CopyToBuffer(category.name, manageChannelNameBuffer_);
                    }
                    if (ImGui::MenuItem("Kategoriyi arsivle")) {
                        channelManageAction_ = "category_delete";
                        channelManageTargetId_ = category.id;
                        channelManageTargetLabel_ = category.name;
                    }
                    ImGui::EndPopup();
                }
                if (categoryOpen) drawChannels(category.id);
            }
            if (activePage_ == ClientPage::Rooms
                && (selectedRoom.role == "owner" || selectedRoom.role == "admin")) {
                if (SurfaceCollapsingHeader("Kanal ekle")) {
                    ImGui::InputTextWithHint("##newCategory", "Kategori adi", createCategoryNameBuffer_.data(), createCategoryNameBuffer_.size());
                    if (ImGui::SmallButton("Kategori olustur")) {
                        const std::string roomId = selectedRoom.id;
                        const std::string name(createCategoryNameBuffer_.data());
                        QueuePlatformAction([this, roomId, name](std::string& error) {
                            return platform_.CreateChannelCategory(roomId, name, error);
                        }, "Kategori olusturuldu.", false, false, false, true);
                        createCategoryNameBuffer_.fill('\0');
                    }
                    ImGui::InputTextWithHint("##newChannel", "Kanal adi", createChannelNameBuffer_.data(), createChannelNameBuffer_.size());
                    const char* types[] = {"Metin", "Ses"};
                    ImGui::Combo("##newChannelType", &createChannelType_, types, IM_ARRAYSIZE(types));
                    const char* contentRatings[] = {"Genel (SFW)", "Yetiskin (18+)"};
                    ImGui::TextDisabled("Icerik sinifi");
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::Combo("##createChannelContentRating", &createChannelContentRating_, contentRatings,
                                 IM_ARRAYSIZE(contentRatings));
                    const char* mediaPolicies[] = {"Medya kapali", "Uyeler paylasabilir", "Yalniz moderatorler"};
                    ImGui::TextDisabled("Medya paylasimi");
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::Combo("##createChannelMediaPolicy", &createChannelMediaPolicy_, mediaPolicies,
                                 IM_ARRAYSIZE(mediaPolicies));
                    ImGui::Checkbox("Yerel medya taramasi zorunlu", &createChannelLocalScan_);
                    if (ImGui::SmallButton("Kanal olustur")) {
                        const std::string roomId = selectedRoom.id;
                        const std::string name(createChannelNameBuffer_.data());
                        const std::string type = createChannelType_ == 0 ? "text" : "voice";
                        const std::string contentRating = createChannelContentRating_ == 1 ? "adult" : "sfw";
                        const std::string mediaPolicy = createChannelMediaPolicy_ == 0 ? "disabled"
                            : createChannelMediaPolicy_ == 2 ? "moderators" : "members";
                        const bool localScanRequired = createChannelLocalScan_;
                        const std::string categoryId = roomOverview_->categories.empty() ? std::string{} : roomOverview_->categories.front().id;
                        QueuePlatformAction([this, roomId, categoryId, type, name, contentRating,
                                             mediaPolicy, localScanRequired](std::string& error) {
                            return platform_.CreateRoomChannel(roomId, categoryId, type, name, contentRating,
                                                               mediaPolicy, localScanRequired, error);
                        }, "Kanal olusturuldu.", false, false, false, true);
                        createChannelNameBuffer_.fill('\0');
                    }
                    if (!platformActionPending_ && ImGui::SmallButton("Kanal listesini yenile")) RefreshRoomOverview();
                }
            }
            if (!channelManageAction_.empty()) ImGui::OpenPopup("Kanal yonetimi");
            if (ImGui::BeginPopupModal("Kanal yonetimi", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                const bool rename = channelManageAction_.ends_with("_rename");
                const bool category = channelManageAction_.starts_with("category_");
                const bool safety = channelManageAction_ == "channel_safety";
                ImGui::Text("%s: %s", category ? "Kategori" : "Kanal", channelManageTargetLabel_.c_str());
                if (rename) {
                    ImGui::InputText("Yeni ad", manageChannelNameBuffer_.data(), manageChannelNameBuffer_.size());
                } else if (safety) {
                    const char* contentRatings[] = {"Genel (SFW)", "Yetiskin (18+)"};
                    const char* mediaPolicies[] = {"Medya kapali", "Uyeler paylasabilir", "Yalniz moderatorler"};
                    ImGui::Combo("Icerik sinifi", &channelManageContentRating_, contentRatings,
                                 IM_ARRAYSIZE(contentRatings));
                    ImGui::Combo("Medya paylasimi", &channelManageMediaPolicy_, mediaPolicies,
                                 IM_ARRAYSIZE(mediaPolicies));
                    ImGui::Checkbox("Yerel medya taramasi zorunlu", &channelManageLocalScan_);
                    ImGui::TextWrapped("18+ kanallar yalniz yetiskin hesaplara gosterilir. "
                                       "Yerel tarama, medya sifrelenmeden once cihazda yapilir.");
                } else {
                    ImGui::TextWrapped("Bu oge arsivlenecek. Mesajlar silinmez; son metin veya ses kanali arsivlenemez.");
                }
                const bool validName = !rename || std::strlen(manageChannelNameBuffer_.data()) >= 2;
                if (!validName || platformActionPending_) ImGui::BeginDisabled();
            if (ImGui::Button(rename || safety ? "Kaydet" : "Arsivle",
                              ImVec2(130.0F * dpiScale, 32.0F * dpiScale))) {
                    const std::string roomId = selectedRoom.id;
                    const std::string targetId = channelManageTargetId_;
                    const std::string name(manageChannelNameBuffer_.data());
                    const std::string action = channelManageAction_;
                    const std::string contentRating = channelManageContentRating_ == 1 ? "adult" : "sfw";
                    const std::string mediaPolicy = channelManageMediaPolicy_ == 0 ? "disabled"
                        : channelManageMediaPolicy_ == 2 ? "moderators" : "members";
                    const bool localScanRequired = channelManageLocalScan_;
                    if (action == "channel_delete" && activeVoiceChannelId_ == targetId) Disconnect();
                    channelManageAction_.clear();
                    channelManageTargetId_.clear();
                    channelManageTargetLabel_.clear();
                    if (action == "category_rename") {
                        QueuePlatformAction([this, roomId, targetId, name](std::string& error) {
                            return platform_.RenameChannelCategory(roomId, targetId, name, error);
                        }, "Kategori adi degistirildi.", false, false, false, true);
                    } else if (action == "category_delete") {
                        QueuePlatformAction([this, roomId, targetId](std::string& error) {
                            return platform_.DeleteChannelCategory(roomId, targetId, error);
                        }, "Kategori arsivlendi.", false, false, false, true);
                    } else if (action == "channel_rename") {
                        QueuePlatformAction([this, roomId, targetId, name](std::string& error) {
                            return platform_.RenameRoomChannel(roomId, targetId, name, error);
                        }, "Kanal adi degistirildi.", false, false, false, true);
                    } else if (action == "channel_safety") {
                        QueuePlatformAction([this, roomId, targetId, contentRating, mediaPolicy,
                                             localScanRequired](std::string& error) {
                            return platform_.UpdateRoomChannelSafety(roomId, targetId, contentRating,
                                                                     mediaPolicy, localScanRequired, error);
                        }, "Kanal guvenlik ayarlari kaydedildi.", false, false, false, true);
                    } else {
                        QueuePlatformAction([this, roomId, targetId](std::string& error) {
                            return platform_.DeleteRoomChannel(roomId, targetId, error);
                        }, "Kanal arsivlendi.", false, false, false, true);
                    }
                    ImGui::CloseCurrentPopup();
                }
                if (!validName || platformActionPending_) ImGui::EndDisabled();
                ImGui::SameLine();
            if (ImGui::Button(T(TextId::Cancel), ImVec2(130.0F * dpiScale, 32.0F * dpiScale))) {
                    channelManageAction_.clear();
                    channelManageTargetId_.clear();
                    channelManageTargetLabel_.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        } else if (ImGui::SmallButton("Kanallari yukle")) {
            RefreshRoomOverview();
        }
        if (activePage_ == ClientPage::Rooms
            && (selectedRoom.role == "owner" || selectedRoom.role == "admin")) {
            bool roomDenoiseEnabled = selectedRoom.serverDenoiseEnabled;
            if (ImGui::Checkbox(T(TextId::ServerDenoiseForRoom), &roomDenoiseEnabled)) {
                const std::string roomId = selectedRoom.id;
                QueuePlatformAction([this, roomId, roomDenoiseEnabled](std::string& error) {
                    return platform_.UpdateRoomDenoise(roomId, roomDenoiseEnabled, error);
                }, roomDenoiseEnabled ? "Oda DSP izni acildi." : "Oda DSP izni kapatildi.", true, false, false);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", T(TextId::ServerDenoiseEligibility));
            ImGui::PopStyleColor();
            const float inviteFieldGap = 8.0F * dpiScale;
            const float inviteAvailableWidth = ImGui::GetContentRegionAvail().x;
            const bool inviteFieldsInline = inviteAvailableWidth >= 310.0F * dpiScale;
            const float inviteFieldWidth = inviteFieldsInline
                ? (inviteAvailableWidth - inviteFieldGap) * 0.5F : inviteAvailableWidth;
            ImGui::BeginGroup();
            ImGui::TextDisabled("%s", T(TextId::DurationHours));
            ImGui::SetNextItemWidth(inviteFieldWidth);
            ImGui::InputInt("##inviteExpiry", &inviteExpiresHours_, 24, 168);
            ImGui::EndGroup();
            inviteExpiresHours_ = std::clamp(inviteExpiresHours_, 1, 2160);
            if (inviteFieldsInline) ImGui::SameLine(0.0F, inviteFieldGap);
            ImGui::BeginGroup();
            ImGui::TextDisabled("%s", T(TextId::MaximumUses));
            ImGui::SetNextItemWidth(inviteFieldWidth);
            ImGui::InputInt("##inviteUses", &inviteMaxUses_, 1, 10);
            ImGui::EndGroup();
            inviteMaxUses_ = std::clamp(inviteMaxUses_, 1, 10000);
            if (ImGui::Button(T(TextId::CreateInvite), ImVec2(-FLT_MIN, 32.0F * dpiScale))) {
                const std::string roomId = selectedRoom.id;
                const int expiresHours = inviteExpiresHours_;
                const int maxUses = inviteMaxUses_;
                if (!platformActionPending_) {
                    platformActionPending_ = true;
                    if (!worker_.Submit([this, roomId, expiresHours, maxUses] {
                        std::string error;
                        auto invite = platform_.CreateRoomInvite(roomId, expiresHours, maxUses, error);
                        return BackgroundWorker::Completion([this, invite = std::move(invite), error] {
                            platformActionPending_ = false;
                            if (!invite) { uiMessage_ = error; return; }
                            CopyToBuffer(invite->code, generatedInviteBuffer_);
                            ImGui::SetClipboardText(invite->code.c_str());
                            uiMessage_ = "Davet kodu olusturuldu ve panoya kopyalandi.";
                        });
                    })) platformActionPending_ = false;
                }
            }
            if (generatedInviteBuffer_[0] != '\0') {
                const float copyButtonWidth = ImGui::CalcTextSize(T(TextId::Copy)).x + 24.0F * dpiScale;
                const float generatedWidth = ImGui::GetContentRegionAvail().x;
                const bool generatedInline = generatedWidth >= copyButtonWidth + 150.0F * dpiScale;
                ImGui::SetNextItemWidth(generatedInline ? generatedWidth - copyButtonWidth - 8.0F * dpiScale
                                                        : generatedWidth);
                ImGui::InputText("##generatedInvite", generatedInviteBuffer_.data(), generatedInviteBuffer_.size(), ImGuiInputTextFlags_ReadOnly);
                if (generatedInline) ImGui::SameLine(0.0F, 8.0F * dpiScale);
                if (ImGui::Button(T(TextId::Copy), ImVec2(generatedInline ? copyButtonWidth : -FLT_MIN, 0.0F))) {
                    ImGui::SetClipboardText(generatedInviteBuffer_.data());
                }
            }
        }
    }
    if (connectedOrConnecting) ImGui::EndDisabled();
    if (activePage_ == ClientPage::Rooms) {
    const float joinButtonWidth = ImGui::CalcTextSize(T(TextId::JoinWithCode)).x + 24.0F * dpiScale;
    const float joinAvailableWidth = ImGui::GetContentRegionAvail().x;
    const bool joinInline = joinAvailableWidth >= joinButtonWidth + 150.0F * dpiScale;
    ImGui::SetNextItemWidth(joinInline ? joinAvailableWidth - joinButtonWidth - 8.0F * dpiScale
                                       : joinAvailableWidth);
    ImGui::InputTextWithHint("##invite", "SS-XXXX-XXXX", inviteBuffer_.data(), inviteBuffer_.size(), ImGuiInputTextFlags_CharsUppercase);
    if (joinInline) ImGui::SameLine(0.0F, 8.0F * dpiScale);
    const bool validInviteCode = HasNonWhitespace(std::string_view(inviteBuffer_.data()));
    if (!validInviteCode || platformActionPending_) ImGui::BeginDisabled();
    if (ImGui::Button(T(TextId::JoinWithCode), ImVec2(joinInline ? joinButtonWidth : -FLT_MIN, 0.0F))) {
        const std::string code(inviteBuffer_.data());
        QueuePlatformAction([this, code](std::string& error) { return platform_.JoinRoomCode(code, error); },
                            "Odaya katıldınız.", true, false, false, false,
                            [this] { inviteBuffer_.fill('\0'); });
    }
    if (!validInviteCode || platformActionPending_) ImGui::EndDisabled();
    if (ImGui::Button(T(TextId::RefreshRooms), ImVec2(-FLT_MIN, 30.0F * dpiScale))) {
        RefreshPlatformRooms();
    }

    if (SurfaceCollapsingHeader(T(TextId::NewRoom))) {
        ImGui::InputTextWithHint("##createRoomName", T(TextId::RoomName), createRoomNameBuffer_.data(), createRoomNameBuffer_.size());
        ImGui::InputTextMultiline("##createRoomDescription", createRoomDescriptionBuffer_.data(),
                                  createRoomDescriptionBuffer_.size(), ImVec2(-FLT_MIN, 54.0F));
        const bool validRoomName = HasNonWhitespace(std::string_view(createRoomNameBuffer_.data()));
        if (!validRoomName || platformActionPending_) ImGui::BeginDisabled();
        if (ImGui::Button(T(TextId::CreateRoom), ImVec2(-FLT_MIN, 32.0F * dpiScale))) {
            const std::string name(createRoomNameBuffer_.data());
            const std::string description(createRoomDescriptionBuffer_.data());
            const auto createdRoomId = std::make_shared<std::string>();
            QueuePlatformAction([this, name, description, createdRoomId](std::string& error) {
                return platform_.CreateRoom(name, description, *createdRoomId, error);
            }, "Oda oluşturuldu.", true, false, false, false,
            [this, createdRoomId] {
                pendingRoomId_ = *createdRoomId;
                createRoomNameBuffer_.fill('\0');
                createRoomDescriptionBuffer_.fill('\0');
            });
        }
        if (!validRoomName || platformActionPending_) ImGui::EndDisabled();
    }

    if (!platformRooms_.empty() && SurfaceCollapsingHeader(T(TextId::MembersAndManagement))) {
        const PlatformRoom& selectedRoom = platformRooms_[static_cast<std::size_t>(
            std::clamp(selectedRoomIndex_, 0, static_cast<int>(platformRooms_.size() - 1)))];
        if (ImGui::SmallButton(roomMembersRefreshPending_ ? T(TextId::Refreshing) : T(TextId::RefreshMembers))) RefreshRoomMembers();
        const auto roleRank = [](const std::string& role) {
            if (role == "owner") return 4;
            if (role == "admin") return 3;
            if (role == "mod") return 2;
            return 1;
        };
        bool openBanPopup = false;
        for (const auto& member : roomMembers_) {
            ImGui::PushID(member.id.c_str());
            ImGui::Text("%s  @%s", member.nickname.c_str(), member.username.c_str());
            ImGui::SameLine(); ImGui::TextDisabled("%s", member.role.c_str());
            const bool canEditRole = (selectedRoom.role == "owner" || selectedRoom.role == "admin")
                && member.role != "owner" && member.id != account.id;
            if (canEditRole) {
                const std::string roomId = selectedRoom.id;
                const std::string memberId = member.id;
                if (ImGui::SmallButton(T(TextId::MemberRole))) QueuePlatformAction([this, roomId, memberId](std::string& error) {
                    return platform_.UpdateRoomMemberRole(roomId, memberId, "member", error);
                }, "Rol guncellendi.", false, false, true);
                ImGui::SameLine();
                if (ImGui::SmallButton(T(TextId::ModeratorRole))) QueuePlatformAction([this, roomId, memberId](std::string& error) {
                    return platform_.UpdateRoomMemberRole(roomId, memberId, "mod", error);
                }, "Rol guncellendi.", false, false, true);
                if (selectedRoom.role == "owner") {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(T(TextId::AdministratorRole))) QueuePlatformAction([this, roomId, memberId](std::string& error) {
                        return platform_.UpdateRoomMemberRole(roomId, memberId, "admin", error);
                    }, "Rol guncellendi.", false, false, true);
                }
            }
            if (member.id != account.id && roleRank(selectedRoom.role) > roleRank(member.role)) {
                ImGui::SameLine();
                if (ImGui::SmallButton(T(TextId::Ban))) {
                    banTargetUserId_ = member.id; banTargetLabel_ = member.nickname; banReasonBuffer_.fill('\0');
                    openBanPopup = true;
                }
            }
            ImGui::PopID();
        }
        if (openBanPopup) ImGui::OpenPopup("Kullaniciyi banla");
        if (ImGui::BeginPopupModal("Kullaniciyi banla", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s kullanicisini odadan banla", banTargetLabel_.c_str());
                ImGui::InputTextMultiline("##banReason", banReasonBuffer_.data(), banReasonBuffer_.size(),
                                          ImVec2(420.0F * dpiScale, 70.0F * dpiScale));
            if (ImGui::Button(T(TextId::Ban))) {
                const std::string roomId = selectedRoom.id;
                const std::string userId = banTargetUserId_;
                const std::string reason(banReasonBuffer_.data());
                QueuePlatformAction([this, roomId, userId, reason](std::string& error) {
                    return platform_.BanRoomMember(roomId, userId, reason, error);
                }, "Kullanici banlandi.", false, false, true);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(); if (ImGui::Button(T(TextId::Cancel))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    }
    }

    if (activePage_ == ClientPage::Messages) {
        if (directConversationId_.empty()) {
            EyebrowLabel("METİN KANALLARI");
            if (!roomOverview_) {
                ImGui::TextDisabled("Bir topluluk seçerek metin kanallarını görebilirsin.");
                if (ImGui::SmallButton("Odaları aç")) activePage_ = ClientPage::Rooms;
            } else {
                for (const auto& category : roomOverview_->categories) {
                    EyebrowLabel(category.name.c_str());
                    for (const auto& channel : roomOverview_->channels) {
                        if (channel.type != "text" || channel.categoryId != category.id) continue;
                        ImGui::PushID(("message-channel-" + channel.id).c_str());
                        if (ChannelMenuRow(channel, chatChannelId_ == channel.id)) {
                            const bool changed = chatChannelId_ != channel.id;
                            chatChannelId_ = channel.id;
                            selectedChannelId_ = channel.id;
                            selectedChannelType_ = channel.type;
                            if (changed) {
                                directConversationId_.clear();
                                directConversationLabel_.clear();
                                chatConversationId_.clear();
                                WipeChatLines();
                                crypto_wipe(chatKey_.data(), chatKey_.size());
                                chatKeyReady_ = false;
                                RefreshChat();
                            }
                            if (horizonLayout_.channelPanel.IsDrawer()) showChannelPanel_ = false;
                        }
                        ImGui::PopID();
                    }
                }
                bool uncategorizedShown = false;
                for (const auto& channel : roomOverview_->channels) {
                    if (channel.type != "text" || !channel.categoryId.empty()) continue;
                    if (!uncategorizedShown) { EyebrowLabel("GENEL"); uncategorizedShown = true; }
                    ImGui::PushID(("message-channel-" + channel.id).c_str());
                    if (ChannelMenuRow(channel, chatChannelId_ == channel.id)) {
                        chatChannelId_ = channel.id;
                        selectedChannelId_ = channel.id;
                        selectedChannelType_ = channel.type;
                        chatConversationId_.clear();
                        WipeChatLines();
                        crypto_wipe(chatKey_.data(), chatKey_.size());
                        chatKeyReady_ = false;
                        RefreshChat();
                        if (horizonLayout_.channelPanel.IsDrawer()) showChannelPanel_ = false;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::Dummy(ImVec2(0.0F, 5.0F * dpiScale));
            if (PanelLinkRow("open-direct-messages", "Özel mesajlara geç",
                             horizon::Icon::Messages)) {
                directConversationId_ = "picker";
                directConversationLabel_ = "Özel mesajlar";
                WipeChatLines();
            }
        } else {
            EyebrowLabel("ÖZEL MESAJLAR");
            bool anyDirect = false;
            for (const auto& friendEntry : friends_) {
                if (friendEntry.state != "accepted") continue;
                anyDirect = true;
                ImGui::PushID(("direct-picker-" + friendEntry.id).c_str());
                const bool selectedDirect = directConversationLabel_ == friendEntry.nickname;
                if (ImGui::Selectable(friendEntry.nickname.c_str(), selectedDirect, ImGuiSelectableFlags_None,
                    ImVec2(-FLT_MIN, 40.0F * dpiScale))) OpenDirectChat(friendEntry);
                ImGui::PopID();
            }
            if (!anyDirect) ImGui::TextDisabled("Mesaj gönderebileceğin bir arkadaşın yok.");
            ImGui::Dummy(ImVec2(0.0F, 5.0F * dpiScale));
            if (PanelLinkRow("back-to-community-channels", "Topluluk kanallarına dön",
                             horizon::Icon::Community)) {
                directConversationId_.clear();
                directConversationLabel_.clear();
                chatConversationId_.clear();
                WipeChatLines();
                crypto_wipe(chatKey_.data(), chatKey_.size());
                chatKeyReady_ = false;
                RefreshChat();
            }
        }
    }

    if (activePage_ == ClientPage::Home) {
        unsigned acceptedFriends = 0;
        unsigned pendingFriends = 0;
        unsigned unreadNotifications = 0;
        for (const auto& friendEntry : friends_) {
            if (friendEntry.state == "accepted") ++acceptedFriends;
            else if (friendEntry.state == "pending") ++pendingFriends;
        }
        for (const auto& notification : notifications_) if (!notification.read) ++unreadNotifications;
        if (SidebarMenuRow("homeFriends", "Arkadaşlar", acceptedFriends, horizon::Icon::Friends,
                           homeSection_ == HomeSection::Friends)) homeSection_ = HomeSection::Friends;
        if (SidebarMenuRow("homePending", "Bekleyenler", pendingFriends, horizon::Icon::Invite,
                           homeSection_ == HomeSection::Pending)) homeSection_ = HomeSection::Pending;
        if (SidebarMenuRow("homeNotifications", "Bildirimler", unreadNotifications, horizon::Icon::Notification,
                           homeSection_ == HomeSection::Notifications)) homeSection_ = HomeSection::Notifications;
        ImGui::Separator();
        if (homeSection_ == HomeSection::Friends) {
        const float searchButtonWidth = ImGui::CalcTextSize(T(TextId::Search)).x + 24.0F * dpiScale;
        ImGui::SetNextItemWidth(std::max(80.0F * dpiScale,
                                        ImGui::GetContentRegionAvail().x - searchButtonWidth - 8.0F * dpiScale));
        ImGui::InputTextWithHint("##friendSearch", T(TextId::SearchUsername), friendSearchBuffer_.data(), friendSearchBuffer_.size());
        ImGui::SameLine(0.0F, 8.0F * dpiScale);
        if (ImGui::Button(T(TextId::Search), ImVec2(searchButtonWidth, 0.0F))) {
            const std::string query(friendSearchBuffer_.data());
            if (!platformActionPending_) {
                platformActionPending_ = true;
                if (!worker_.Submit([this, query] {
                    std::string error;
                    auto results = platform_.SearchUsers(query, error);
                    return BackgroundWorker::Completion([this, results = std::move(results), error]() mutable {
                        platformActionPending_ = false;
                        if (!error.empty()) { uiMessage_ = error; return; }
                        friendSearchResults_ = std::move(results);
                    });
                })) platformActionPending_ = false;
            }
        }
        for (const auto& result : friendSearchResults_) {
            ImGui::PushID(("search-" + result.id).c_str());
            ImGui::Text("%s  @%s", result.nickname.c_str(), result.username.c_str()); ImGui::SameLine();
            if (ImGui::SmallButton(T(TextId::SendFriendRequest))) {
                const std::string username = result.username;
                QueuePlatformAction([this, username](std::string& error) {
                    return platform_.SendFriendRequest(username, error);
                }, "Arkadaslik istegi gonderildi.", false, true, false);
                friendSearchResults_.clear();
            }
            ImGui::PopID();
        }
        }
        if (ImGui::SmallButton(T(TextId::RefreshFriends))) RefreshSocial();
        for (const auto& friendEntry : friends_) {
            if (homeSection_ == HomeSection::Friends && friendEntry.state != "accepted") continue;
            if (homeSection_ == HomeSection::Pending && friendEntry.state != "pending") continue;
            if (homeSection_ == HomeSection::Notifications) continue;
            ImGui::PushID(("friend-" + friendEntry.id).c_str());
            ImGui::Text("%s  @%s", friendEntry.nickname.c_str(), friendEntry.username.c_str());
            if (friendEntry.state == "pending") {
                const bool incoming = friendEntry.requesterId != account.id;
                ImGui::SameLine(); ImGui::TextDisabled(incoming ? "gelen istek" : "bekliyor");
                if (incoming) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(T(TextId::Accept))) {
                        const std::string friendshipId = friendEntry.id;
                        QueuePlatformAction([this, friendshipId](std::string& actionError) {
                            return platform_.AcceptFriendRequest(friendshipId, actionError);
                        }, "Arkadaslik istegi kabul edildi.", false, true, false);
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(incoming ? T(TextId::Reject) : T(TextId::Cancel))) {
                    const std::string friendshipId = friendEntry.id;
                    QueuePlatformAction([this, friendshipId](std::string& actionError) {
                        return platform_.DismissFriendRequest(friendshipId, actionError);
                    }, "Arkadaslik istegi kaldirildi.", false, true, false);
                }
            } else if (friendEntry.state == "accepted") {
                const auto direct = std::find_if(conversations_.begin(), conversations_.end(), [&friendEntry](const PlatformConversation& item) {
                    return item.kind == "direct" && item.directPeerId == friendEntry.id;
                });
                if (direct != conversations_.end() && direct->unreadCount > 0) {
                    ImGui::SameLine(); ImGui::TextColored(ImVec4(0.30F, 0.85F, 0.65F, 1.0F), "%u yeni", direct->unreadCount);
                }
                ImGui::SameLine(); if (ImGui::SmallButton("Mesaj")) OpenDirectChat(friendEntry);
                ImGui::SameLine();
                if (ImGui::SmallButton(T(TextId::RemoveFriend))) {
                    const std::string friendshipId = friendEntry.id;
                    QueuePlatformAction([this, friendshipId](std::string& error) {
                        return platform_.RemoveFriend(friendshipId, error);
                    }, "Arkadaslik kaldirildi.", false, true, false);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(T(TextId::Block))) {
                    const std::string userId = friendEntry.id;
                    QueuePlatformAction([this, userId](std::string& error) {
                        return platform_.BlockUser(userId, error);
                    }, "Kullanici engellendi.", false, true, false);
                }
            }
            ImGui::PopID();
        }
        int shown = 0;
        if (homeSection_ == HomeSection::Notifications) for (const auto& notification : notifications_) {
            if (notification.read || shown++ >= 5) continue;
            ImGui::PushID(("notification-" + notification.id).c_str());
            ImGui::Separator(); ImGui::TextUnformatted(notification.title.c_str()); ImGui::TextWrapped("%s", notification.body.c_str());
            if (ImGui::SmallButton(T(TextId::MarkRead))) {
                const std::string notificationId = notification.id;
                QueuePlatformAction([this, notificationId](std::string& error) {
                    return platform_.MarkNotificationRead(notificationId, error);
                }, "Bildirim okundu.", false, true, false);
            }
            ImGui::PopID();
        }
    }

    if (activePage_ == ClientPage::Settings) {
    struct SettingsNavigationEntry final {
        SettingsSection section;
        const char* label;
        horizon::Icon icon;
        const char* group;
    };
    constexpr std::array<SettingsNavigationEntry, 9> settingsSections{{
        {SettingsSection::Account, "Hesabım", horizon::Icon::Friends, "KİŞİSEL"},
        {SettingsSection::Appearance, "Görünüm", horizon::Icon::Settings, "DENEYİM"},
        {SettingsSection::Notifications, "Bildirimler", horizon::Icon::Notification, nullptr},
        {SettingsSection::Audio, "Ses ve konuşma", horizon::Icon::VoiceChannel, nullptr},
        {SettingsSection::Privacy, "Gizlilik ve Guardian", horizon::Icon::Shield, nullptr},
        {SettingsSection::Language, "Dil", horizon::Icon::Connection, nullptr},
        {SettingsSection::Updates, "Güncellemeler", horizon::Icon::Update, "SİSTEM"},
        {SettingsSection::Diagnostics, "Tanılama", horizon::Icon::Relay, nullptr},
        {SettingsSection::About, "Hakkında", horizon::Icon::Community, nullptr},
    }};
    const float settingsMenuWidth = std::max(
        120.0F * dpiScale,
        ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize - 10.0F * dpiScale);
    const bool settingsRtl = IsRightToLeft(language_);
    const auto withAlpha = [](ImVec4 color, const float alpha) noexcept {
        color.w = alpha;
        return color;
    };
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0F * dpiScale, 4.0F * dpiScale));
    for (const SettingsNavigationEntry& entry : settingsSections) {
        if (entry.group != nullptr) {
            if (entry.section != SettingsSection::Account) ImGui::Dummy(ImVec2(0.0F, 5.0F * dpiScale));
            ImGui::PushStyleColor(ImGuiCol_Text, withAlpha(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), 0.82F));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0F * dpiScale);
            ImGui::TextUnformatted(entry.group);
            ImGui::PopStyleColor();
        }

        ImGui::PushID(static_cast<int>(entry.section));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
        const bool selected = settingsSection_ == entry.section;
        const bool pressed = ImGui::Selectable("##settingsNavigationItem", selected, 0,
                                               ImVec2(settingsMenuWidth, 42.0F * dpiScale));
        ImGui::PopStyleColor(3);

        const ImVec2 itemMinimum = ImGui::GetItemRectMin();
        const ImVec2 itemMaximum = ImGui::GetItemRectMax();
        const bool highlighted = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        ImDrawList* const drawList = ImGui::GetWindowDrawList();
        ImVec4 surfaceColor = ImGui::GetStyleColorVec4(ImGuiCol_Button);
        if (selected) {
            surfaceColor = withAlpha(ImGui::GetStyleColorVec4(ImGuiCol_Header), 0.24F);
        } else if (highlighted) {
            surfaceColor = withAlpha(ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered), 0.72F);
        } else {
            surfaceColor = withAlpha(surfaceColor, 0.24F);
        }
        drawList->AddRectFilled(itemMinimum, itemMaximum,
                                ImGui::ColorConvertFloat4ToU32(surfaceColor), 9.0F * dpiScale);

        const ImVec4 accentColor = ImGui::GetStyleColorVec4(
            selected ? ImGuiCol_HeaderHovered : ImGuiCol_TextDisabled);
        if (selected) {
            const float accentX = settingsRtl ? itemMaximum.x - 5.0F * dpiScale
                                              : itemMinimum.x + 2.0F * dpiScale;
            drawList->AddRectFilled(
                ImVec2(accentX, itemMinimum.y + 9.0F * dpiScale),
                ImVec2(accentX + 3.0F * dpiScale, itemMaximum.y - 9.0F * dpiScale),
                ImGui::ColorConvertFloat4ToU32(accentColor), 2.0F * dpiScale);
        }

        const float iconCenterX = settingsRtl ? itemMaximum.x - 24.0F * dpiScale
                                              : itemMinimum.x + 24.0F * dpiScale;
        const ImVec2 iconCenter(iconCenterX, (itemMinimum.y + itemMaximum.y) * 0.5F);
        ImVec4 iconSurface = selected
            ? withAlpha(ImGui::GetStyleColorVec4(ImGuiCol_Header), 0.32F)
            : withAlpha(ImGui::GetStyleColorVec4(ImGuiCol_FrameBg), 0.72F);
        drawList->AddCircleFilled(iconCenter, 14.0F * dpiScale,
                                  ImGui::ColorConvertFloat4ToU32(iconSurface), 18);
        horizon::DrawIcon(drawList, entry.icon, iconCenter, 16.0F * dpiScale,
                          ImGui::ColorConvertFloat4ToU32(accentColor));

        const float labelMaximumWidth = settingsMenuWidth - 70.0F * dpiScale;
        const std::string fittedLabel = FitTextToWidth(entry.label, labelMaximumWidth);
        const ImVec2 labelSize = ImGui::CalcTextSize(fittedLabel.c_str());
        const float labelX = settingsRtl
            ? itemMaximum.x - 46.0F * dpiScale - labelSize.x
            : itemMinimum.x + 48.0F * dpiScale;
        drawList->AddText(
            ImVec2(labelX, itemMinimum.y + (itemMaximum.y - itemMinimum.y - labelSize.y) * 0.5F),
            ImGui::GetColorU32(selected ? ImGuiCol_Text : ImGuiCol_TextDisabled),
            fittedLabel.c_str());

        if (pressed) settingsSection_ = entry.section;
        ImGui::PopID();
    }
    ImGui::PopStyleVar();

    ImGui::Dummy(ImVec2(0.0F, 7.0F * dpiScale));
    ImVec4 hintSurface = withAlpha(ImGui::GetStyleColorVec4(ImGuiCol_FrameBg), 0.58F);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, hintSurface);
    ImGui::PushStyleColor(ImGuiCol_Border, withAlpha(ImGui::GetStyleColorVec4(ImGuiCol_Border), 0.72F));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 9.0F * dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0F * dpiScale, 10.0F * dpiScale));
    if (ImGui::BeginChild("settingsNavigationHint", ImVec2(settingsMenuWidth, 68.0F * dpiScale),
                          ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const ImVec2 iconCenter(ImGui::GetCursorScreenPos().x + 12.0F * dpiScale,
                                ImGui::GetCursorScreenPos().y + 13.0F * dpiScale);
        horizon::DrawIcon(ImGui::GetWindowDrawList(), horizon::Icon::Settings, iconCenter,
                          17.0F * dpiScale, ImGui::GetColorU32(ImGuiCol_HeaderHovered));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 32.0F * dpiScale);
        ImGui::TextUnformatted("Hızlı ayarlar");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 32.0F * dpiScale);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + settingsMenuWidth - 58.0F * dpiScale);
        ImGui::TextDisabled("Seçtiğin bölüm sağdaki çalışma alanında açılır.");
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (settingsSection_ == SettingsSection::Audio || settingsSection_ == SettingsSection::Language) {
    ImGui::Spacing();
    SectionTitle(settingsSection_ == SettingsSection::Language ? T(TextId::LanguageLabel)
                                                                : T(TextId::AudioDevices), nullptr);
    FieldLabel(T(TextId::LanguageLabel));
    const auto& languages = SupportedLanguages();
    const auto currentLanguage = std::find_if(languages.begin(), languages.end(), [this](const LanguageOption& option) {
        return option.language == language_;
    });
    const char* languagePreview = currentLanguage == languages.end() ? "English" : LanguageDisplayName(currentLanguage->language);
    if (ImGui::BeginCombo("##settingsLanguage", languagePreview)) {
        for (const auto& option : languages) {
            const bool selected = option.language == language_;
            if (ImGui::Selectable(LanguageDisplayName(option.language), selected)) {
                language_ = option.language;
                settings_.language = std::string(option.code);
                SaveSettings();
                uiMessage_ = T(TextId::LanguageChanged);
                fontReloadRequested_.store(true);
                if (redrawEvent_ != nullptr) SetEvent(redrawEvent_);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::Spacing();
    bool audioDeviceChanged = false;
    if (DeviceCombo(T(TextId::Microphone), inputDevices_, inputIndex_, !audioRestartPending_)) {
        settings_.inputDeviceId = inputDevices_[static_cast<std::size_t>(inputIndex_)].id;
        audioDeviceChanged = true;
    }
    if (DeviceCombo(T(TextId::Output), outputDevices_, outputIndex_, !audioRestartPending_)) {
        settings_.outputDeviceId = outputDevices_[static_cast<std::size_t>(outputIndex_)].id;
        audioDeviceChanged = true;
    }
    if (inputDevices_.size() <= 1U) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0F, 0.62F, 0.24F, 1.0F),
            "Mikrofon algilanmadi. Uzak Masaustu baglantisinda Yerel Kaynaklar > Uzak ses > Bu bilgisayardan kaydet secenegini acip yeniden baglanin.");
        ImGui::PopTextWrapPos();
    }
    if (audioDeviceChanged) {
        SaveSettings();
        RestartAudioDevices();
    }
    int outputPercent = static_cast<int>(std::lround(settings_.outputVolume * 100.0F));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##masterVolume", &outputPercent, 0, 200, "Genel ses %d%%")) {
        settings_.outputVolume = static_cast<float>(outputPercent) / 100.0F;
        audio_.SetMasterOutputVolume(settings_.outputVolume);
        SaveSettings();
    }
    if (ImGui::SmallButton(T(TextId::RefreshDevices))) {
        RefreshDevices();
        settings_.inputDeviceId = inputDevices_[static_cast<std::size_t>(inputIndex_)].id;
        settings_.outputDeviceId = outputDevices_[static_cast<std::size_t>(outputIndex_)].id;
        SaveSettings();
        RestartAudioDevices();
    }
    if (connectedOrConnecting) {
        ImGui::SameLine();
    ImGui::TextDisabled("%s", T(TextId::LiveDeviceChanges));
    }

    ImGui::Spacing();
    SectionTitle(T(TextId::SmartTransmission), nullptr);
    int transmitMode = settings_.pushToTalk ? 1 : (settings_.voiceActivation ? 0 : 2);
    const char* transmitModes[] = {T(TextId::AutomaticVad), T(TextId::PushToTalk), T(TextId::ContinuousTransmission)};
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##transmitMode", &transmitMode, transmitModes, IM_ARRAYSIZE(transmitModes))) {
        settings_.pushToTalk = transmitMode == 1;
        settings_.voiceActivation = transmitMode == 0;
        audio_.SetPushToTalk(settings_.pushToTalk);
        audio_.SetVoiceActivation(settings_.voiceActivation);
        SaveSettings();
    }
    if (settings_.pushToTalk) {
        constexpr int keys[] = {'V', 'C', VK_CAPITAL, VK_XBUTTON1, VK_XBUTTON2, VK_CONTROL};
        constexpr const char* names[] = {"V", "C", "Caps Lock", "Mouse 4", "Mouse 5", "Ctrl"};
        int selectedKey = 0;
        for (int index = 0; index < IM_ARRAYSIZE(keys); ++index) {
            if (keys[index] == settings_.pushToTalkVirtualKey) selectedKey = index;
        }
        if (ImGui::Combo("Bas-konuş tuşu", &selectedKey, names, IM_ARRAYSIZE(names))) {
            settings_.pushToTalkVirtualKey = keys[selectedKey];
            audio_.SetPushToTalkKey(settings_.pushToTalkVirtualKey);
            SaveSettings();
        }
    }
    if (!settings_.voiceActivation || settings_.pushToTalk) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##sensitivity", &settings_.vadSensitivity, 0.0F, 1.0F,
                           (std::string(T(TextId::Sensitivity)) + " %.2f").c_str())) {
        audio_.SetVadSensitivity(settings_.vadSensitivity);
        SaveSettings();
    }
    if (!settings_.voiceActivation || settings_.pushToTalk) ImGui::EndDisabled();
    ImGui::TextDisabled(settings_.pushToTalk ? "Seçtiğiniz bas-konuş tuşunu basılı tutun." : "Sessizlikte ses yüklemesi durur; eşik ortama uyarlanır.");
    if (!settings_.voiceActivation && !settings_.pushToTalk) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.68F, 0.28F, 1.0F));
        ImGui::TextWrapped("%s", T(TextId::ContinuousWarning));
        ImGui::PopStyleColor();
        if (ImGui::SmallButton(T(TextId::SwitchToAutomaticVad))) {
            settings_.voiceActivation = true;
            settings_.pushToTalk = false;
            audio_.SetVoiceActivation(true);
            audio_.SetPushToTalk(false);
            SaveSettings();
        }
    }
    }
    }

    ImGui::EndChild();
    if ((activePage_ == ClientPage::Voice || activePage_ == ClientPage::Rooms) && !connectedOrConnecting) {
        if (connectPending_) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        const bool connectPressed = ImGui::Button(connectPending_ ? T(TextId::Connecting) : T(TextId::Connect),
                                                  ImVec2(-FLT_MIN, 34.0F * dpiScale));
        ImGui::PopStyleColor(3);
        if (connectPressed) Connect();
        if (connectPending_) ImGui::EndDisabled();
    } else if ((activePage_ == ClientPage::Voice || activePage_ == ClientPage::Rooms)
               && ImGui::Button(T(TextId::Disconnect), ImVec2(-FLT_MIN, 34.0F * dpiScale))) {
        Disconnect();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    }

    ImGui::SameLine(0.0F, gap);
    const bool showWideMemberPanel = showMemberPanel_ && memberPanelContext
        && horizonLayout_.memberPanel.IsInline();
    const float wideMemberWidth = horizonLayout_.memberPanel.bounds.width * dpiScale;
    const float roomContentWidth = showWideMemberPanel
        ? std::max(horizonLayout_.minimumContentWidth * dpiScale,
                   ImGui::GetContentRegionAvail().x - wideMemberWidth - gap) : 0.0F;
    const ImVec4 canvasSurface = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const ImVec4 raisedSurface = ImGui::GetStyleColorVec4(ImGuiCol_ChildBg);
    ImVec4 sceneSurface(
        canvasSurface.x * 0.82F + raisedSurface.x * 0.18F,
        canvasSurface.y * 0.82F + raisedSurface.y * 0.18F,
        canvasSurface.z * 0.82F + raisedSurface.z * 0.18F,
        1.0F);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, sceneSurface);
    ImGui::BeginChild("room", ImVec2(roomContentWidth, -1.0F),
                      ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);

    const char* pageTitle = activePage_ == ClientPage::Home ? "Ana sayfa"
        : activePage_ == ClientPage::Voice ? T(TextId::Voice)
        : activePage_ == ClientPage::Rooms ? T(TextId::Rooms)
        : activePage_ == ClientPage::Messages ? T(TextId::Messages) : T(TextId::Settings);
    std::string sceneTitle = pageTitle;
    std::string sceneSubtitle;
    if (activePage_ == ClientPage::Home) {
        sceneSubtitle = "Arkadaşlar, özel mesajlar ve bildirimler";
    } else if (activePage_ == ClientPage::Settings) {
        sceneSubtitle = "Kişisel deneyim, ses, gizlilik ve güncelleme tercihleri";
    } else if (!directConversationId_.empty()) {
        sceneTitle = directConversationLabel_;
        sceneSubtitle = "Özel ve şifreli konuşma";
    } else if (roomOverview_) {
        const auto selectedHeaderChannel = std::find_if(roomOverview_->channels.begin(), roomOverview_->channels.end(),
            [this](const PlatformChannel& channel) { return channel.id == selectedChannelId_; });
        if (selectedHeaderChannel != roomOverview_->channels.end()) {
            sceneTitle = selectedHeaderChannel->type == "voice" ? "Ses  " : "#  ";
            sceneTitle += selectedHeaderChannel->name;
            sceneSubtitle = selectedHeaderChannel->topic.empty() ? roomOverview_->name : selectedHeaderChannel->topic;
        } else {
            sceneSubtitle = roomOverview_->name;
        }
    } else {
        sceneSubtitle = "Sonalis çalışma alanı";
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    const float sceneHeaderHeight = horizonLayout_.headerHeight * dpiScale;
    ImGui::BeginChild("sceneHeader", ImVec2(0.0F, sceneHeaderHeight), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const bool channelDrawerAvailable = horizonLayout_.channelPanel.IsDrawer();
    const float headerControlSize = 32.0F * dpiScale;
    const char* panelLabel = showChannelPanel_ ? "Paneli kapat"
        : activePage_ == ClientPage::Settings ? "Ayar menüsü"
        : activePage_ == ClientPage::Messages ? "Konuşmalar"
        : activePage_ == ClientPage::Home ? "Ana menü" : "Kanallar";
    const float channelButtonWidth = channelDrawerAvailable
        ? std::max(112.0F * dpiScale,
                   ImGui::CalcTextSize(panelLabel).x + 48.0F * dpiScale)
        : 0.0F;
    const float memberButtonWidth = memberPanelContext ? headerControlSize : 0.0F;
    const float statusWidth = std::max(74.0F * dpiScale, ImGui::CalcTextSize(status.c_str()).x + 8.0F * dpiScale);
    const float headerControlsWidth = channelButtonWidth + memberButtonWidth + statusWidth
        + (channelDrawerAvailable ? gap : 0.0F)
        + (memberPanelContext ? gap : 0.0F);
    const float controlsX = std::max(220.0F * dpiScale,
        ImGui::GetWindowWidth() - headerControlsWidth - 14.0F * dpiScale);
    const float headerTextWidth = std::max(40.0F * dpiScale, controlsX - 32.0F * dpiScale);
    const std::string fittedSceneTitle = FitTextToWidth(sceneTitle, headerTextWidth);
    const std::string fittedSceneSubtitle = FitTextToWidth(sceneSubtitle, headerTextWidth);
    const float lineHeight = ImGui::GetTextLineHeight();
    const float titleY = std::max(5.0F * dpiScale,
        (sceneHeaderHeight - lineHeight * 2.0F - 2.0F * dpiScale) * 0.5F);
    ImGui::SetCursorPos(ImVec2(16.0F * dpiScale, titleY));
    ImGui::TextUnformatted(fittedSceneTitle.c_str());
    ImGui::SetCursorPos(ImVec2(16.0F * dpiScale, titleY + lineHeight + 2.0F * dpiScale));
    ImGui::TextDisabled("%s", fittedSceneSubtitle.c_str());
    ImGui::SetCursorPos(ImVec2(controlsX,
        std::max(0.0F, (sceneHeaderHeight - headerControlSize) * 0.5F)));
    if (channelDrawerAvailable) {
        if (LabeledIconButton("headerChannelPanel", panelLabel, horizon::Icon::Community,
                              ImVec2(channelButtonWidth, headerControlSize),
                              showChannelPanel_)) {
            showChannelPanel_ = !showChannelPanel_;
            if (showChannelPanel_ && horizonLayout_.memberPanel.IsDrawer()) showMemberPanel_ = false;
        }
        ImGui::SameLine(0.0F, gap);
    }
    ImGui::TextColored(statusColor, "%s", status.c_str());
    if (memberPanelContext) {
        ImGui::SameLine(0.0F, gap);
        horizon::IconButtonOptions memberOptions{};
        memberOptions.size = ImVec2(memberButtonWidth, headerControlSize);
        memberOptions.iconSize = 19.0F * dpiScale;
        memberOptions.rounding = 6.0F * dpiScale;
        memberOptions.selected = showMemberPanel_;
        memberOptions.tooltip = showMemberPanel_ ? "Üye listesini gizle" : "Üye listesini göster";
        if (horizon::IconButton("headerMembers", horizon::Icon::Friends, memberOptions)) {
            showMemberPanel_ = !showMemberPanel_;
            if (showMemberPanel_ && horizonLayout_.channelPanel.IsDrawer()) showChannelPanel_ = false;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();
    if (activePage_ == ClientPage::Settings) {
        SectionTitle(T(TextId::ApplicationSettings), "Sonalis Horizon");
        ImGui::TextWrapped("Ayarlar kategorilere ayrılmıştır. Sol panelden bir bölüm seçerek yalnız ilgili seçenekleri görüntüleyin.");
        ImGui::Spacing();
        if (settingsSection_ == SettingsSection::Appearance) {
        ImGui::SeparatorText("AURORA GÖRÜNÜMÜ");
        const char* themeNames[] = {
            "Horizon Graphite", "Aurora Gece (önerilen)", "Aurora Gündüz", "OLED Saf Siyah", "Özel Horizon"
        };
        int selectedTheme = static_cast<int>(settings_.uiTheme);
        ImGui::SetNextItemWidth(280.0F);
        if (!experiencePolicy_.allowUserThemeChoice) ImGui::BeginDisabled();
        if (ImGui::Combo("Tema", &selectedTheme, themeNames, IM_ARRAYSIZE(themeNames))) {
            settings_.uiTheme = static_cast<UiTheme>(std::clamp(selectedTheme, 0, 4));
            settings_.auroraPreviewShown = true;
            SaveSettings();
            fontReloadRequested_.store(true);
            if (redrawEvent_ != nullptr) SetEvent(redrawEvent_);
        }
        if (!experiencePolicy_.allowUserThemeChoice) ImGui::EndDisabled();
        const char* themeDescription = settings_.uiTheme == UiTheme::AuroraDark
            ? "Modern topluluk görünümü; koyu yüzeyler ve mor-mavi Aurora vurguları."
            : settings_.uiTheme == UiTheme::AuroraLight
            ? "Aydınlık ortamlar için yüksek okunabilirlikli modern görünüm."
            : settings_.uiTheme == UiTheme::Oled
            ? "OLED ekranlarda düşük ışık ve saf siyah arka plan."
            : settings_.uiTheme == UiTheme::Custom
            ? "Sonalis düzenini kendi vurgu renginizle kullanın."
            : "Katmanlı grafit yüzeyler ve dengeli indigo vurgularla modern, sakin görünüm.";
        ImGui::TextDisabled("%s", themeDescription);
        const char* profileNames[] = {"Sonalis Lite", "Sonalis Balanced", "Sonalis Studio"};
        int selectedProfile = static_cast<int>(settings_.resourceProfile);
        ImGui::SetNextItemWidth(280.0F);
        if (!experiencePolicy_.allowUserResourceProfileChoice) ImGui::BeginDisabled();
        if (ImGui::Combo("Kaynak profili", &selectedProfile, profileNames, IM_ARRAYSIZE(profileNames))) {
            settings_.resourceProfile = static_cast<ResourceProfile>(std::clamp(selectedProfile, 0, 2));
            SaveSettings();
            fontReloadRequested_.store(true);
            if (redrawEvent_ != nullptr) SetEvent(redrawEvent_);
        }
        if (!experiencePolicy_.allowUserResourceProfileChoice) ImGui::EndDisabled();
        const ResourceBudget resourceBudget = BudgetFor(settings_.resourceProfile);
        ImGui::TextDisabled("Hedef bellek: %zu MB · uyarı: %zu MB · görsel önbellek: %zu MB",
                            resourceBudget.workingSetTargetBytes / (1024U * 1024U),
                            resourceBudget.workingSetWarningBytes / (1024U * 1024U),
                            resourceBudget.imageCacheBytes / (1024U * 1024U));
        ImGui::TextDisabled("Mesaj belleği: en fazla %zu ileti · ses kalitesi profilden etkilenmez",
                            resourceBudget.maximumResolvedMessages);
        const char* densityNames[] = {"Sıkı", "Normal", "Rahat"};
        ImGui::SetNextItemWidth(280.0F);
        if (ImGui::Combo("Arayüz yoğunluğu", &settings_.uiDensity,
                         densityNames, IM_ARRAYSIZE(densityNames))) {
            SaveSettings();
            fontReloadRequested_.store(true);
            if (redrawEvent_ != nullptr) SetEvent(redrawEvent_);
        }
        ImGui::SetNextItemWidth(280.0F);
        if (ImGui::SliderFloat("Yazı ölçeği", &settings_.textScale, 0.85F, 1.35F, "x%.2f")) {
            settings_.textScale = std::clamp(settings_.textScale, 0.85F, 1.35F);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            SaveSettings();
            fontReloadRequested_.store(true);
            if (redrawEvent_ != nullptr) SetEvent(redrawEvent_);
        }
        if (ImGui::Checkbox("Yüksek kontrast", &settings_.highContrast)) {
            SaveSettings();
            fontReloadRequested_.store(true);
            if (redrawEvent_ != nullptr) SetEvent(redrawEvent_);
        }
        const char* colorModes[] = {"Standart", "Protanopi desteği", "Döteranopi desteği", "Tritanopi desteği"};
        ImGui::SetNextItemWidth(280.0F);
        if (ImGui::Combo("Renk görüşü", &settings_.colorVisionMode,
                         colorModes, IM_ARRAYSIZE(colorModes))) {
            SaveSettings();
            fontReloadRequested_.store(true);
            if (redrawEvent_ != nullptr) SetEvent(redrawEvent_);
        }
        if (settings_.uiTheme == UiTheme::Custom) {
            if (!experiencePolicy_.customAccentsEnabled) ImGui::BeginDisabled();
            float accent[3]{settings_.customAccentR, settings_.customAccentG, settings_.customAccentB};
            if (ImGui::ColorEdit3("Vurgu rengi", accent,
                                  ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_DisplayRGB)) {
                settings_.customAccentR = accent[0];
                settings_.customAccentG = accent[1];
                settings_.customAccentB = accent[2];
                SaveSettings();
                fontReloadRequested_.store(true);
                if (redrawEvent_ != nullptr) SetEvent(redrawEvent_);
            }
            if (!experiencePolicy_.customAccentsEnabled) ImGui::EndDisabled();
        }
        ImGui::SeparatorText("PROFİL KARŞILAŞTIRMASI");
        ImGui::TextWrapped("Lite: en düşük RAM/CPU ve azaltılmış görsel efektler. Balanced: günlük kullanım için önerilen denge. Studio: geniş/16K ekranlarda daha zengin paneller ve görsel önbellek.");
        }
        if (settingsSection_ == SettingsSection::Privacy) {
        ImGui::SeparatorText("GÜVENLİ MEDYA");
        const char* sensitiveModes[] = {"Engelle", "Bulanık göster", "Göster (yalnız 18+)"};
        int sensitiveMode = static_cast<int>(mediaPreferences_.sensitiveMediaMode);
        if (mediaPreferences_.lockedForMinor || guardianPreferenceUpdatePending_) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(280.0F);
        if (ImGui::Combo("Hassas medya", &sensitiveMode,
                         sensitiveModes, IM_ARRAYSIZE(sensitiveModes))) {
            mediaPreferences_.sensitiveMediaMode =
                static_cast<SensitiveMediaMode>(std::clamp(sensitiveMode, 0, 2));
            UpdateGuardianPreferences();
        }
        if (ImGui::Checkbox("Arkadaş olmayan kişilerden medya kabul et",
                            &mediaPreferences_.allowNonFriendMedia)) {
            UpdateGuardianPreferences();
        }
        if (mediaPreferences_.lockedForMinor || guardianPreferenceUpdatePending_) ImGui::EndDisabled();
        if (mediaPreferences_.lockedForMinor) {
            ImGui::TextColored(ImVec4(1.0F, 0.72F, 0.25F, 1.0F),
                               "Bu hesapta hassas medya gösterimi yaş güvenliği nedeniyle kilitli.");
        }
        if (!mediaSafetyConfig_ || !mediaSafetyConfig_->privateMediaReportingAvailable) {
            ImGui::TextDisabled("Özel medya raporlama anahtarı henüz yapılandırılmadı; public tarama fail-closed kalır.");
        }
        ImGui::TextDisabled("Public avatar, banner, CMS ve reklam görsellerinde yetişkin içerik her zaman engellenir.");
        ImGui::SeparatorText("HESAP KISITLAMALARI VE İTİRAZ");
        if (guardianRefreshPending_) ImGui::TextDisabled("Guardian durumu yenileniyor...");
        if (restrictions_.empty() && !guardianRefreshPending_) {
            ImGui::TextDisabled("Etkin hesap kısıtlaması yok.");
        }
        bool openRestrictionAppeal = false;
        for (const auto& restriction : restrictions_) {
            ImGui::PushID(restriction.id.c_str());
            ImGui::Text("%s", restriction.scope.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", restriction.expiresAt.empty()
                ? "süresiz" : restriction.expiresAt.c_str());
            ImGui::TextWrapped("Sebep: %s", restriction.reasonCode.c_str());
            if (ImGui::SmallButton("İtiraz et")) {
                appealRestrictionId_ = restriction.id;
                restrictionAppealBuffer_.fill('\0');
                openRestrictionAppeal = true;
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        if (openRestrictionAppeal) ImGui::OpenPopup("restrictionAppeal");
        if (ImGui::SmallButton("Guardian durumunu yenile")) RefreshGuardian();
        ImGui::SetNextWindowSize(ImVec2(540.0F * dpiScale, 300.0F * dpiScale), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("restrictionAppeal", nullptr, ImGuiWindowFlags_NoResize)) {
            ImGui::TextWrapped("Kararın neden hatalı olduğunu açıklayın. İlk kararı veren yönetici bu itirazı inceleyemez.");
            ImGui::InputTextMultiline("##appeal", restrictionAppealBuffer_.data(),
                                      restrictionAppealBuffer_.size(), ImVec2(-FLT_MIN, 150.0F * dpiScale));
            const std::string statement(restrictionAppealBuffer_.data());
            if (statement.size() < 20U || platformActionPending_) ImGui::BeginDisabled();
            if (ImGui::Button("İtirazı gönder", ImVec2(190.0F * dpiScale, 34.0F * dpiScale))) {
                const std::string restrictionId = appealRestrictionId_;
                QueuePlatformAction([this, restrictionId, statement](std::string& error) {
                    return platform_.AppealRestriction(restrictionId, statement, error);
                }, "İtirazınız bağımsız incelemeye gönderildi.", false, false, false, false,
                [this] { RefreshGuardian(); });
                ImGui::CloseCurrentPopup();
            }
            if (statement.size() < 20U || platformActionPending_) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Vazgeç", ImVec2(120.0F * dpiScale, 34.0F * dpiScale))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::Spacing();
        }
        if (settingsSection_ == SettingsSection::Diagnostics) {
    ImGui::SeparatorText(T(TextId::RuntimeStatus));
        ImGui::Text("Gercek zamanli mesaj: %s", realtime_.IsConnected() ? "bagli" : "yeniden baglaniyor");
        ImGui::Text("Ses oturumu: %s", network_.StatusText().c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Arayuz olay odaklidir: aktif ses 15 FPS, odak disi 4 FPS, minimize durumda 0 FPS.");
        ImGui::TextWrapped("Pencerenin carpisina basmak Sonalis'i bildirim alanina indirir; ses ve mesaj baglantilari arka planda surer.");
        ImGui::TextWrapped("Tanilama metrikleri sabit boyutlu bellek halkasinda tutulur; otomatik olarak diske yazilmaz.");
        if (processMemory_.available) {
            const ResourceBudget budget = BudgetFor(settings_.resourceProfile);
            ImGui::Text("Calisma kumesi: %llu MB  |  ozel bellek: %llu MB  |  profil hedefi: %zu MB",
                        static_cast<unsigned long long>(processMemory_.workingSetBytes / (1024U * 1024U)),
                        static_cast<unsigned long long>(processMemory_.privateBytes / (1024U * 1024U)),
                        budget.workingSetTargetBytes / (1024U * 1024U));
            if (memoryBudgetWarning_) {
                ImGui::TextColored(ImVec4(1.0F, 0.68F, 0.24F, 1.0F),
                                   "Bellek profil uyarisi: zorla bosaltma yapilmadi; ses hatti korunuyor.");
            }
        }
        ImGui::Text("Guvenli hata raporlama: Etkin  |  bekleyen %zu",
                    PendingDiagnosticErrorCount());
        ImGui::TextWrapped("Yalniz hata kodu, bilesen, surum ve kirpilmis teknik baglam merkeze gonderilir. Parola, token, mesaj, anahtar, kullanici adi ve tam IP gonderilmez.");
        ImGui::PopStyleColor();
        }
        if (settingsSection_ == SettingsSection::Notifications) {
        ImGui::SeparatorText("BİLDİRİM GİZLİLİĞİ");
        const char* previewModes[] = {"İçeriği göster", "Yalnız gönderen", "Tamamen gizle"};
        ImGui::SetNextItemWidth(280.0F);
        if (ImGui::Combo("##notificationPreview", &settings_.notificationPreview,
                         previewModes, IM_ARRAYSIZE(previewModes))) SaveSettings();
        ImGui::TextDisabled("Windows bildirimleri yalnız uygulama arka plandayken DM, mention ve önemli oda olayları için gösterilir.");
        }
        if (settingsSection_ == SettingsSection::Diagnostics) {
        const AudioDiagnosticsSnapshot liveAudio = audio_.Diagnostics();
        const NetworkDiagnosticsSnapshot liveNetwork = network_.Diagnostics();
        ImGui::SeparatorText("VOICE PIPELINE");
        ImGui::TextDisabled("captured %llu  -> encoded %llu  -> sent %llu  -> received %llu  -> decoded %llu  -> rendered %llu",
            static_cast<unsigned long long>(liveAudio.capturedPcmFrames),
            static_cast<unsigned long long>(liveAudio.opusEncodeSuccess),
            static_cast<unsigned long long>(liveNetwork.udpAudioSent),
            static_cast<unsigned long long>(liveNetwork.udpAudioReceived),
            static_cast<unsigned long long>(liveAudio.decodedOpusFrames),
            static_cast<unsigned long long>(liveAudio.wasapiRenderFrames));
        ImGui::TextDisabled("VAD accepted %llu  |  VAD rejected %llu",
            static_cast<unsigned long long>(liveAudio.vadAcceptedFrames),
            static_cast<unsigned long long>(liveAudio.vadRejectedFrames));
        ImGui::TextDisabled("jitter %.1f ms  |  loss %.1f%%  |  FEC %llu  |  PLC %llu  |  decrypt reject %llu",
            static_cast<double>(liveAudio.jitterMs),
            static_cast<double>(liveAudio.packetLossPercent),
            static_cast<unsigned long long>(liveAudio.fecRecoveries),
            static_cast<unsigned long long>(liveAudio.plcFrames),
            static_cast<unsigned long long>(liveNetwork.udpDecryptRejected));
        ImGui::TextDisabled("capture age %llu ms  |  receive age %llu ms",
            static_cast<unsigned long long>(liveAudio.lastCaptureAgeMs),
            static_cast<unsigned long long>(liveAudio.lastReceiveAgeMs));
    if (ImGui::Button(T(TextId::ExportDiagnostics))) {
            const AudioDiagnosticsSnapshot audioDiagnostics = audio_.Diagnostics();
            const NetworkDiagnosticsSnapshot networkDiagnostics = network_.Diagnostics();
            char audioSummary[320]{};
            std::snprintf(audioSummary, sizeof(audioSummary),
                          "pcm=%llu opus_ok=%llu opus_error=%llu decode=%llu render=%llu fec=%llu plc=%llu jitter_ms=%.1f loss_percent=%.1f capture_age_ms=%llu receive_age_ms=%llu",
                          static_cast<unsigned long long>(audioDiagnostics.capturedPcmFrames),
                          static_cast<unsigned long long>(audioDiagnostics.opusEncodeSuccess),
                          static_cast<unsigned long long>(audioDiagnostics.opusEncodeErrors),
                          static_cast<unsigned long long>(audioDiagnostics.decodedOpusFrames),
                          static_cast<unsigned long long>(audioDiagnostics.wasapiRenderFrames),
                          static_cast<unsigned long long>(audioDiagnostics.fecRecoveries),
                          static_cast<unsigned long long>(audioDiagnostics.plcFrames),
                          static_cast<double>(audioDiagnostics.jitterMs),
                          static_cast<double>(audioDiagnostics.packetLossPercent),
                          static_cast<unsigned long long>(audioDiagnostics.lastCaptureAgeMs),
                          static_cast<unsigned long long>(audioDiagnostics.lastReceiveAgeMs));
            DiagnosticLog("audio.counters", audioSummary);
            char networkSummary[224]{};
            std::snprintf(networkSummary, sizeof(networkSummary),
                          "udp_sent=%llu udp_rejected=%llu udp_received=%llu decrypt_rejected=%llu",
                          static_cast<unsigned long long>(networkDiagnostics.udpAudioSent),
                          static_cast<unsigned long long>(networkDiagnostics.udpAudioRejected),
                          static_cast<unsigned long long>(networkDiagnostics.udpAudioReceived),
                          static_cast<unsigned long long>(networkDiagnostics.udpDecryptRejected));
            DiagnosticLog("network.counters", networkSummary);
            const std::wstring path = DefaultDiagnosticsPath();
            uiMessage_ = !path.empty() && ExportDiagnostics(path)
                ? "Tanilama raporu LocalAppData/Sonalis/diagnostics.txt konumuna yazildi."
                : "Tanilama raporu yazilamadi.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Sonalis'ten cik")) {
            PostMessageW(windowHandle_, kMessageExitApplication, 0, 0);
        }
        }
        if (settingsSection_ == SettingsSection::About) {
        ImGui::Spacing();
        ImGui::SeparatorText(T(TextId::About));
        ImGui::TextUnformatted("Sonalis " SONALIS_VERSION);
        ImGui::TextDisabled("TLS 1.3 sinyalizasyon  |  ChaCha20-Poly1305 UDP");
        ImGui::TextDisabled("Opus 48 kHz mono  |  20 ms ses paketleri");
        ImGui::TextDisabled("Native C++20 · Win32 · ImGui/DX11 · WASAPI");
        }
        if (settingsSection_ == SettingsSection::Account) {
            if (!accountSecurityLoaded_ && !accountSecurityRefreshPending_) RefreshAccountSecurity();
            ImGui::SeparatorText("HESABIM");
            ImGui::Text("%s", account.nickname.c_str());
            ImGui::TextDisabled("@%s", account.username.c_str());
            ImGui::Spacing();
            ImGui::TextWrapped("Oturum ve güvenlik cihaz bazında korunur. Parolanız Windows istemcisinde saklanmaz.");
            if (ImGui::Button("Web hesabını aç")) {
                DynamicShellExecute(Utf8ToWide(platform_.Origin() + "/account"));
            }
            ImGui::SameLine();
            if (ImGui::Button(accountSecurityRefreshPending_ ? "Yenileniyor..." : "Cihazları yenile")) {
                RefreshAccountSecurity();
            }
            ImGui::Spacing();
            ImGui::SeparatorText("AKTİF OTURUMLAR");
            if (accountSecurityRefreshPending_ && accountSessions_.empty()) ImGui::TextDisabled("Oturumlar alınıyor...");
            for (const auto& session : accountSessions_) {
                ImGui::PushID(("session-" + session.id).c_str());
                ImGui::TextUnformatted(session.deviceName.empty() ? "Bilinmeyen cihaz" : session.deviceName.c_str());
                ImGui::TextDisabled("Son kullanım: %s", session.lastSeenAt.empty() ? "bilinmiyor" : session.lastSeenAt.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Oturumu kapat")) {
                    const std::string sessionId = session.id;
                    QueuePlatformAction([this, sessionId](std::string& error) {
                        return platform_.RevokeSession(sessionId, error);
                    }, "Oturum kapatıldı.", false, false, false, false,
                    [this] { accountSecurityLoaded_ = false; RefreshAccountSecurity(); });
                }
                ImGui::Separator();
                ImGui::PopID();
            }
            ImGui::SeparatorText("YETKİLİ NATIVE CİHAZLAR");
            for (const auto& device : accountDevices_) {
                ImGui::PushID(("device-" + device.id).c_str());
                ImGui::Text("%s · Client %s", device.platform.empty() ? "windows" : device.platform.c_str(),
                            device.clientVersion.empty() ? "bilinmiyor" : device.clientVersion.c_str());
                ImGui::TextDisabled("%u etkin oturum · son kullanım %s", device.activeSessions,
                                    device.lastSeenAt.empty() ? "bilinmiyor" : device.lastSeenAt.c_str());
                if (ImGui::SmallButton("Cihaz yetkisini kaldır")) {
                    const std::string deviceId = device.id;
                    QueuePlatformAction([this, deviceId](std::string& error) {
                        return platform_.RevokeClientDevice(deviceId, error);
                    }, "Cihaz ve bağlı oturumları iptal edildi.", false, false, false, false,
                    [this] { accountSecurityLoaded_ = false; RefreshAccountSecurity(); });
                }
                ImGui::Separator();
                ImGui::PopID();
            }
        }
        if (settingsSection_ == SettingsSection::Updates) {
            ImGui::SeparatorText("GÜNCELLEMELER");
            ImGui::Text("Kurulu sürüm: %s", SONALIS_VERSION);
            ImGui::TextDisabled("Kanal: %s", SONALIS_UPDATE_CHANNEL);
            ImGui::TextWrapped("Açılışta bulunan uygun yeni sürüm otomatik kurulmadan Sonalis devam etmez. Periyodik denetimler etkin oturumu kesmez; kurulumu siz başlatırsınız.");
            if (!manualUpdateCheckPending_ && ImGui::Button("Şimdi güncellemeleri denetle")) {
                manualUpdateCheckPending_ = updater_.CheckAsync(settings_.controlOrigin, true);
            }
        }
        if (settingsSection_ == SettingsSection::Audio || settingsSection_ == SettingsSection::Language) {
            ImGui::SeparatorText(settingsSection_ == SettingsSection::Audio ? "SES VE KONUŞMA" : "DİL");
            ImGui::TextWrapped("Bu bölümün hızlı kontrolleri sol panelde bulunur. Değişiklikler bağlı ses oturumunu mümkün olduğunca koruyarak uygulanır.");
        }
    } else {
    if (activePage_ == ClientPage::Home) {
        unsigned acceptedCount = 0;
        unsigned pendingCount = 0;
        unsigned unreadCount = 0;
        for (const auto& friendEntry : friends_) {
            if (friendEntry.state == "accepted") ++acceptedCount;
            else if (friendEntry.state == "pending") ++pendingCount;
        }
        for (const auto& notification : notifications_) if (!notification.read) ++unreadCount;

        SectionTitle(("Tekrar hoş geldin, " + account.nickname).c_str(), "Sonalis merkez görünümü");
        ImGui::TextDisabled("Konuşmalarına dön, bekleyen isteklerini yönet veya bir topluluğa geç.");
        ImGui::Spacing();
        const float overviewGap = 10.0F * dpiScale;
        const float overviewAvailable = ImGui::GetContentRegionAvail().x;
        const unsigned overviewColumns = overviewAvailable >= 760.0F * dpiScale ? 3U
            : (overviewAvailable >= 470.0F * dpiScale ? 2U : 1U);
        const float overviewWidth = std::max(150.0F * dpiScale,
            (overviewAvailable - overviewGap * static_cast<float>(overviewColumns - 1U))
                / static_cast<float>(overviewColumns));
        const auto overviewCard = [this, overviewWidth, dpiScale](const char* id, const char* title, const unsigned count,
                                                       const char* detail, const HomeSection section) {
            ImGui::PushID(id);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
            ImGui::BeginChild("overviewCard", ImVec2(overviewWidth, 92.0F * dpiScale), ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::TextDisabled("%s", title);
            ImGui::Text("%u", count);
            ImGui::TextDisabled("%s", detail);
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) homeSection_ = section;
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopID();
        };
        overviewCard("friends", "ARKADAŞLAR", acceptedCount, "Çevren ve özel mesajlar", HomeSection::Friends);
        if (overviewColumns > 1U) ImGui::SameLine(0.0F, overviewGap);
        overviewCard("pending", "BEKLEYENLER", pendingCount, "Yanıtını bekleyen istekler", HomeSection::Pending);
        if (overviewColumns > 2U) ImGui::SameLine(0.0F, overviewGap);
        overviewCard("notifications", "BİLDİRİMLER", unreadCount, "Okunmamış gelişmeler", HomeSection::Notifications);
        ImGui::Spacing();

        const char* sectionTitle = homeSection_ == HomeSection::Friends ? "Arkadaşların"
            : homeSection_ == HomeSection::Pending ? "Bekleyen istekler" : "Bildirim merkezi";
        ImGui::SeparatorText(sectionTitle);
        if (homeSection_ == HomeSection::Friends) {
            bool any = false;
            for (const auto& friendEntry : friends_) {
                if (friendEntry.state != "accepted") continue;
                any = true;
                ImGui::PushID(("home-friend-" + friendEntry.id).c_str());
                const ImVec2 rowStart = ImGui::GetCursorScreenPos();
                const ImVec4 friendPresence = presenceByUser_.contains(friendEntry.id)
                    && presenceByUser_.at(friendEntry.id) == "online"
                    ? ImVec4(0.12F, 0.88F, 0.67F, 1.0F) : ImVec4(0.35F, 0.40F, 0.50F, 1.0F);
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(rowStart.x + 15.0F, rowStart.y + 15.0F),
                                                             14.0F, ImGui::GetColorU32(ImGuiCol_Header), 20);
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(rowStart.x + 25.0F, rowStart.y + 25.0F),
                                                             4.5F, ImGui::GetColorU32(friendPresence), 14);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 40.0F);
                ImGui::TextUnformatted(friendEntry.nickname.c_str());
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 40.0F);
                ImGui::TextDisabled("@%s", friendEntry.username.c_str());
                ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0F, ImGui::GetWindowWidth() - 126.0F));
                if (ImGui::Button("Mesaj gönder", ImVec2(112.0F * dpiScale, 30.0F * dpiScale))) OpenDirectChat(friendEntry);
                ImGui::Separator();
                ImGui::PopID();
            }
            if (!any) ImGui::TextDisabled("Henüz arkadaşın yok. Sol panelden kullanıcı adıyla arama yapabilirsin.");
        } else if (homeSection_ == HomeSection::Pending) {
            bool any = false;
            for (const auto& friendEntry : friends_) {
                if (friendEntry.state != "pending") continue;
                any = true;
                const bool incoming = friendEntry.requesterId != account.id;
                ImGui::PushID(("home-pending-" + friendEntry.id).c_str());
                ImGui::Text("%s  @%s", friendEntry.nickname.c_str(), friendEntry.username.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled(incoming ? "sana istek gönderdi" : "yanıt bekleniyor");
                if (incoming) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Kabul et")) {
                        const std::string friendshipId = friendEntry.id;
                        QueuePlatformAction([this, friendshipId](std::string& error) {
                            return platform_.AcceptFriendRequest(friendshipId, error);
                        }, "Arkadaşlık isteği kabul edildi.", false, true, false);
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(incoming ? "Reddet" : "İptal et")) {
                    const std::string friendshipId = friendEntry.id;
                    QueuePlatformAction([this, friendshipId](std::string& error) {
                        return platform_.DismissFriendRequest(friendshipId, error);
                    }, "Arkadaşlık isteği kaldırıldı.", false, true, false);
                }
                ImGui::Separator();
                ImGui::PopID();
            }
            if (!any) ImGui::TextDisabled("Bekleyen arkadaşlık isteği yok.");
        } else {
            bool any = false;
            for (const auto& notification : notifications_) {
                if (notification.read) continue;
                any = true;
                ImGui::PushID(("home-notification-" + notification.id).c_str());
                ImGui::TextUnformatted(notification.title.c_str());
                ImGui::TextWrapped("%s", notification.body.c_str());
                if (ImGui::SmallButton("Okundu işaretle")) {
                    const std::string notificationId = notification.id;
                    QueuePlatformAction([this, notificationId](std::string& error) {
                        return platform_.MarkNotificationRead(notificationId, error);
                    }, "Bildirim okundu.", false, true, false);
                }
                ImGui::Separator();
                ImGui::PopID();
            }
            if (!any) ImGui::TextDisabled("Yeni bildirimin yok.");
        }
    }
    if (activePage_ == ClientPage::Voice) {
    std::string voiceChannelLabel = roomBuffer_.data();
    if (roomOverview_) {
        const auto selectedVoice = std::find_if(roomOverview_->channels.begin(), roomOverview_->channels.end(),
            [this](const PlatformChannel& channel) { return channel.id == activeVoiceChannelId_; });
        if (selectedVoice != roomOverview_->channels.end()) voiceChannelLabel += " / " + selectedVoice->name;
    }
    SectionTitle(T(TextId::Room), voiceChannelLabel.c_str());
    const auto peers = network_.PeersSnapshot();
    const unsigned peerCount = peers ? static_cast<unsigned>(peers->size()) : 0U;
    ImGui::TextDisabled("%u katılımcı · yerel ses ve gizlilik kontrolleri", peerCount);
    ImGui::Spacing();
    const float toggleGap = 8.0F;
    const float toggleWidth = (ImGui::GetContentRegionAvail().x - toggleGap) * 0.5F;
    const bool microphoneMuted = audio_.IsMicrophoneMuted();
    const std::string microphoneButton = std::string(microphoneMuted ? T(TextId::UnmuteMicrophone) : T(TextId::MuteMicrophone)) + "##microphone";
    if (!voiceCanSpeak_) ImGui::BeginDisabled();
    if (ImGui::Button(microphoneButton.c_str(),
                                ImVec2(toggleWidth, 34.0F * dpiScale))) {
        ToggleMicrophoneMuted();
    }
    if (!voiceCanSpeak_) ImGui::EndDisabled();
    ImGui::SameLine(0.0F, toggleGap);
    const bool outputMuted = audio_.IsOutputMuted();
    const std::string outputButton = std::string(outputMuted ? T(TextId::UnmuteAll) : T(TextId::MuteAll)) + "##output";
    if (ImGui::Button(outputButton.c_str(),
                                ImVec2(toggleWidth, 34.0F * dpiScale))) {
        ToggleOutputMuted();
    }
    ImGui::TextDisabled("%s", T(TextId::LocalControlsOnly));
    ImGui::Spacing();
    ImGui::TextDisabled("%s", T(TextId::MicrophoneLabel));
    Meter(audio_.MicrophoneLevel(), settings_.voiceActivation && !microphoneMuted
        ? audio_.VoiceThresholdLevel() : -1.0F);
    ImGui::TextDisabled("RMS %.1f dBFS", static_cast<double>(audio_.MicrophoneRmsDbfs()));
    const bool transmitting = audio_.IsTransmitting();
    const bool microphoneAvailable = audio_.HasCaptureDevice();
    const ImVec4 microphoneColor = !voiceCanSpeak_
        ? ImVec4(1.0F, 0.72F, 0.25F, 1.0F)
        : !microphoneAvailable
        ? ImVec4(1.0F, 0.62F, 0.24F, 1.0F)
        : microphoneMuted
        ? ImVec4(1.0F, 0.42F, 0.36F, 1.0F)
        : (transmitting ? ImVec4(0.25F, 0.90F, 0.62F, 1.0F)
                        : ImVec4(0.48F, 0.57F, 0.70F, 1.0F));
    const char* microphoneStatus = !voiceCanSpeak_ ? "Hesap kısıtlaması - yalnız dinleme modu"
        : !microphoneAvailable ? "Mikrofon yok - yalniz dinleme modu"
        : microphoneMuted ? "Mikrofon kapali - ses gonderilmiyor"
        : (transmitting ? "Konusma algilandi - gonderiliyor" : "Sessiz - ses paketi gonderilmiyor");
    char voiceTraffic[96]{};
    std::snprintf(voiceTraffic, sizeof(voiceTraffic), "%.1f KB / %llu paket",
                  static_cast<double>(network_.AudioBytesSent()) / 1024.0,
                  static_cast<unsigned long long>(network_.AudioPacketsSent()));
    const bool voiceTrafficFitsInline = ImGui::CalcTextSize(microphoneStatus).x
        + ImGui::CalcTextSize(voiceTraffic).x + 24.0F * dpiScale
        <= ImGui::GetContentRegionAvail().x;
    ImGui::TextColored(microphoneColor, "%s", microphoneStatus);
    if (voiceTrafficFitsInline) ImGui::SameLine();
    ImGui::TextDisabled("%s", voiceTraffic);
    const VoicePath voicePath = network_.ActiveVoicePath();
    ImGui::TextDisabled("Ses yolu: %s", voicePath == VoicePath::DirectPeer
        ? "Dogrudan P2P" : (voicePath == VoicePath::Probing ? "P2P baglantisi araniyor" : "Sunucu relay"));

    ImGui::Spacing();
    const bool denoiseAvailable = network_.ServerDenoiseAvailable();
    const bool denoiseEnabled = network_.ServerDenoiseEnabled();
    const bool denoiseControlLocked = network_.State() == ConnectionState::Connecting || connectPending_;
    if (denoiseControlLocked) ImGui::BeginDisabled();
    int denoiseMode = settings_.serverDenoise ? 2 : (settings_.localDenoise ? 1 : 0);
    constexpr const char* denoiseModes[] = {"Kapali", "Yerel RNNoise", "Sunucu RNNoise"};
    ImGui::SetNextItemWidth(std::max(180.0F, ImGui::GetContentRegionAvail().x - 164.0F));
    if (ImGui::Combo("##denoiseMode", &denoiseMode, denoiseModes, IM_ARRAYSIZE(denoiseModes))) {
        const bool previousServerDenoise = settings_.serverDenoise;
        settings_.localDenoise = denoiseMode == 1;
        settings_.serverDenoise = denoiseMode == 2;
        audio_.SetLocalDenoise(settings_.localDenoise);
        SaveSettings();
        if (network_.State() == ConnectionState::Connected
            && previousServerDenoise != settings_.serverDenoise) {
            Disconnect();
            Connect();
            uiMessage_ = "Gurultu engelleme yolu yeniden baglaniyor";
        }
    }
    if (denoiseControlLocked) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(denoiseEnabled ? ImVec4(0.35F, 0.72F, 1.0F, 1.0F)
                                     : ImVec4(0.48F, 0.57F, 0.70F, 1.0F),
                       "%s", denoiseEnabled ? "SUNUCUDA ETKIN" : (settings_.localDenoise ? "YERELDE ETKIN" : (connectedOrConnecting && !denoiseAvailable ? "HAK/KAPASITE YOK" : "KAPALI")));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", settings_.serverDenoise ? ServerDenoiseReasonText(serverDenoiseReason_)
                                                      : "Yerel veya sunucu filtresini buradan secin.");
    ImGui::PopStyleColor();

    if (connectedOrConnecting) ImGui::BeginDisabled();
    if (ImGui::Checkbox(T(TextId::DirectP2P), &settings_.p2pEnabled)) SaveSettings();
    if (connectedOrConnecting) ImGui::EndDisabled();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", T(TextId::DirectP2PPrivacy));
    ImGui::PopStyleColor();

    if (!connectedOrConnecting) ImGui::BeginDisabled();
    if (ImGui::Button(T(TextId::TestEncryptedVoice))) {
        if (!network_.BeginEncryptedEchoTest()) uiMessage_ = "Ses yolu testi baslatilamadi";
    }
    if (!connectedOrConnecting) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", network_.EchoTestStatus().c_str());

    if (showMemberPanel_ && !showWideMemberPanel) {
    ImGui::Spacing();
    ImGui::SeparatorText(T(TextId::UsersLabel));
    if (!peers || peers->empty()) {
        ImGui::TextDisabled("%s", T(TextId::NoOtherUsers));
    } else {
        for (const auto& peer : *peers) {
            ImGui::PushID(static_cast<int>(peer.peerId));
            if (audio_.PeerSpeaking(peer.peerId)) {
                ImGui::TextColored(ImVec4(0.25F, 0.90F, 0.62F, 1.0F), "%s", peer.nickname.c_str());
            } else {
                ImGui::TextUnformatted(peer.nickname.c_str());
            }
            ImGui::SameLine();
            bool peerMuted = audio_.IsPeerMuted(peer.peerId);
            if (ImGui::Checkbox(T(TextId::Mute), &peerMuted)) audio_.SetPeerMuted(peer.peerId, peerMuted);
            int peerVolumePercent = static_cast<int>(std::lround(audio_.PeerVolume(peer.peerId) * 100.0F));
            if (peerMuted) ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderInt("##peerVolume", &peerVolumePercent, 0, 200, "Ses %d%%")) {
                audio_.SetPeerVolume(peer.peerId, static_cast<float>(peerVolumePercent) / 100.0F);
            }
            if (peerMuted) ImGui::EndDisabled();
            Meter(audio_.PeerLevel(peer.peerId));
            ImGui::Spacing();
            ImGui::PopID();
        }
    }
    }

    }
    if (activePage_ == ClientPage::Rooms) {
        SectionTitle("Oda yonetimi", platformRooms_.empty() ? nullptr : roomBuffer_.data());
        if (platformRooms_.empty()) {
            ImGui::TextWrapped("Henuz bir odaya uye degilsiniz. Sol panelden davet koduyla katilabilir veya yeni oda olusturabilirsiniz.");
        } else {
            const PlatformRoom& room = platformRooms_[static_cast<std::size_t>(
                std::clamp(selectedRoomIndex_, 0, static_cast<int>(platformRooms_.size() - 1)))];
            const char* role = room.role == "owner" ? "Sahip" : room.role == "admin" ? "Yonetici"
                : room.role == "mod" ? "Moderator" : "Uye";
            ImGui::Text("Rolunuz: %s", role);
            ImGui::Text("Dugum durumu: %s", connectedOrConnecting ? "aktif"
                : (room.nodeState.empty() ? "oda etkin degil" : room.nodeState.c_str()));
            ImGui::TextWrapped("Davet, katilim ve oda ayarlari sol panelde; uye ve rol islemleri Oda uyeleri ve yonetim bolumundedir.");
            ImGui::Spacing();
        ImGui::SeparatorText(T(TextId::Members));
            if (roomMembers_.empty()) {
                ImGui::TextDisabled("Uye listesini gormek icin yenileyin.");
                if (ImGui::Button(roomMembersRefreshPending_ ? "Yenileniyor..." : "Uye listesini yenile")) {
                    RefreshRoomMembers();
                }
            } else {
                for (const auto& member : roomMembers_) {
                    ImGui::BulletText("%s  @%s", member.nickname.c_str(), member.username.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", member.role.c_str());
                }
            }
        }
    }
    if (activePage_ == ClientPage::Messages) {
    ImGui::Spacing();
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 11.0F * dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(13.0F * dpiScale, 9.0F * dpiScale));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::BeginChild("messageSecurityBar", ImVec2(0.0F, 48.0F * dpiScale),
                      ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushFont(SemiboldUiFont(), ImGui::GetStyle().FontSizeBase * 0.96F);
    ImGui::TextUnformatted(directConversationId_.empty()
        ? T(TextId::EncryptedRoomMessages) : T(TextId::EncryptedDirectMessage));
    ImGui::PopFont();
    const char* secureLabel = chatKeyReady_ ? "Güvenli · Hazır" : "Güvenli sohbet hazırlanıyor";
    const ImVec2 secureSize = ImGui::CalcTextSize(secureLabel);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0F * dpiScale,
                            ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x
                                - secureSize.x));
    const ImVec4 secureColor = chatKeyReady_ ? ImVec4(0.20F, 0.86F, 0.62F, 1.0F)
                                             : ImVec4(0.95F, 0.68F, 0.25F, 1.0F);
    ImGui::TextColored(secureColor, "%s", secureLabel);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::Dummy(ImVec2(0.0F, 4.0F * dpiScale));
    if (directConversationId_.empty() && roomOverview_) {
        const auto selected = std::find_if(roomOverview_->channels.begin(), roomOverview_->channels.end(),
            [this](const PlatformChannel& channel) { return channel.id == chatChannelId_; });
        if (selected != roomOverview_->channels.end()) {
            ImGui::Text("# %s", selected->name.c_str());
            if (selected->contentRating == "adult") {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95F, 0.52F, 0.28F, 1.0F), "18+");
            }
            if (!selected->topic.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", selected->topic.c_str()); }
            ImGui::TextDisabled("Medya: %s%s",
                selected->mediaPostingPolicy == "disabled" ? "kapali"
                    : selected->mediaPostingPolicy == "moderators" ? "yalniz moderatorler" : "uyeler",
                selected->localMediaScanRequired ? " · yerel tarama zorunlu" : "");
            ImGui::SameLine();
            if (selected->pinCount > 0) {
                const std::string pinsLabel = pinsRefreshPending_ ? "Sabitlenenler..."
                    : "Sabitlenen: " + std::to_string(selected->pinCount);
                if (pinsRefreshPending_) ImGui::BeginDisabled();
                if (ImGui::SmallButton(pinsLabel.c_str())) RefreshPins();
                if (pinsRefreshPending_) ImGui::EndDisabled();
                ImGui::SameLine();
            }
            int notificationMode = selected->notificationMode == "all" ? 0
                : selected->notificationMode == "muted" ? (selected->muteUntil.empty() ? 5 : 2) : 1;
            const char* notificationModes[] = {
                "Tum mesajlar", "Yalniz bahsetmeler", "1 saat sessiz",
                "8 saat sessiz", "24 saat sessiz", "Suresiz sessiz",
            };
            ImGui::SetNextItemWidth(190.0F);
            if (ImGui::Combo("##channelNotifications", &notificationMode, notificationModes, IM_ARRAYSIZE(notificationModes))) {
                const std::string channelId = selected->id;
                const std::string mode = notificationMode == 0 ? "all"
                    : notificationMode == 1 ? "mentions" : "muted";
                const int muteMinutes = notificationMode == 2 ? 60 : notificationMode == 3 ? 480
                    : notificationMode == 4 ? 1'440 : 0;
                QueuePlatformAction([this, channelId, mode, muteMinutes](std::string& error) {
                    return platform_.SetChannelNotifications(channelId, mode, error, muteMinutes);
                }, "Kanal bildirim tercihi kaydedildi.", false, false, false, true);
            }
        }
    }
    if (showPinnedMessages_) ImGui::OpenPopup("Sabitlenen mesajlar");
    ImGui::SetNextWindowSize(ImVec2(600.0F, 420.0F), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Sabitlenen mesajlar", nullptr, ImGuiWindowFlags_NoResize)) {
        const bool canManagePins = !platformRooms_.empty()
            && (platformRooms_[static_cast<std::size_t>(selectedRoomIndex_)].role == "owner"
                || platformRooms_[static_cast<std::size_t>(selectedRoomIndex_)].role == "admin"
                || platformRooms_[static_cast<std::size_t>(selectedRoomIndex_)].role == "mod");
        if (pinnedMessageIds_.empty()) ImGui::TextDisabled("Bu kanalda sabitlenmis mesaj yok.");
        ImGui::BeginChild("pinnedMessageList", ImVec2(0.0F, -42.0F), ImGuiChildFlags_Borders);
        std::string unpinRequested;
        for (const auto& messageId : pinnedMessageIds_) {
            ImGui::PushID(("pin-" + messageId).c_str());
            const auto line = std::find_if(chatLines_.begin(), chatLines_.end(),
                [&messageId](const ChatLine& value) { return value.id == messageId; });
            if (line == chatLines_.end()) {
                ImGui::TextDisabled("Bu mesaj mevcut 300 mesajlik gorunumun disinda.");
            } else {
                ImGui::TextColored(ImVec4(0.48F, 0.67F, 1.0F, 1.0F), "%s", line->sender.c_str());
                ImGui::TextWrapped("%s", line->text.c_str());
            }
            if (canManagePins) {
                if (ImGui::SmallButton("Sabitlemeyi kaldir")) {
                    unpinRequested = messageId;
                    ImGui::PopID();
                    break;
                }
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        if (!unpinRequested.empty()) {
            const std::string channelId = chatChannelId_;
            QueuePlatformAction([this, channelId, unpinRequested](std::string& error) {
                return platform_.UnpinChannelMessage(channelId, unpinRequested, error);
            }, "Mesaj sabitlemesi kaldirildi.", false, false, false, true);
            std::erase(pinnedMessageIds_, unpinRequested);
        }
        ImGui::EndChild();
            if (ImGui::Button("Kapat", ImVec2(-FLT_MIN, 32.0F * dpiScale))) {
            showPinnedMessages_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!directConversationId_.empty()) {
        ImGui::Text("Konusma: %s", directConversationLabel_.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(T(TextId::BackToRoomMessages))) {
            directConversationId_.clear(); directConversationLabel_.clear(); chatConversationId_.clear(); WipeChatLines();
            if (roomOverview_) {
                const auto text = std::find_if(roomOverview_->channels.begin(), roomOverview_->channels.end(),
                    [](const PlatformChannel& channel) { return channel.type == "text"; });
                if (text != roomOverview_->channels.end()) {
                    chatChannelId_ = text->id; selectedChannelId_ = text->id; selectedChannelType_ = text->type;
                    const auto draft = chatDrafts_.find(chatChannelId_);
                    CopyToBuffer(draft == chatDrafts_.end() ? std::string{} : draft->second, chatBuffer_);
                }
            }
            crypto_wipe(chatKey_.data(), chatKey_.size()); chatKeyReady_ = false; RefreshChat();
        }
    }
    if (chatRefreshPending_) ImGui::TextDisabled("Mesajlar güvenli biçimde eşitleniyor...");
        else if (ImGui::SmallButton(T(TextId::RefreshMessages))) RefreshChat();
    if (!chatRefreshPending_ && chatHasMore_ && !chatBeforeCursor_.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Daha eski mesajlari yukle")) RefreshChat(true);
    }
    if (focusChatSearch_) {
        ImGui::SetKeyboardFocusHere();
        focusChatSearch_ = false;
    }
    ImGui::SetNextItemWidth(std::min(320.0F, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##chatSearch", "Bu kanalda ara (Ctrl+F)",
                             chatSearchBuffer_.data(), chatSearchBuffer_.size());
    float composerReserve = 58.0F * dpiScale + ImGui::GetStyle().ItemSpacing.y;
    if (!typingUntilMs_.empty()) composerReserve += ImGui::GetTextLineHeightWithSpacing();
    if (!editTargetId_.empty() || !replyTargetId_.empty()) {
        composerReserve += ImGui::GetFrameHeightWithSpacing();
    }
    composerReserve += static_cast<float>(pendingChatAttachments_.size())
        * ImGui::GetFrameHeightWithSpacing();
    const float chatHistoryHeight = std::max(
        120.0F * dpiScale,
        ImGui::GetContentRegionAvail().y - composerReserve);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0F);
    ImGui::BeginChild("chatHistory", ImVec2(0.0F, chatHistoryHeight), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (chatLines_.empty()) {
        const float emptyOffset = std::max(12.0F, (chatHistoryHeight - 90.0F) * 0.5F);
        ImGui::SetCursorPosY(emptyOffset);
        const char* emptyTitle = chatRefreshPending_ ? "Mesajlar getiriliyor"
            : chatKeyReady_ ? "Henüz mesaj yok" : "Güvenli sohbet hazırlanıyor";
        const char* emptyDetail = chatRefreshPending_ ? "Şifreli geçmiş eşitleniyor, lütfen kısa bir süre bekleyin."
            : chatKeyReady_ ? "Bu kanaldaki ilk mesajı yazarak sohbeti başlatabilirsin."
                            : "Cihaz anahtarı doğrulanınca mesaj alanı otomatik olarak açılacak.";
        const ImVec2 titleSize = ImGui::CalcTextSize(emptyTitle);
        ImGui::SetCursorPosX(std::max(ImGui::GetStyle().WindowPadding.x,
            (ImGui::GetWindowWidth() - titleSize.x) * 0.5F));
        ImGui::TextUnformatted(emptyTitle);
        const ImVec2 detailSize = ImGui::CalcTextSize(emptyDetail);
        ImGui::SetCursorPosX(std::max(ImGui::GetStyle().WindowPadding.x,
            (ImGui::GetWindowWidth() - detailSize.x) * 0.5F));
        ImGui::TextDisabled("%s", emptyDetail);
    }
    std::array<int, 300> visibleMessageIndices{};
    int visibleMessageCount = 0;
    const std::string_view search(chatSearchBuffer_.data());
    for (int index = 0; index < static_cast<int>(chatLines_.size()) && visibleMessageCount < static_cast<int>(visibleMessageIndices.size()); ++index) {
        const ChatLine& line = chatLines_[static_cast<std::size_t>(index)];
        if (search.empty() || line.text.find(search) != std::string::npos
            || line.sender.find(search) != std::string::npos) {
            visibleMessageIndices[static_cast<std::size_t>(visibleMessageCount++)] = index;
        }
    }
    if (!search.empty() && visibleMessageCount == 0) ImGui::TextDisabled("Eslesen mesaj bulunamadi.");
    const float historyViewportTop = ImGui::GetWindowPos().y;
    const float historyViewportBottom = historyViewportTop + ImGui::GetWindowHeight();
    const float messageIndent = 42.0F * dpiScale;
    for (int visibleIndex = 0; visibleIndex < visibleMessageCount; ++visibleIndex) {
        const int index = visibleMessageIndices[static_cast<std::size_t>(visibleIndex)];
        const ChatLine& line = chatLines_[static_cast<std::size_t>(index)];
        const bool grouped = visibleIndex > 0 && [&] {
            const ChatLine& previous = chatLines_[static_cast<std::size_t>(
                visibleMessageIndices[static_cast<std::size_t>(visibleIndex - 1)])];
            return CanGroupMessages(previous.senderId, previous.createdAt,
                                    line.senderId, line.createdAt, line.replyTo);
        }();
        const bool nextGrouped = visibleIndex + 1 < visibleMessageCount && [&] {
            const ChatLine& next = chatLines_[static_cast<std::size_t>(
                visibleMessageIndices[static_cast<std::size_t>(visibleIndex + 1)])];
            return CanGroupMessages(line.senderId, line.createdAt,
                                    next.senderId, next.createdAt, next.replyTo);
        }();
        const float availableRowWidth = std::max(220.0F * dpiScale,
                                                 ImGui::GetContentRegionAvail().x);
        const float messageTimeReserve = (line.delivery == ChatDeliveryState::Sent
            ? 60.0F : 150.0F) * dpiScale;
        const float textWrapWidth = std::max(120.0F * dpiScale,
            availableRowWidth - messageIndent - messageTimeReserve - 30.0F * dpiScale);
        const float textHeight = line.text.empty() ? 0.0F
            : ImGui::CalcTextSize(line.text.c_str(), nullptr, false, textWrapWidth).y;
        const bool hasReactions = std::any_of(line.reactions.begin(), line.reactions.end(),
            [](const std::uint16_t count) { return count > 0U; });
        const float headerHeight = grouped ? 0.0F : 28.0F * dpiScale;
        const float attachmentHeight = line.attachmentIds.empty() ? 0.0F : 29.0F * dpiScale;
        const float reactionHeight = hasReactions ? 20.0F * dpiScale : 0.0F;
        const float rowHeight = std::max(grouped ? 42.0F * dpiScale : 66.0F * dpiScale,
            17.0F * dpiScale + headerHeight + textHeight + attachmentHeight + reactionHeight);
        // Consecutive messages from the same sender form one visual bubble.
        // ImGui normally inserts ItemSpacing between child windows; cancel it
        // only inside a group so the surfaces physically touch.
        if (grouped) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
        }
        const float rowTop = ImGui::GetCursorScreenPos().y;
        if (rowTop + rowHeight < historyViewportTop - 12.0F * dpiScale
            || rowTop > historyViewportBottom + 12.0F * dpiScale) {
            ImGui::Dummy(ImVec2(0.0F, rowHeight));
            continue;
        }
        ImGui::PushID(line.id.c_str());
        const ImVec4 messageSurface = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
        const ImVec2 bubbleMinimum = ImGui::GetCursorScreenPos();
        const ImVec2 bubbleMaximum(
            bubbleMinimum.x + ImGui::GetContentRegionAvail().x,
            bubbleMinimum.y + rowHeight);
        const ImDrawFlags bubbleCorners = !grouped && nextGrouped
            ? ImDrawFlags_RoundCornersTop
            : grouped && nextGrouped
                ? ImDrawFlags_None
                : grouped
                    ? ImDrawFlags_RoundCornersBottom
                    : ImDrawFlags_RoundCornersAll;
        ImGui::GetWindowDrawList()->AddRectFilled(
            bubbleMinimum, bubbleMaximum, ImGui::GetColorU32(messageSurface),
            10.0F * dpiScale, bubbleCorners);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(12.0F * dpiScale, 8.0F * dpiScale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(7.0F * dpiScale, 4.0F * dpiScale));
        ImGui::BeginChild("message", ImVec2(0.0F, rowHeight),
                          ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            const ImVec2 rowMinimum = ImGui::GetWindowPos();
            const ImVec2 rowMaximum(rowMinimum.x + ImGui::GetWindowWidth(), rowMinimum.y + ImGui::GetWindowHeight());
            ImVec4 hover = ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered);
            hover.w = 0.34F;
            ImGui::GetWindowDrawList()->AddRectFilled(rowMinimum, rowMaximum,
                                                      ImGui::GetColorU32(hover),
                                                      10.0F * dpiScale,
                                                      bubbleCorners);
        }
        const float contentStartX = ImGui::GetCursorPosX();
        const float contentStartY = ImGui::GetCursorPosY();
        const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
        std::string messageTime = LocalMessageTime(line.createdAt);
        if (line.delivery == ChatDeliveryState::Pending) messageTime += " · Gönderiliyor";
        else if (line.delivery == ChatDeliveryState::Failed) messageTime += " · Gönderilemedi";
        const ImVec2 messageTimeSize = ImGui::CalcTextSize(messageTime.c_str());
        const float timestampY = ImGui::GetWindowPos().y + contentStartY
            + (grouped ? 1.0F * dpiScale : 2.0F * dpiScale);
        ImGui::GetWindowDrawList()->AddText(
            SemiboldUiFont(), ImGui::GetStyle().FontSizeBase * 0.76F,
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth()
                       - ImGui::GetStyle().WindowPadding.x - messageTimeSize.x,
                   timestampY),
            line.delivery == ChatDeliveryState::Failed
                ? IM_COL32(239, 100, 108, 255)
                : ImGui::GetColorU32(ImGuiCol_TextDisabled),
            messageTime.c_str());
        if (!grouped) {
            const ImVec2 avatarOrigin = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(avatarOrigin.x + 14.0F * dpiScale,
                       avatarOrigin.y + 14.0F * dpiScale),
                14.0F * dpiScale, ImGui::GetColorU32(accent), 20);
            const char initial[2]{line.sender.empty() ? '?' : static_cast<char>(std::toupper(
                static_cast<unsigned char>(line.sender.front()))), '\0'};
            const ImVec2 initialSize = ImGui::CalcTextSize(initial);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(avatarOrigin.x + 14.0F * dpiScale - initialSize.x * 0.5F,
                       avatarOrigin.y + 14.0F * dpiScale - initialSize.y * 0.5F),
                IM_COL32_WHITE, initial);
            ImGui::SetCursorPosX(contentStartX + messageIndent);
            ImGui::PushFont(SemiboldUiFont(), ImGui::GetStyle().FontSizeBase * 0.94F);
            ImGui::TextColored(accent, "%s", line.sender.c_str());
            ImGui::PopFont();
            if (!line.replyTo.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("· yanıt");
            }
            ImGui::SetCursorPosY(contentStartY + 27.0F * dpiScale);
        }
        ImGui::SetCursorPosX(contentStartX + messageIndent);
        if (!line.text.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textWrapWidth);
            ImGui::TextUnformatted(line.text.c_str());
            ImGui::PopTextWrapPos();
            if (line.edited) {
                ImGui::SameLine();
                ImGui::TextDisabled("(düzenlendi)");
            }
        }
        for (std::size_t attachmentIndex = 0; attachmentIndex < line.attachmentIds.size(); ++attachmentIndex) {
            if (attachmentIndex == 0U) ImGui::SetCursorPosX(contentStartX + messageIndent);
            if (attachmentIndex > 0) ImGui::SameLine();
            const std::string label = mediaDownloadPending_ ? "Indiriliyor..."
                : "Şifreli dosyayı indir##" + line.attachmentIds[attachmentIndex];
            if (mediaDownloadPending_) ImGui::BeginDisabled();
            if (ImGui::SmallButton(label.c_str())) DownloadChatAttachment(line.attachmentIds[attachmentIndex]);
            if (mediaDownloadPending_) ImGui::EndDisabled();
        }
        constexpr std::array<const char*, 6> reactionLabels{"+1", "<3", ":D", "!", ":(", ">:("};
        bool firstReaction = true;
        for (std::size_t reactionIndex = 0; reactionIndex < line.reactions.size(); ++reactionIndex) {
            if (line.reactions[reactionIndex] == 0) continue;
            if (firstReaction) {
                ImGui::SetCursorPosX(contentStartX + messageIndent);
                firstReaction = false;
            } else {
                ImGui::SameLine();
            }
            ImGui::TextDisabled("%s %u", reactionLabels[reactionIndex], line.reactions[reactionIndex]);
        }
        if (ImGui::BeginPopupContextWindow("messageActions", ImGuiPopupFlags_MouseButtonRight)) {
                if (ImGui::MenuItem(T(TextId::Reply))) replyTargetId_ = line.id;
            const bool ownMessage = line.senderId == account.id;
                if (ownMessage && ImGui::MenuItem(T(TextId::Edit))) {
                CopyToBuffer(line.text, chatBuffer_); editTargetId_ = line.id; replyTargetId_.clear();
            }
            if (ImGui::BeginMenu("Tepki")) {
                constexpr std::array<const char*, 6> reactionCodes{"like","love","laugh","wow","sad","angry"};
                for (std::size_t reactionIndex = 0; reactionIndex < reactionCodes.size(); ++reactionIndex) {
                    if (ImGui::MenuItem(reactionLabels[reactionIndex])) SendChatEvent("reaction", line.id, {}, reactionCodes[reactionIndex]);
                }
                ImGui::EndMenu();
            }
            if (!ownMessage && !line.attachmentIds.empty() && mediaSafetyConfig_
                && mediaSafetyConfig_->privateMediaReportingAvailable
                && ImGui::BeginMenu("Görseli Guardian'a bildir")) {
                for (std::size_t attachmentIndex = 0; attachmentIndex < line.attachmentIds.size(); ++attachmentIndex) {
                    const std::string reportLabel = "Ek " + std::to_string(attachmentIndex + 1U)
                        + "##report-" + line.attachmentIds[attachmentIndex];
                    if (ImGui::MenuItem(reportLabel.c_str())) {
                        reportAttachmentId_ = line.attachmentIds[attachmentIndex];
                        reportMessageId_ = line.id;
                        reportUserId_ = line.senderId;
                        mediaReportNoteBuffer_.fill('\0');
                        mediaReportReasonIndex_ = 0;
                    }
                }
                ImGui::EndMenu();
            }
                if (ownMessage && ImGui::MenuItem(T(TextId::Delete))) SendChatEvent("delete", line.id, {});
            const bool canModerate = !directConversationId_.empty() ? false : (!platformRooms_.empty() &&
                (platformRooms_[static_cast<std::size_t>(selectedRoomIndex_)].role == "owner" ||
                 platformRooms_[static_cast<std::size_t>(selectedRoomIndex_)].role == "admin" ||
                 platformRooms_[static_cast<std::size_t>(selectedRoomIndex_)].role == "mod"));
            if (canModerate && !chatChannelId_.empty() && ImGui::MenuItem("Mesaji sabitle")) {
                const std::string channelId = chatChannelId_;
                const std::string messageId = line.id;
                QueuePlatformAction([this, channelId, messageId](std::string& error) {
                    return platform_.PinChannelMessage(channelId, messageId, error);
                }, "Mesaj kanala sabitlendi.", false, false, false, false);
            }
                if (!ownMessage && canModerate && ImGui::MenuItem(T(TextId::ModeratorRemove))) {
                moderationTargetId_ = line.id; moderationMessageReasonBuffer_.fill('\0'); ImGui::OpenPopup("Mesaji kaldir");
            }
            ImGui::EndPopup();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    if (!moderationTargetId_.empty()) ImGui::OpenPopup("Mesaji kaldir");
    if (ImGui::BeginPopupModal("Mesaji kaldir", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(T(TextId::ModerationReasonRequired));
        ImGui::InputTextMultiline("##moderationReason", moderationMessageReasonBuffer_.data(), moderationMessageReasonBuffer_.size(), ImVec2(420.0F, 72.0F));
        if (ImGui::Button(T(TextId::RemoveMessage)) && std::strlen(moderationMessageReasonBuffer_.data()) >= 3) {
            SendChatEvent("delete", moderationTargetId_, {}, {}, moderationMessageReasonBuffer_.data());
            moderationTargetId_.clear(); ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(); if (ImGui::Button(T(TextId::Cancel))) { moderationTargetId_.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (!reportAttachmentId_.empty()) ImGui::OpenPopup("Guardian medya raporu");
    if (ImGui::BeginPopupModal("Guardian medya raporu", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Şifreli görseli Guardian incelemesine gönder");
        ImGui::TextDisabled("Görsel yalnız moderasyon anahtarıyla açılabilen bir kanıt olarak yüklenir.");
        static constexpr std::array<const char*, 9> reasonLabels{
            "Yetişkin içerik", "İstenmeyen hassas görsel", "Çocuk güvenliği", "Grafik şiddet",
            "Nefret sembolü", "Taciz", "Kimliğe bürünme", "Spam", "Diğer",
        };
        ImGui::SetNextItemWidth(420.0F * dpiScale);
        ImGui::Combo("Neden", &mediaReportReasonIndex_, reasonLabels.data(),
                     static_cast<int>(reasonLabels.size()));
        ImGui::InputTextMultiline("Açıklama (isteğe bağlı)", mediaReportNoteBuffer_.data(),
                                  mediaReportNoteBuffer_.size(), ImVec2(420.0F * dpiScale, 90.0F * dpiScale));
        if (mediaReportPending_) ImGui::BeginDisabled();
        if (ImGui::Button(mediaReportPending_ ? "Gönderiliyor..." : "Şifreli raporu gönder",
                          ImVec2(200.0F * dpiScale, 0.0F))) {
            SubmitMediaReport();
        }
        ImGui::SameLine();
        if (ImGui::Button(T(TextId::Cancel))) {
            reportAttachmentId_.clear();
            reportMessageId_.clear();
            reportUserId_.clear();
            mediaReportNoteBuffer_.fill('\0');
            ImGui::CloseCurrentPopup();
        }
        if (mediaReportPending_) ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    if (showMediaReportReceipt_) ImGui::OpenPopup("Guardian raporu alındı");
    if (ImGui::BeginPopupModal("Guardian raporu alındı", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Rapor ve şifreli kanıt inceleme kuyruğuna alındı.");
        ImGui::TextDisabled("Yanlış bildirdiyseniz yalnız siz bu raporu geri çekebilirsiniz.");
        if (mediaReportPending_) ImGui::BeginDisabled();
        if (ImGui::Button("Raporu geri çek")) WithdrawLastMediaReport();
        ImGui::SameLine();
        if (ImGui::Button("Kapat")) {
            showMediaReportReceipt_ = false;
            ImGui::CloseCurrentPopup();
        }
        if (mediaReportPending_) ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    if (!typingUntilMs_.empty()) ImGui::TextDisabled("%s", T(TextId::SomeoneTyping));
    if (!editTargetId_.empty() || !replyTargetId_.empty()) {
        ImGui::TextDisabled(editTargetId_.empty() ? "Bir mesaja yanit veriyorsunuz" : "Mesaji duzenliyorsunuz");
        ImGui::SameLine();
        const std::string cancelChatAction = std::string(T(TextId::Cancel)) + "##chatAction";
        if (ImGui::SmallButton(cancelChatAction.c_str())) { editTargetId_.clear(); replyTargetId_.clear(); chatBuffer_.fill('\0'); }
    }
    for (std::size_t index = 0; index < pendingChatAttachments_.size();) {
        ImGui::TextDisabled("%s (%llu KB)", pendingChatAttachments_[index].name.c_str(),
                            static_cast<unsigned long long>((pendingChatAttachments_[index].encryptedBytes + 1023U) / 1024U));
        ImGui::SameLine();
        const std::string removeLabel = "Kaldir##pending-" + pendingChatAttachments_[index].id;
        if (ImGui::SmallButton(removeLabel.c_str())) {
            pendingChatAttachments_.erase(pendingChatAttachments_.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            ++index;
        }
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0F * dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(8.0F * dpiScale, 7.0F * dpiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(11.0F * dpiScale, 10.0F * dpiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0F * dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(8.0F * dpiScale, 0.0F));
    ImVec4 composerSurface = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    composerSurface.x = std::min(1.0F, composerSurface.x * 1.08F);
    composerSurface.y = std::min(1.0F, composerSurface.y * 1.07F);
    composerSurface.z = std::min(1.0F, composerSurface.z * 1.06F);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, composerSurface);
    ImGui::BeginChild("composerSurface", ImVec2(0.0F, 58.0F * dpiScale),
                      ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const float composerWidth = ImGui::GetContentRegionAvail().x;
    const float fileButtonWidth = std::min(112.0F * dpiScale,
                                           std::max(92.0F * dpiScale, composerWidth * 0.18F));
    const float sendButtonWidth = std::min(100.0F * dpiScale,
                                           std::max(84.0F * dpiScale, composerWidth * 0.15F));
    const float composerControlHeight = 40.0F * dpiScale;
    const float composerControlTop = ImGui::GetCursorPosY();
    if (mediaUploadPending_) ImGui::BeginDisabled();
    if (ImGui::Button(mediaUploadPending_ ? "Şifreleniyor..." : "Dosya ekle",
                      ImVec2(fileButtonWidth, composerControlHeight))) {
        SelectChatAttachment();
    }
    if (mediaUploadPending_) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetCursorPosY(composerControlTop + 3.0F * dpiScale);
    ImGui::SetNextItemWidth(std::max(120.0F * dpiScale,
        ImGui::GetContentRegionAvail().x - sendButtonWidth - 8.0F * dpiScale));
    const bool submitChat = ImGui::InputTextWithHint("##chat", T(TextId::WriteMessage), chatBuffer_.data(), chatBuffer_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    const bool chatEdited = ImGui::IsItemEdited();
    const bool chatActive = ImGui::IsItemActive();
    const ImVec2 mentionPopupPosition(ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y);
    std::string mentionPrefix;
    std::size_t mentionStart = std::string::npos;
    if (chatActive && directConversationId_.empty()) {
        const std::string draft(chatBuffer_.data());
        mentionStart = draft.rfind('@');
        if (mentionStart != std::string::npos
            && (mentionStart == 0 || draft[mentionStart - 1] == ' ' || draft[mentionStart - 1] == '\n')) {
            mentionPrefix = draft.substr(mentionStart + 1);
            if (mentionPrefix.find_first_of(" \r\n\t") != std::string::npos) mentionStart = std::string::npos;
        }
    }
    if (chatEdited) {
        const std::string draftKey = directConversationId_.empty() ? chatChannelId_ : directConversationId_;
        if (!draftKey.empty()) {
            chatDrafts_[draftKey] = chatBuffer_.data();
            if (chatDrafts_.size() > 20) chatDrafts_.erase(chatDrafts_.begin());
        }
    }
    if (chatEdited && !chatConversationId_.empty() && SteadyNowMs() - lastTypingSentMs_ >= 2'000) {
        realtime_.SendTyping(chatConversationId_, true, directConversationId_.empty() ? chatChannelId_ : std::string{});
        typingSent_ = true; lastTypingSentMs_ = SteadyNowMs();
    }
    if (!chatActive && typingSent_) {
        realtime_.SendTyping(chatConversationId_, false, directConversationId_.empty() ? chatChannelId_ : std::string{});
        typingSent_ = false;
    }
    ImGui::SameLine();
    ImGui::SetCursorPosY(composerControlTop);
    if (chatSendPending_) ImGui::BeginDisabled();
    if (ImGui::Button(chatSendPending_ ? "Gönderiliyor" : T(TextId::Send),
                      ImVec2(sendButtonWidth, composerControlHeight)) || submitChat) SendChatMessage();
    if (chatSendPending_) ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(5);
    if (mentionStart != std::string::npos) ImGui::OpenPopup("mentionCompletion");
    ImGui::SetNextWindowPos(mentionPopupPosition, ImGuiCond_Appearing, ImVec2(0.0F, 1.0F));
    if (ImGui::BeginPopup("mentionCompletion")) {
        if (mentionStart == std::string::npos) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        } else {
        unsigned shown = 0;
        for (const auto& member : roomMembers_) {
            if (!mentionPrefix.empty() && !std::string_view(member.username).starts_with(mentionPrefix)) continue;
            const std::string label = member.nickname + "  @" + member.username + "##mention-" + member.id;
            if (ImGui::Selectable(label.c_str())) {
                std::string draft(chatBuffer_.data());
                draft.replace(mentionStart, draft.size() - mentionStart, "@" + member.username + " ");
                CopyToBuffer(draft, chatBuffer_);
                ImGui::CloseCurrentPopup();
            }
            if (++shown >= 5) break;
        }
        if (shown == 0) ImGui::TextDisabled("Eslesen uye yok");
        ImGui::EndPopup();
        }
    }
    }
    }

    const std::string audioError = audio_.LastError();
    if (!audioError.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "%s", audioError.c_str());
    }
    if (!uiMessage_.empty()) {
        ImGui::TextColored(ImVec4(1.0F, 0.65F, 0.25F, 1.0F), "%s", uiMessage_.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    if (showWideMemberPanel) {
        ImGui::SameLine(0.0F, gap);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, sidebarSurface);
        ImGui::BeginChild("communityMembers", ImVec2(0.0F, -1.0F),
                          ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
        SectionTitle(T(TextId::Members), roomOverview_ ? roomOverview_->name.c_str() : nullptr);
        constexpr std::array<const char*, 4> roles{"owner", "admin", "mod", "member"};
        constexpr std::array<const char*, 4> roleLabels{"SAHIP", "YONETICILER", "MODERATORLER", "UYELER"};
        for (std::size_t roleIndex = 0; roleIndex < roles.size(); ++roleIndex) {
            unsigned roleCount = 0;
            for (const auto& member : roomMembers_) if (member.role == roles[roleIndex]) ++roleCount;
            if (roleCount == 0) continue;
            ImGui::Spacing();
            ImGui::TextDisabled("%s - %u", roleLabels[roleIndex], roleCount);
            for (const auto& member : roomMembers_) {
                if (member.role != roles[roleIndex]) continue;
                ImGui::PushID(("member-panel-" + member.id).c_str());
                const ImVec2 cursor = ImGui::GetCursorScreenPos();
                const ImVec2 dot(cursor.x + 5.0F, cursor.y + ImGui::GetTextLineHeight() * 0.5F);
                const auto presence = presenceByUser_.find(member.id);
                const std::string presenceStatus = member.id == account.id ? ownPresence_
                    : (presence == presenceByUser_.end() ? "offline" : presence->second);
                const ImVec4 presenceColor = presenceStatus == "online" ? ImVec4(0.25F, 0.84F, 0.61F, 1.0F)
                    : presenceStatus == "idle" ? ImVec4(0.95F, 0.72F, 0.25F, 1.0F)
                    : presenceStatus == "do_not_disturb" ? ImVec4(0.92F, 0.30F, 0.32F, 1.0F)
                    : ImVec4(0.34F, 0.39F, 0.47F, 1.0F);
                ImGui::GetWindowDrawList()->AddCircleFilled(dot, 4.0F, ImGui::GetColorU32(presenceColor));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16.0F);
                ImGui::TextUnformatted(member.nickname.c_str());
                const auto customStatus = member.id == account.id
                    ? std::string(customStatusBuffer_.data())
                    : (customStatusByUser_.contains(member.id) ? customStatusByUser_.at(member.id) : std::string{});
                if (!customStatus.empty()) {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16.0F);
                    ImGui::TextDisabled("%s", customStatus.c_str());
                }
                if (ImGui::BeginPopupContextItem("memberMenu")) {
                    ImGui::TextDisabled("@%s", member.username.c_str());
                    if (member.id != account.id && ImGui::MenuItem("Ozel mesaj")) {
                        const auto friendEntry = std::find_if(friends_.begin(), friends_.end(),
                            [&member](const PlatformFriend& friendValue) { return friendValue.id == member.id; });
                        if (friendEntry != friends_.end()) OpenDirectChat(*friendEntry);
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        if (roomMembers_.empty()) {
            ImGui::TextDisabled("Uye listesi henuz yuklenmedi.");
            if (ImGui::SmallButton("Uyeleri yenile")) RefreshRoomMembers();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - bottomBarHeight);
    ImVec4 footerSurface = ImGui::GetStyleColorVec4(ImGuiCol_ChildBg);
    footerSurface.x *= 0.82F;
    footerSurface.y *= 0.88F;
    footerSurface.z *= 1.08F;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, footerSurface);
    ImGui::BeginChild("statusFooter", ImVec2(0.0F, bottomBarHeight), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const UpdateState updateState = updater_.State();
    std::string updateLabel;
    if (updateState == UpdateState::Checking) updateLabel = T(TextId::Checking);
    else if (updateState == UpdateState::Available) {
        const auto available = updater_.Available();
        updateLabel = available ? available->version + " " + T(TextId::UpdateAvailable) : T(TextId::ReadyToInstall);
    } else if (updateState == UpdateState::Downloading) updateLabel = T(TextId::Downloading);
    else if (updateState == UpdateState::Ready) updateLabel = T(TextId::ReadyToInstall);
    else if (updateState == UpdateState::Error) updateLabel = T(TextId::CheckFailed);
    else updateLabel = std::string(T(TextId::UpToDate)) + "  |  " SONALIS_VERSION;
    const float controlSize = 38.0F * dpiScale;
    const float footerGap = 7.0F * dpiScale;
    const float accountWidth = (horizonLayout_.layoutClass == HorizonLayoutClass::Compact
        ? 148.0F : 188.0F) * dpiScale;
    const float updateButtonWidth = std::clamp(
        ImGui::CalcTextSize(updateLabel.c_str()).x + 28.0F * dpiScale,
        142.0F * dpiScale, 245.0F * dpiScale);
    const bool updateBusy = updateState == UpdateState::Checking || updateState == UpdateState::Downloading;
    const bool updateAccent = updateState == UpdateState::Available || updateState == UpdateState::Ready;

    const float footerY = std::max(0.0F, (ImGui::GetWindowHeight() - controlSize) * 0.5F);
    ImGui::SetCursorPosY(footerY);
    if (ImGui::BeginTable("footerLayout", 3,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX)) {
        const float leftControlsWidth = accountWidth + controlSize * 3.0F + footerGap * 4.0F;
        ImGui::TableSetupColumn("account-controls", ImGuiTableColumnFlags_WidthFixed, leftControlsWidth);
        ImGui::TableSetupColumn("connection-state", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("update-state", ImGuiTableColumnFlags_WidthFixed,
                                updateButtonWidth + 12.0F * dpiScale);
        ImGui::TableNextColumn();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0F * dpiScale);
        std::string accountLabel = account.nickname;
        if (horizonLayout_.layoutClass != HorizonLayoutClass::Compact) accountLabel += "  @" + account.username;
        const std::string fittedAccountLabel = FitTextToWidth(accountLabel, accountWidth - 22.0F * dpiScale);
        if (ImGui::Button((fittedAccountLabel + "##accountDock").c_str(), ImVec2(accountWidth, controlSize))) {
            ImGui::OpenPopup("accountMenu");
        }
        if (ImGui::IsItemHovered() && fittedAccountLabel != accountLabel) {
            ImGui::SetTooltip("%s", accountLabel.c_str());
        }
        ImGui::SameLine(0.0F, footerGap);
        const bool dockMicMuted = audio_.IsMicrophoneMuted();
        horizon::IconButtonOptions micOptions{};
        micOptions.size = ImVec2(controlSize, controlSize);
        micOptions.iconSize = 21.0F * dpiScale;
        micOptions.rounding = 7.0F * dpiScale;
        micOptions.selected = dockMicMuted;
        micOptions.tooltip = dockMicMuted ? "Mikrofonu aç" : "Mikrofonu kapat";
        if (horizon::IconButton("footerMicrophone",
                                dockMicMuted ? horizon::Icon::MicrophoneOff : horizon::Icon::MicrophoneOn,
                                micOptions)) {
            ToggleMicrophoneMuted();
        }
        ImGui::SameLine(0.0F, footerGap);
        const bool dockOutputMuted = audio_.IsOutputMuted();
        horizon::IconButtonOptions outputOptions = micOptions;
        outputOptions.selected = dockOutputMuted;
        outputOptions.tooltip = dockOutputMuted ? "Tüm sesi aç" : "Tüm sesi kapat";
        if (horizon::IconButton("footerOutput",
                                dockOutputMuted ? horizon::Icon::HeadphonesOff : horizon::Icon::HeadphonesOn,
                                outputOptions)) {
            ToggleOutputMuted();
        }
        ImGui::SameLine(0.0F, footerGap);
        horizon::IconButtonOptions settingsOptions = micOptions;
        settingsOptions.selected = activePage_ == ClientPage::Settings;
        settingsOptions.tooltip = "Ayarlar";
        if (horizon::IconButton("footerSettings", horizon::Icon::Settings, settingsOptions)) {
            activePage_ = ClientPage::Settings;
        }

        ImGui::TableNextColumn();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0F * dpiScale);
        const horizon::Icon routeIcon = network_.ActiveVoicePath() == VoicePath::DirectPeer
            ? horizon::Icon::P2P : horizon::Icon::Relay;
        const ImVec2 routeStart = ImGui::GetCursorScreenPos();
        horizon::DrawIcon(ImGui::GetWindowDrawList(), routeIcon,
                          ImVec2(routeStart.x + 11.0F * dpiScale, routeStart.y + 11.0F * dpiScale),
                          19.0F * dpiScale, ImGui::GetColorU32(statusColor));
        ImGui::Dummy(ImVec2(27.0F * dpiScale, 22.0F * dpiScale));
        ImGui::SameLine();
        const char* routeLabel = connectedOrConnecting
            ? (network_.ActiveVoicePath() == VoicePath::DirectPeer ? "P2P ses bağlantısı" : "Sunucu relay ses bağlantısı")
            : T(TextId::Ready);
        const std::string fittedRouteLabel = FitTextToWidth(routeLabel,
            std::max(20.0F * dpiScale, ImGui::GetContentRegionAvail().x - 8.0F * dpiScale));
        ImGui::TextColored(statusColor, "%s", fittedRouteLabel.c_str());
        if (ImGui::IsItemHovered() && fittedRouteLabel != routeLabel) ImGui::SetTooltip("%s", routeLabel);

        ImGui::TableNextColumn();
        if (updateBusy) ImGui::BeginDisabled();
        if (updateAccent) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10F, 0.45F, 0.74F, 1.0F));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.12F, 0.55F, 0.88F, 1.0F));
        }
        const std::string fittedUpdateLabel = FitTextToWidth(updateLabel,
            updateButtonWidth - 22.0F * dpiScale);
        const bool updatePressed = ImGui::Button((fittedUpdateLabel + "##updateDock").c_str(),
                                                 ImVec2(updateButtonWidth, controlSize));
        if (updateAccent) ImGui::PopStyleColor(2);
        if (updateBusy) ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && updateState == UpdateState::Error) {
            ImGui::SetTooltip("%s\nTekrar denemek için tıklayın.", updater_.Status().c_str());
        } else if (ImGui::IsItemHovered() && fittedUpdateLabel != updateLabel) {
            ImGui::SetTooltip("%s", updateLabel.c_str());
        }
        if (updatePressed) {
            if (updateState == UpdateState::Available) updateDownloadPending_ = updater_.DownloadAsync();
            else if (updateState == UpdateState::Ready) updateInstallPromptPending_ = true;
            else manualUpdateCheckPending_ = updater_.CheckAsync(settings_.controlOrigin, true);
        }
        ImGui::EndTable();
    }

    if (ImGui::BeginPopup("accountMenu")) {
        ImGui::TextUnformatted(account.nickname.c_str());
        ImGui::TextDisabled("@%s", account.username.c_str());
        ImGui::Separator();
        if (ImGui::BeginMenu("Durum")) {
            constexpr std::array<std::pair<const char*, const char*>, 4> values{{
                {"Çevrimiçi", "online"}, {"Boşta", "idle"},
                {"Rahatsız etmeyin", "do_not_disturb"}, {"Görünmez", "invisible"},
            }};
            for (const auto& [label, value] : values) {
                if (ImGui::MenuItem(label, nullptr, ownPresence_ == value)) {
                    ownPresence_ = value;
                    realtime_.SetPresence(ownPresence_, customStatusBuffer_.data());
                }
            }
            ImGui::EndMenu();
        }
        ImGui::SetNextItemWidth(240.0F * dpiScale);
        ImGui::InputTextWithHint("##customStatus", "Özel durum", customStatusBuffer_.data(), customStatusBuffer_.size());
        if (ImGui::SmallButton("Durumu kaydet")) realtime_.SetPresence(ownPresence_, customStatusBuffer_.data());
        ImGui::Separator();
        if (ImGui::MenuItem(T(TextId::Logout))) {
            platform_.Logout();
            terminalSessionHandled_ = false;
            ResetSessionView({});
        }
        if (ImGui::MenuItem(T(TextId::ExitApplication))) {
            PostMessageW(windowHandle_, kMessageExitApplication, 0, 0);
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    if (updateInstallPromptPending_) {
        ImGui::OpenPopup("Guncelleme hazir");
        updateInstallPromptPending_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
    if (ImGui::BeginPopupModal("Guncelleme hazir", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        const auto available = updater_.Available();
        ImGui::Text("Sonalis %s indirildi ve imzasi dogrulandi.",
                    available ? available->version.c_str() : "guncellemesi");
        if (connectedOrConnecting) {
            ImGui::TextColored(ImVec4(1.0F, 0.68F, 0.28F, 1.0F),
                               "Kurulum ses baglantisini kapatacaktir.");
        }
        ImGui::TextDisabled("Kurulum yalniz onayinizdan sonra baslatilir.");
        ImGui::Spacing();
        if (ImGui::Button(T(TextId::Later), ImVec2(120.0F * dpiScale, 34.0F * dpiScale))) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10F, 0.45F, 0.74F, 1.0F));
        if (ImGui::Button(T(TextId::StartInstallation), ImVec2(150.0F * dpiScale, 34.0F * dpiScale))) {
            Disconnect();
            realtime_.Stop();
            SaveSettings();
            std::string error;
            if (updater_.LaunchInstaller(error)) PostMessageW(windowHandle_, kMessageExitApplication, 0, 0);
            else uiMessage_ = error;
        }
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }
    // Navigation is rendered after the header. In the event-driven loop a page
    // change can otherwise leave the old header visible until another external
    // event arrives.
    if (activePage_ != pageAtFrameStart && redrawEvent_ != nullptr) SetEvent(redrawEvent_);
    ImGui::End();
}

void AppUi::RenderStartupUpdateGate(const float dpiScale) {
    voiceMeterVisible_.store(false, std::memory_order_relaxed);
    const UpdateState state = updater_.State();
    const auto available = updater_.Available();
    const ImVec2 availableSize = ImGui::GetContentRegionAvail();
    const float panelWidth = std::min(520.0F * dpiScale, std::max(320.0F * dpiScale, availableSize.x - 40.0F * dpiScale));
    const float panelHeight = 250.0F * dpiScale;
    ImGui::SetCursorPos(ImVec2(std::max(20.0F * dpiScale, (availableSize.x - panelWidth) * 0.5F),
                              std::max(20.0F * dpiScale, (availableSize.y - panelHeight) * 0.5F)));
    ImGui::BeginChild("startup-update-gate", ImVec2(panelWidth, panelHeight), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("Sonalis guncelleniyor");
    ImGui::Spacing();
    if (state == UpdateState::Checking || state == UpdateState::Idle) {
        ImGui::TextWrapped("Guvenli bir oturum baslatmadan once surum denetleniyor.");
        ImGui::ProgressBar(-1.0F, ImVec2(-1.0F, 0.0F), "Denetleniyor");
    } else if (state == UpdateState::Available || state == UpdateState::Downloading) {
        ImGui::TextWrapped("Sonalis %s indiriliyor. Paket SHA-256, Ed25519 ve kanal guvenlik politikasiyla dogrulanacak.",
                           available ? available->version.c_str() : "guncellemesi");
        ImGui::ProgressBar(-1.0F, ImVec2(-1.0F, 0.0F), "Indiriliyor ve dogrulaniyor");
    } else if (state == UpdateState::Ready && startupUpdateFailure_.empty()) {
        ImGui::TextWrapped("Dogrulanan kurulum baslatiliyor. Sonalis otomatik olarak kapanacak.");
        ImGui::ProgressBar(-1.0F, ImVec2(-1.0F, 0.0F), "Kurulum baslatiliyor");
    } else {
        const std::string detail = startupUpdateFailure_.empty() ? updater_.Status() : startupUpdateFailure_;
        ImGui::TextColored(ImVec4(1.0F, 0.38F, 0.38F, 1.0F), "Guncelleme tamamlanamadi");
        ImGui::TextWrapped("%s", detail.c_str());
        ImGui::Spacing();
        if (state == UpdateState::Ready) {
            if (ImGui::Button("Kurulumu yeniden baslat")) {
                startupUpdateFailure_.clear();
                startupInstallerLaunchAttempted_ = false;
            }
        } else if (ImGui::Button("Yeniden denetle")) {
            startupUpdateFailure_.clear();
            startupInstallerLaunchAttempted_ = false;
            if (!updater_.CheckAsync(settings_.controlOrigin, true)) {
                startupUpdateFailure_ = "Guncelleme denetimi baslatilamadi";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Sonalis'ten cik")) {
            PostMessageW(windowHandle_, kMessageExitApplication, 0, 0);
        }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Uygulama yalniz guncel ve dogrulanmis surumle devam eder.");
    ImGui::EndChild();
}

bool AppUi::IsActive() const noexcept {
    return network_.State() == ConnectionState::Connected || network_.State() == ConnectionState::Connecting;
}

std::uint32_t AppUi::RefreshIntervalMs(const bool focused, const bool visible) const noexcept {
    std::uint32_t refresh = INFINITE;
    const std::uint64_t now = SteadyNowMs();
    const auto includeDeadline = [&refresh, now](const std::uint64_t deadline) {
        if (deadline <= now) {
            refresh = 0U;
            return;
        }
        const std::uint64_t remaining = deadline - now;
        refresh = std::min(refresh, static_cast<std::uint32_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(INFINITE - 1U))));
    };
    if (!visible) {
        if (notificationIconVisible_) includeDeadline(notificationIconExpiresMs_);
        if (!realtime_.IsConnected() && platform_.IsAuthenticated()) {
            const std::uint64_t interval = chatConversationId_.empty() ? 60'000U : 15'000U;
            includeDeadline(lastFallbackSyncMs_ + interval);
        }
    } else if (!focused) {
        refresh = static_cast<std::uint32_t>(1'000 / std::max(1, experiencePolicy_.unfocusedFps));
    }
    else if (platform_.IsAuthenticated() && !realtime_.IsConnected()) refresh = 1'000U;
    else if (network_.State() == ConnectionState::Connecting) refresh = 66U;
    // Meter callbacks wake the event-driven UI at most 20 times per second.
    // This 10 FPS deadline is only a loss-tolerant fallback for missed events.
    else if (audio_.IsTransmitting() || audio_.HasActivePeerAudio()) {
        const auto policyFrameMs = static_cast<std::uint32_t>(
            1'000 / std::max(1, experiencePolicy_.focusedVoiceFps));
        refresh = std::max(BudgetFor(settings_.resourceProfile).focusedVoiceFrameMs, policyFrameMs);
    }
    if (platform_.IsAuthenticated() && realtime_.IsConnected()) includeDeadline(lastPresenceCheckMs_ + 60'000U);
    return std::min(refresh, updater_.MillisecondsUntilNextCheck());
}

HANDLE AppUi::RedrawEvent() const noexcept { return redrawEvent_; }
void AppUi::ConsumeRedraw() noexcept { if (redrawEvent_ != nullptr) ResetEvent(redrawEvent_); }

}  // namespace ss
