#include "horizon_icons.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ss::horizon {
namespace {

constexpr float kPi = 3.14159265358979323846F;

struct GlyphCanvas final {
    ImDrawList* draw{};
    ImVec2 center{};
    float scale{1.0F};
    ImU32 color{};
    float stroke{1.5F};

    [[nodiscard]] ImVec2 Point(const float x, const float y) const noexcept {
        return ImVec2(center.x + (x - 12.0F) * scale,
                      center.y + (y - 12.0F) * scale);
    }

    [[nodiscard]] float Radius(const float value) const noexcept {
        return value * scale;
    }

    void Line(const float x1, const float y1, const float x2, const float y2) const noexcept {
        draw->AddLine(Point(x1, y1), Point(x2, y2), color, stroke);
    }

    void Circle(const float x, const float y, const float radius,
                const int segments = 16) const noexcept {
        draw->AddCircle(Point(x, y), Radius(radius), color, segments, stroke);
    }

    void FilledCircle(const float x, const float y, const float radius,
                      const int segments = 12) const noexcept {
        draw->AddCircleFilled(Point(x, y), Radius(radius), color, segments);
    }

    void Rect(const float x1, const float y1, const float x2, const float y2,
              const float rounding = 0.0F) const noexcept {
        draw->AddRect(Point(x1, y1), Point(x2, y2), color, Radius(rounding), stroke, 0);
    }

    template <std::size_t Count>
    void Polyline(const std::array<ImVec2, Count>& normalizedPoints,
                  const bool closed = false) const noexcept {
        std::array<ImVec2, Count> points{};
        for (std::size_t index = 0; index < Count; ++index) {
            points[index] = Point(normalizedPoints[index].x, normalizedPoints[index].y);
        }
        draw->AddPolyline(points.data(), static_cast<int>(Count), color, stroke,
                          closed ? ImDrawFlags_Closed : ImDrawFlags_None);
    }

    void Arc(const float x, const float y, const float radius, const float angleMinimum,
             const float angleMaximum, const int segments = 12) const noexcept {
        draw->PathArcTo(Point(x, y), Radius(radius), angleMinimum, angleMaximum, segments);
        draw->PathStroke(color, stroke, ImDrawFlags_None);
    }
};

void DrawHome(const GlyphCanvas& canvas) noexcept {
    constexpr std::array<ImVec2, 5> outline{{
        ImVec2(3.0F, 11.0F), ImVec2(12.0F, 3.5F), ImVec2(21.0F, 11.0F),
        ImVec2(19.0F, 20.5F), ImVec2(5.0F, 20.5F),
    }};
    canvas.Polyline(outline, true);
    canvas.Rect(10.0F, 14.0F, 14.0F, 20.5F, 0.7F);
}

void DrawMessages(const GlyphCanvas& canvas) noexcept {
    canvas.Rect(3.0F, 4.5F, 21.0F, 17.5F, 4.0F);
    constexpr std::array<ImVec2, 3> tail{{
        ImVec2(8.5F, 17.5F), ImVec2(5.5F, 21.0F), ImVec2(12.0F, 17.5F),
    }};
    canvas.Polyline(tail, false);
    canvas.FilledCircle(8.0F, 11.0F, 1.0F);
    canvas.FilledCircle(12.0F, 11.0F, 1.0F);
    canvas.FilledCircle(16.0F, 11.0F, 1.0F);
}

void DrawCommunity(const GlyphCanvas& canvas) noexcept {
    canvas.Circle(9.0F, 8.0F, 3.0F, 14);
    canvas.Circle(16.8F, 9.0F, 2.3F, 12);
    canvas.Arc(9.0F, 18.5F, 6.0F, 3.55F, 5.87F, 14);
    canvas.Arc(16.6F, 18.0F, 4.5F, 3.75F, 5.76F, 12);
}

void DrawTextChannel(const GlyphCanvas& canvas) noexcept {
    canvas.Line(8.0F, 3.5F, 6.0F, 20.5F);
    canvas.Line(16.5F, 3.5F, 14.5F, 20.5F);
    canvas.Line(3.5F, 9.0F, 20.5F, 9.0F);
    canvas.Line(3.0F, 15.0F, 20.0F, 15.0F);
}

void DrawVoiceChannel(const GlyphCanvas& canvas) noexcept {
    constexpr std::array<ImVec2, 6> speaker{{
        ImVec2(4.0F, 10.0F), ImVec2(8.0F, 10.0F), ImVec2(12.5F, 6.0F),
        ImVec2(12.5F, 18.0F), ImVec2(8.0F, 14.0F), ImVec2(4.0F, 14.0F),
    }};
    canvas.Polyline(speaker, true);
    canvas.Arc(13.0F, 12.0F, 4.0F, -0.85F, 0.85F, 10);
    canvas.Arc(13.0F, 12.0F, 7.0F, -0.75F, 0.75F, 12);
}

void DrawSettings(const GlyphCanvas& canvas) noexcept {
    canvas.Circle(12.0F, 12.0F, 6.0F, 20);
    canvas.Circle(12.0F, 12.0F, 2.2F, 12);
    for (int index = 0; index < 8; ++index) {
        const float angle = static_cast<float>(index) * kPi * 0.25F;
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        canvas.Line(12.0F + cosine * 7.0F, 12.0F + sine * 7.0F,
                    12.0F + cosine * 9.5F, 12.0F + sine * 9.5F);
    }
}

void DrawMicrophone(const GlyphCanvas& canvas, const bool muted) noexcept {
    canvas.Rect(8.5F, 3.0F, 15.5F, 14.0F, 3.5F);
    canvas.Arc(12.0F, 12.5F, 7.0F, 0.15F, 2.99F, 14);
    canvas.Line(12.0F, 19.5F, 12.0F, 22.0F);
    canvas.Line(8.0F, 22.0F, 16.0F, 22.0F);
    if (muted) canvas.Line(3.0F, 3.0F, 21.0F, 21.0F);
}

void DrawHeadphones(const GlyphCanvas& canvas, const bool muted) noexcept {
    canvas.Arc(12.0F, 12.0F, 8.5F, 3.30F, 6.12F, 20);
    canvas.Rect(3.0F, 11.0F, 7.0F, 19.0F, 2.0F);
    canvas.Rect(17.0F, 11.0F, 21.0F, 19.0F, 2.0F);
    if (muted) canvas.Line(3.0F, 3.0F, 21.0F, 21.0F);
}

void DrawFriends(const GlyphCanvas& canvas) noexcept {
    canvas.Circle(9.0F, 8.0F, 3.0F, 14);
    canvas.Circle(17.0F, 9.0F, 2.3F, 12);
    canvas.Arc(9.0F, 19.0F, 6.5F, 3.50F, 5.93F, 14);
    canvas.Arc(17.0F, 18.5F, 4.2F, 3.72F, 5.72F, 12);
}

void DrawInvite(const GlyphCanvas& canvas) noexcept {
    canvas.Circle(8.5F, 8.0F, 3.0F, 14);
    canvas.Arc(8.5F, 19.0F, 6.0F, 3.50F, 5.93F, 14);
    canvas.Line(17.5F, 8.0F, 17.5F, 16.0F);
    canvas.Line(13.5F, 12.0F, 21.5F, 12.0F);
}

void DrawShield(const GlyphCanvas& canvas) noexcept {
    constexpr std::array<ImVec2, 6> shield{{
        ImVec2(12.0F, 2.5F), ImVec2(20.0F, 5.5F), ImVec2(19.0F, 14.5F),
        ImVec2(12.0F, 21.5F), ImVec2(5.0F, 14.5F), ImVec2(4.0F, 5.5F),
    }};
    canvas.Polyline(shield, true);
    constexpr std::array<ImVec2, 3> check{{
        ImVec2(8.0F, 12.0F), ImVec2(11.0F, 15.0F), ImVec2(16.5F, 9.0F),
    }};
    canvas.Polyline(check, false);
}

void DrawNotification(const GlyphCanvas& canvas) noexcept {
    canvas.Arc(12.0F, 12.5F, 7.0F, 3.42F, 6.00F, 16);
    canvas.Line(5.3F, 10.6F, 5.3F, 15.5F);
    canvas.Line(18.7F, 10.6F, 18.7F, 15.5F);
    canvas.Line(5.3F, 15.5F, 18.7F, 15.5F);
    canvas.Arc(12.0F, 16.0F, 3.0F, 0.35F, 2.79F, 8);
    canvas.Line(12.0F, 3.5F, 12.0F, 2.0F);
}

void DrawUpdate(const GlyphCanvas& canvas) noexcept {
    canvas.Arc(12.0F, 12.0F, 8.0F, -0.35F, 4.40F, 20);
    constexpr std::array<ImVec2, 3> arrow{{
        ImVec2(4.0F, 4.5F), ImVec2(4.0F, 10.0F), ImVec2(9.5F, 9.0F),
    }};
    canvas.Polyline(arrow, false);
}

void DrawConnection(const GlyphCanvas& canvas) noexcept {
    canvas.Arc(12.0F, 18.5F, 9.0F, 3.92F, 5.50F, 16);
    canvas.Arc(12.0F, 18.5F, 6.0F, 3.92F, 5.50F, 14);
    canvas.Arc(12.0F, 18.5F, 3.0F, 3.92F, 5.50F, 10);
    canvas.FilledCircle(12.0F, 18.5F, 1.2F);
}

void DrawP2P(const GlyphCanvas& canvas) noexcept {
    canvas.Circle(5.0F, 12.0F, 3.0F, 14);
    canvas.Circle(19.0F, 12.0F, 3.0F, 14);
    canvas.Line(8.0F, 10.5F, 16.0F, 10.5F);
    canvas.Line(8.0F, 13.5F, 16.0F, 13.5F);
    canvas.Line(13.5F, 8.0F, 16.0F, 10.5F);
    canvas.Line(10.5F, 16.0F, 8.0F, 13.5F);
}

void DrawRelay(const GlyphCanvas& canvas) noexcept {
    canvas.Circle(12.0F, 12.0F, 2.8F, 12);
    canvas.Circle(5.0F, 5.0F, 2.0F, 10);
    canvas.Circle(19.0F, 5.0F, 2.0F, 10);
    canvas.Circle(12.0F, 21.0F, 2.0F, 10);
    canvas.Line(7.0F, 7.0F, 10.0F, 10.0F);
    canvas.Line(17.0F, 7.0F, 14.0F, 10.0F);
    canvas.Line(12.0F, 14.8F, 12.0F, 19.0F);
}

void DrawSearch(const GlyphCanvas& canvas) noexcept {
    canvas.Circle(10.0F, 10.0F, 6.0F, 18);
    canvas.Line(14.5F, 14.5F, 21.0F, 21.0F);
}

void DrawChevron(const GlyphCanvas& canvas, const IconDirection direction) noexcept {
    std::array<ImVec2, 3> points{};
    switch (direction) {
    case IconDirection::Up:
        points = {ImVec2(5.0F, 15.0F), ImVec2(12.0F, 8.0F), ImVec2(19.0F, 15.0F)};
        break;
    case IconDirection::Down:
        points = {ImVec2(5.0F, 9.0F), ImVec2(12.0F, 16.0F), ImVec2(19.0F, 9.0F)};
        break;
    case IconDirection::Left:
        points = {ImVec2(15.0F, 5.0F), ImVec2(8.0F, 12.0F), ImVec2(15.0F, 19.0F)};
        break;
    case IconDirection::Right:
        points = {ImVec2(9.0F, 5.0F), ImVec2(16.0F, 12.0F), ImVec2(9.0F, 19.0F)};
        break;
    }
    canvas.Polyline(points, false);
}

}  // namespace

void DrawIcon(ImDrawList* const drawList, const Icon icon, const ImVec2& center,
              const float size, const ImU32 color, const float thickness,
              const IconDirection direction) noexcept {
    if (drawList == nullptr || size <= 0.0F || (color & IM_COL32_A_MASK) == 0U) return;

    const float scale = size / 24.0F;
    const float resolvedThickness = thickness > 0.0F
        ? thickness
        : std::clamp(1.65F * scale, 1.25F, 3.25F);
    const GlyphCanvas canvas{drawList, center, scale, color, resolvedThickness};

    switch (icon) {
    case Icon::Home: DrawHome(canvas); break;
    case Icon::Messages: DrawMessages(canvas); break;
    case Icon::Community: DrawCommunity(canvas); break;
    case Icon::TextChannel: DrawTextChannel(canvas); break;
    case Icon::VoiceChannel: DrawVoiceChannel(canvas); break;
    case Icon::Settings: DrawSettings(canvas); break;
    case Icon::MicrophoneOn: DrawMicrophone(canvas, false); break;
    case Icon::MicrophoneOff: DrawMicrophone(canvas, true); break;
    case Icon::HeadphonesOn: DrawHeadphones(canvas, false); break;
    case Icon::HeadphonesOff: DrawHeadphones(canvas, true); break;
    case Icon::Friends: DrawFriends(canvas); break;
    case Icon::Invite: DrawInvite(canvas); break;
    case Icon::Shield: DrawShield(canvas); break;
    case Icon::Notification: DrawNotification(canvas); break;
    case Icon::Update: DrawUpdate(canvas); break;
    case Icon::Connection: DrawConnection(canvas); break;
    case Icon::P2P: DrawP2P(canvas); break;
    case Icon::Relay: DrawRelay(canvas); break;
    case Icon::Search: DrawSearch(canvas); break;
    case Icon::Add:
        canvas.Line(12.0F, 4.0F, 12.0F, 20.0F);
        canvas.Line(4.0F, 12.0F, 20.0F, 12.0F);
        break;
    case Icon::Chevron: DrawChevron(canvas, direction); break;
    case Icon::More:
        canvas.FilledCircle(5.0F, 12.0F, 1.4F);
        canvas.FilledCircle(12.0F, 12.0F, 1.4F);
        canvas.FilledCircle(19.0F, 12.0F, 1.4F);
        break;
    }
}

bool IconButton(const char* const id, const Icon icon, const IconButtonOptions& options) {
    if (id == nullptr || id[0] == '\0') return false;

    const ImVec2 buttonSize(std::max(options.size.x, 1.0F),
                            std::max(options.size.y, 1.0F));
    const ImVec2 start = ImGui::GetCursorScreenPos();
    if (!options.enabled) ImGui::BeginDisabled();
    const bool pressed = ImGui::InvisibleButton(id, buttonSize);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool focused = ImGui::IsItemFocused();
    if (!options.enabled) ImGui::EndDisabled();

    const ImU32 normalBackground = options.backgroundColor != 0U
        ? options.backgroundColor
        : ImGui::GetColorU32(options.selected ? ImGuiCol_Header : ImGuiCol_Button);
    const ImU32 hoveredBackground = options.hoverColor != 0U
        ? options.hoverColor
        : ImGui::GetColorU32(ImGuiCol_ButtonHovered);
    const ImU32 heldBackground = options.activeColor != 0U
        ? options.activeColor
        : ImGui::GetColorU32(ImGuiCol_ButtonActive);
    const ImU32 background = held ? heldBackground : (hovered ? hoveredBackground : normalBackground);

    ImDrawList* const drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(start, ImVec2(start.x + buttonSize.x, start.y + buttonSize.y),
                            background, std::max(options.rounding, 0.0F));
    if (focused) {
        const ImU32 focusColor = ImGui::GetColorU32(ImGuiCol_NavCursor);
        drawList->AddRect(start, ImVec2(start.x + buttonSize.x, start.y + buttonSize.y),
                          focusColor, std::max(options.rounding, 0.0F), 1.5F, 0);
    }

    ImU32 iconColor = options.iconColor;
    if (iconColor == 0U) {
        iconColor = ImGui::GetColorU32(options.enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    }
    const ImVec2 center(start.x + buttonSize.x * 0.5F, start.y + buttonSize.y * 0.5F);
    const float available = std::max(1.0F, std::min(buttonSize.x, buttonSize.y) - 8.0F);
    DrawIcon(drawList, icon, center, std::clamp(options.iconSize, 1.0F, available), iconColor,
             0.0F, options.direction);

    if (hovered && options.tooltip != nullptr && options.tooltip[0] != '\0') {
        ImGui::SetTooltip("%s", options.tooltip);
    }
    return options.enabled && pressed;
}

}  // namespace ss::horizon
