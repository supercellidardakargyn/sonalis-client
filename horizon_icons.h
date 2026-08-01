#pragma once

#include <cstdint>

#include "imgui.h"

namespace ss::horizon {

// Sonalis Horizon uses small procedural glyphs instead of an icon font or
// texture atlas. Keeping the glyphs in one enum makes their visual language
// consistent and allows them to scale cleanly with the current DPI.
enum class Icon : std::uint8_t {
    Home,
    Messages,
    Community,
    TextChannel,
    VoiceChannel,
    Settings,
    MicrophoneOn,
    MicrophoneOff,
    HeadphonesOn,
    HeadphonesOff,
    Friends,
    Invite,
    Shield,
    Notification,
    Update,
    Connection,
    P2P,
    Relay,
    Search,
    Add,
    Chevron,
    More,
};

enum class IconDirection : std::uint8_t { Up, Right, Down, Left };

struct IconButtonOptions final {
    ImVec2 size{36.0F, 36.0F};
    float iconSize{20.0F};
    float rounding{9.0F};
    bool selected{false};
    bool enabled{true};
    IconDirection direction{IconDirection::Right};
    const char* tooltip{nullptr};

    // A value of zero selects the matching ImGui theme color.
    ImU32 iconColor{0};
    ImU32 backgroundColor{0};
    ImU32 hoverColor{0};
    ImU32 activeColor{0};
};

// Draws a glyph centered on `center`. `size` is the glyph's logical square,
// not a texture size, so it may be scaled directly with the UI DPI.
void DrawIcon(ImDrawList* drawList, Icon icon, const ImVec2& center, float size,
              ImU32 color, float thickness = 0.0F,
              IconDirection direction = IconDirection::Right) noexcept;

// Accessible hover/focus states, optional tooltip and theme-aware colors are
// kept in this helper so every icon-only action behaves consistently.
[[nodiscard]] bool IconButton(const char* id, Icon icon,
                              const IconButtonOptions& options = {});

}  // namespace ss::horizon
