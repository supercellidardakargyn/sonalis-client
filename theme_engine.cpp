#include "theme_engine.h"

#include <algorithm>

#include <imgui.h>

namespace ss {
namespace {

struct Palette final {
    ImVec4 text;
    ImVec4 disabled;
    ImVec4 window;
    ImVec4 child;
    ImVec4 popup;
    ImVec4 border;
    ImVec4 frame;
    ImVec4 frameHovered;
    ImVec4 frameActive;
    ImVec4 button;
    ImVec4 buttonHovered;
    ImVec4 buttonActive;
    ImVec4 accent;
    ImVec4 accentHovered;
    ImVec4 accentActive;
    ImVec4 positive;
    ImVec4 warning;
    ImVec4 selection;
};

[[nodiscard]] constexpr ImVec4 Mix(const ImVec4& left, const ImVec4& right,
                                   const float amount) noexcept {
    return ImVec4(left.x + (right.x - left.x) * amount,
                  left.y + (right.y - left.y) * amount,
                  left.z + (right.z - left.z) * amount,
                  left.w + (right.w - left.w) * amount);
}

[[nodiscard]] constexpr float Luminance(const ImVec4& color) noexcept {
    return color.x * 0.2126F + color.y * 0.7152F + color.z * 0.0722F;
}

[[nodiscard]] Palette PaletteFor(const UiTheme theme) noexcept {
    switch (theme) {
    case UiTheme::Classic:
        // Graphite keeps the quieter character of the original profile while
        // using contemporary layered surfaces and a restrained violet-indigo
        // accent instead of legacy cobalt blocks.
        return {
            {0.945F, 0.950F, 0.970F, 1.0F}, {0.57F, 0.59F, 0.67F, 1.0F},
            {0.032F, 0.036F, 0.050F, 1.0F}, {0.050F, 0.056F, 0.076F, 1.0F},
            {0.066F, 0.073F, 0.098F, 1.0F}, {0.135F, 0.145F, 0.185F, 1.0F},
            {0.070F, 0.077F, 0.104F, 1.0F}, {0.105F, 0.115F, 0.155F, 1.0F},
            {0.138F, 0.151F, 0.205F, 1.0F}, {0.067F, 0.074F, 0.100F, 1.0F},
            {0.104F, 0.114F, 0.154F, 1.0F}, {0.138F, 0.151F, 0.205F, 1.0F},
            {0.400F, 0.350F, 0.920F, 0.88F}, {0.515F, 0.455F, 1.0F, 1.0F},
            {0.315F, 0.275F, 0.810F, 1.0F}, {0.14F, 0.80F, 0.64F, 1.0F},
            {0.96F, 0.66F, 0.20F, 1.0F}, {0.430F, 0.370F, 0.960F, 0.28F},
        };
    case UiTheme::AuroraLight:
        return {
            {0.070F, 0.085F, 0.125F, 1.0F}, {0.38F, 0.42F, 0.50F, 1.0F},
            {0.948F, 0.958F, 0.978F, 1.0F}, {0.980F, 0.985F, 0.996F, 1.0F},
            {0.995F, 0.997F, 1.0F, 1.0F}, {0.72F, 0.76F, 0.84F, 1.0F},
            {0.900F, 0.915F, 0.950F, 1.0F}, {0.840F, 0.875F, 0.945F, 1.0F},
            {0.785F, 0.835F, 0.930F, 1.0F}, {0.885F, 0.905F, 0.945F, 1.0F},
            {0.815F, 0.855F, 0.930F, 1.0F}, {0.735F, 0.790F, 0.905F, 1.0F},
            {0.315F, 0.250F, 0.86F, 0.86F}, {0.205F, 0.390F, 0.94F, 1.0F},
            {0.155F, 0.285F, 0.82F, 1.0F}, {0.025F, 0.61F, 0.43F, 1.0F},
            {0.84F, 0.48F, 0.06F, 1.0F}, {0.245F, 0.39F, 0.91F, 0.25F},
        };
    case UiTheme::Oled:
        // Real black is retained only in OLED. Raised surfaces remain subtly
        // visible without turning the whole UI into outlined boxes.
        return {
            {0.94F, 0.96F, 0.99F, 1.0F}, {0.56F, 0.61F, 0.69F, 1.0F},
            {0.0F, 0.0F, 0.0F, 1.0F}, {0.012F, 0.014F, 0.019F, 1.0F},
            {0.024F, 0.028F, 0.038F, 1.0F}, {0.16F, 0.18F, 0.23F, 1.0F},
            {0.030F, 0.035F, 0.047F, 1.0F}, {0.065F, 0.082F, 0.110F, 1.0F},
            {0.090F, 0.120F, 0.165F, 1.0F}, {0.035F, 0.042F, 0.055F, 1.0F},
            {0.075F, 0.095F, 0.125F, 1.0F}, {0.095F, 0.135F, 0.185F, 1.0F},
            {0.00F, 0.54F, 0.88F, 0.84F}, {0.08F, 0.72F, 1.0F, 1.0F},
            {0.00F, 0.56F, 0.94F, 1.0F}, {0.08F, 0.85F, 0.58F, 1.0F},
            {1.0F, 0.69F, 0.18F, 1.0F}, {0.02F, 0.58F, 0.94F, 0.29F},
        };
    case UiTheme::Custom:
    case UiTheme::AuroraDark:
    default:
        // Aurora uses a deep blue-black canvas, layered slate surfaces and an
        // indigo accent. The contrast comes from depth rather than heavy
        // outlines or saturated full-width blue blocks.
        return {
            {0.955F, 0.965F, 0.985F, 1.0F}, {0.58F, 0.63F, 0.72F, 1.0F},
            {0.026F, 0.034F, 0.055F, 1.0F}, {0.052F, 0.067F, 0.105F, 1.0F},
            {0.070F, 0.089F, 0.136F, 1.0F}, {0.120F, 0.151F, 0.220F, 1.0F},
            {0.073F, 0.094F, 0.145F, 1.0F}, {0.102F, 0.132F, 0.205F, 1.0F},
            {0.135F, 0.176F, 0.274F, 1.0F}, {0.072F, 0.094F, 0.145F, 1.0F},
            {0.105F, 0.137F, 0.215F, 1.0F}, {0.140F, 0.184F, 0.290F, 1.0F},
            {0.345F, 0.405F, 0.965F, 0.90F}, {0.445F, 0.515F, 1.0F, 1.0F},
            {0.265F, 0.315F, 0.845F, 1.0F}, {0.10F, 0.83F, 0.70F, 1.0F},
            {0.99F, 0.70F, 0.24F, 1.0F}, {0.355F, 0.430F, 0.980F, 0.28F},
        };
    }
}

void ApplyCustomAccent(Palette& palette, const float red, const float green,
                       const float blue) noexcept {
    ImVec4 custom(std::clamp(red, 0.0F, 1.0F),
                  std::clamp(green, 0.0F, 1.0F),
                  std::clamp(blue, 0.0F, 1.0F), 1.0F);
    // Keep a user-selected near-black accent readable on Horizon's dark base.
    if (Luminance(custom) < 0.30F) {
        custom = Mix(custom, ImVec4(1.0F, 1.0F, 1.0F, 1.0F), 0.45F);
    }
    palette.accent = ImVec4(custom.x, custom.y, custom.z, 0.78F);
    palette.accentHovered = Mix(custom, ImVec4(1.0F, 1.0F, 1.0F, 1.0F), 0.16F);
    palette.accentActive = Mix(custom, ImVec4(0.0F, 0.0F, 0.0F, 1.0F), 0.14F);
    palette.selection = ImVec4(custom.x, custom.y, custom.z, 0.30F);
}

void ApplyHighContrast(Palette& palette, const bool lightTheme) noexcept {
    if (lightTheme) {
        palette.text = ImVec4(0.0F, 0.0F, 0.0F, 1.0F);
        palette.disabled = ImVec4(0.22F, 0.22F, 0.22F, 1.0F);
        palette.window = ImVec4(1.0F, 1.0F, 1.0F, 1.0F);
        palette.child = ImVec4(0.975F, 0.975F, 0.975F, 1.0F);
        palette.popup = ImVec4(1.0F, 1.0F, 1.0F, 1.0F);
        palette.border = ImVec4(0.08F, 0.08F, 0.08F, 1.0F);
        palette.frame = ImVec4(0.90F, 0.91F, 0.93F, 1.0F);
        palette.frameHovered = ImVec4(0.79F, 0.84F, 0.94F, 1.0F);
        palette.frameActive = ImVec4(0.70F, 0.78F, 0.92F, 1.0F);
        palette.button = ImVec4(0.87F, 0.88F, 0.91F, 1.0F);
        palette.buttonHovered = ImVec4(0.75F, 0.80F, 0.91F, 1.0F);
        palette.buttonActive = ImVec4(0.64F, 0.72F, 0.88F, 1.0F);
        palette.accent = ImVec4(0.04F, 0.24F, 0.72F, 1.0F);
        palette.accentHovered = ImVec4(0.02F, 0.31F, 0.90F, 1.0F);
        palette.accentActive = ImVec4(0.01F, 0.17F, 0.58F, 1.0F);
    } else {
        palette.text = ImVec4(1.0F, 1.0F, 1.0F, 1.0F);
        palette.disabled = ImVec4(0.76F, 0.78F, 0.82F, 1.0F);
        palette.window = ImVec4(0.0F, 0.0F, 0.0F, 1.0F);
        palette.child = ImVec4(0.018F, 0.018F, 0.018F, 1.0F);
        palette.popup = ImVec4(0.025F, 0.025F, 0.025F, 1.0F);
        palette.border = ImVec4(0.74F, 0.78F, 0.86F, 1.0F);
        palette.frame = ImVec4(0.075F, 0.085F, 0.105F, 1.0F);
        palette.frameHovered = ImVec4(0.14F, 0.17F, 0.22F, 1.0F);
        palette.frameActive = ImVec4(0.20F, 0.25F, 0.34F, 1.0F);
        palette.button = ImVec4(0.085F, 0.095F, 0.12F, 1.0F);
        palette.buttonHovered = ImVec4(0.16F, 0.19F, 0.25F, 1.0F);
        palette.buttonActive = ImVec4(0.23F, 0.29F, 0.39F, 1.0F);
        palette.accent = ImVec4(0.08F, 0.58F, 1.0F, 1.0F);
        palette.accentHovered = ImVec4(0.28F, 0.72F, 1.0F, 1.0F);
        palette.accentActive = ImVec4(0.04F, 0.43F, 0.86F, 1.0F);
    }
    palette.selection = ImVec4(palette.accentHovered.x, palette.accentHovered.y,
                               palette.accentHovered.z, 0.42F);
}

}  // namespace

ResourceBudget BudgetFor(const ResourceProfile profile) noexcept {
    constexpr std::size_t mib = 1024U * 1024U;
    switch (profile) {
    case ResourceProfile::Economy:
        return {0U, 60U * mib, 72U * mib, 120U, 100U, false, false, false};
    case ResourceProfile::Visual:
        return {32U * mib, 140U * mib, 160U * mib, 300U, 66U, true, true, true};
    case ResourceProfile::Balanced:
    default:
        return {16U * mib, 90U * mib, 108U * mib, 300U, 66U, true, true, false};
    }
}

void ApplyClientTheme(ImGuiStyle& style,
                      const UiTheme theme,
                      const ResourceProfile profile,
                      const bool rightToLeft,
                      const float dpiScale,
                      const float textScale,
                      const int density,
                      const bool highContrast,
                      const int colorVisionMode,
                      const float customAccentR,
                      const float customAccentG,
                      const float customAccentB) noexcept {
    // ConfigureFontsAndStyle resets ImGuiStyle before calling us. Define all
    // base sizes first and call ScaleAllSizes exactly once so monitor/DPI/theme
    // changes cannot compound prior scaling.
    ImGui::StyleColorsDark(&style);
    Palette palette = PaletteFor(theme);
    if (theme == UiTheme::Custom) {
        ApplyCustomAccent(palette, customAccentR, customAccentG, customAccentB);
    }
    if (highContrast) {
        ApplyHighContrast(palette, theme == UiTheme::AuroraLight);
    }

    const float densityScale = density <= 0 ? 0.92F : (density >= 2 ? 1.14F : 1.0F);
    const float scale = std::clamp(dpiScale, 0.75F, 4.0F)
        * std::clamp(textScale, 0.85F, 1.50F);
    const bool classic = theme == UiTheme::Classic;
    const bool oled = theme == UiTheme::Oled;

    // Restrained radii keep dense channel/message lists crisp and modern. The
    // visual profile may animate state changes elsewhere, but does not inflate
    // geometry or change hit targets.
    style.WindowRounding = classic ? 3.0F : (oled ? 7.0F : 9.0F);
    style.ChildRounding = classic ? 4.0F : (oled ? 8.0F : 10.0F);
    style.PopupRounding = classic ? 4.0F : 10.0F;
    style.FrameRounding = classic ? 4.0F : 8.0F;
    style.GrabRounding = classic ? 3.0F : 8.0F;
    style.ScrollbarRounding = classic ? 3.0F : 9.0F;
    style.TabRounding = classic ? 4.0F : 8.0F;
    style.ImageRounding = classic ? 3.0F : 8.0F;

    style.WindowPadding = ImVec2(16.0F * densityScale, 14.0F * densityScale);
    style.FramePadding = ImVec2(11.0F * densityScale, 7.0F * densityScale);
    style.ItemSpacing = ImVec2(9.0F * densityScale, 9.0F * densityScale);
    style.ItemInnerSpacing = ImVec2(8.0F * densityScale, 6.0F * densityScale);
    style.CellPadding = ImVec2(9.0F * densityScale, 7.0F * densityScale);
    style.IndentSpacing = 20.0F * densityScale;
    style.ColumnsMinSpacing = 8.0F * densityScale;
    style.ScrollbarSize = profile == ResourceProfile::Economy ? 11.0F : 12.0F;
    style.ScrollbarPadding = 1.0F;
    style.GrabMinSize = 10.0F;
    style.TabMinWidthBase = 64.0F;
    style.SeparatorTextPadding = ImVec2(12.0F * densityScale, 6.0F * densityScale);

    style.WindowBorderSize = highContrast ? 2.0F : 1.0F;
    style.ChildBorderSize = highContrast ? 1.5F : 1.0F;
    style.PopupBorderSize = highContrast ? 2.0F : 1.0F;
    style.FrameBorderSize = highContrast ? 1.0F : 0.0F;
    style.TabBorderSize = 0.0F;
    style.TabBarBorderSize = 1.0F;
    style.TabBarOverlineSize = highContrast ? 3.0F : 2.0F;
    style.SeparatorSize = highContrast ? 2.0F : 1.0F;
    style.SeparatorTextBorderSize = highContrast ? 2.0F : 1.0F;
    style.DragDropTargetBorderSize = highContrast ? 3.0F : 2.0F;

    style.ButtonTextAlign = rightToLeft ? ImVec2(1.0F, 0.5F) : ImVec2(0.5F, 0.5F);
    // Selectables with an explicit row height must keep text vertically
    // centred. Top alignment made highlighted rows look clipped and caused
    // labels to sit directly against the colored boundary.
    style.SelectableTextAlign = rightToLeft ? ImVec2(1.0F, 0.5F) : ImVec2(0.0F, 0.5F);
    style.WindowTitleAlign = rightToLeft ? ImVec2(1.0F, 0.5F) : ImVec2(0.0F, 0.5F);
    style.SeparatorTextAlign = rightToLeft ? ImVec2(1.0F, 0.5F) : ImVec2(0.0F, 0.5F);
    style.WindowMenuButtonPosition = rightToLeft ? ImGuiDir_Right : ImGuiDir_Left;
    style.ColorButtonPosition = rightToLeft ? ImGuiDir_Left : ImGuiDir_Right;
    style.DisabledAlpha = highContrast ? 0.82F : 0.64F;

    // Main resets to ImGuiStyle{} before every invocation; a single scaling
    // pass here is therefore idempotent across DPI and theme transitions.
    style.ScaleAllSizes(scale);

    ImVec4* const colors = style.Colors;
    colors[ImGuiCol_Text] = palette.text;
    colors[ImGuiCol_TextDisabled] = palette.disabled;
    colors[ImGuiCol_WindowBg] = palette.window;
    colors[ImGuiCol_ChildBg] = palette.child;
    colors[ImGuiCol_PopupBg] = palette.popup;
    colors[ImGuiCol_Border] = palette.border;
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    colors[ImGuiCol_FrameBg] = palette.frame;
    colors[ImGuiCol_FrameBgHovered] = palette.frameHovered;
    colors[ImGuiCol_FrameBgActive] = palette.frameActive;
    colors[ImGuiCol_TitleBg] = palette.window;
    colors[ImGuiCol_TitleBgActive] = palette.child;
    colors[ImGuiCol_TitleBgCollapsed] = palette.window;
    colors[ImGuiCol_MenuBarBg] = palette.child;
    colors[ImGuiCol_ScrollbarBg] = ImVec4(palette.window.x, palette.window.y, palette.window.z, 0.55F);
    colors[ImGuiCol_ScrollbarGrab] = Mix(palette.frame, palette.border, 0.38F);
    colors[ImGuiCol_ScrollbarGrabHovered] = palette.frameHovered;
    colors[ImGuiCol_ScrollbarGrabActive] = palette.accentHovered;
    colors[ImGuiCol_Button] = palette.button;
    colors[ImGuiCol_ButtonHovered] = palette.buttonHovered;
    colors[ImGuiCol_ButtonActive] = palette.buttonActive;
    colors[ImGuiCol_Header] = palette.accent;
    colors[ImGuiCol_HeaderHovered] = palette.accentHovered;
    colors[ImGuiCol_HeaderActive] = palette.accentActive;

    const ImVec4 accessibleMark = colorVisionMode == 1
        ? ImVec4(1.0F, 0.76F, 0.16F, 1.0F)
        : palette.accentHovered;
    colors[ImGuiCol_CheckMark] = accessibleMark;
    colors[ImGuiCol_CheckboxSelectedBg] = palette.accent;
    colors[ImGuiCol_SliderGrab] = accessibleMark;
    colors[ImGuiCol_SliderGrabActive] = palette.accentActive;
    colors[ImGuiCol_InputTextCursor] = accessibleMark;

    colors[ImGuiCol_Separator] = palette.border;
    colors[ImGuiCol_SeparatorHovered] = palette.accentHovered;
    colors[ImGuiCol_SeparatorActive] = palette.accentActive;
    colors[ImGuiCol_ResizeGrip] = ImVec4(palette.accent.x, palette.accent.y, palette.accent.z, 0.16F);
    colors[ImGuiCol_ResizeGripHovered] = palette.accentHovered;
    colors[ImGuiCol_ResizeGripActive] = palette.accentActive;

    colors[ImGuiCol_Tab] = palette.button;
    colors[ImGuiCol_TabHovered] = palette.buttonHovered;
    colors[ImGuiCol_TabSelected] = palette.frameActive;
    colors[ImGuiCol_TabSelectedOverline] = palette.accentHovered;
    colors[ImGuiCol_TabDimmed] = palette.frame;
    colors[ImGuiCol_TabDimmedSelected] = palette.buttonActive;
    colors[ImGuiCol_TabDimmedSelectedOverline] = palette.border;

    colors[ImGuiCol_PlotLines] = palette.accentHovered;
    colors[ImGuiCol_PlotLinesHovered] = palette.warning;
    colors[ImGuiCol_PlotHistogram] = colorVisionMode == 2 ? palette.warning : palette.positive;
    colors[ImGuiCol_PlotHistogramHovered] = palette.warning;
    colors[ImGuiCol_TableHeaderBg] = palette.frame;
    colors[ImGuiCol_TableBorderStrong] = palette.border;
    colors[ImGuiCol_TableBorderLight] = ImVec4(palette.border.x, palette.border.y,
                                               palette.border.z, 0.56F);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(palette.frame.x, palette.frame.y,
                                            palette.frame.z, 0.38F);
    colors[ImGuiCol_TextLink] = palette.accentHovered;
    colors[ImGuiCol_TextSelectedBg] = palette.selection;
    colors[ImGuiCol_TreeLines] = palette.border;
    colors[ImGuiCol_DragDropTarget] = palette.warning;
    colors[ImGuiCol_DragDropTargetBg] = ImVec4(palette.warning.x, palette.warning.y,
                                               palette.warning.z, 0.12F);
    colors[ImGuiCol_UnsavedMarker] = palette.warning;

    // Keyboard focus is deliberately brighter than hover so mouse and keyboard
    // interaction remain visually distinguishable at every contrast setting.
    colors[ImGuiCol_NavCursor] = highContrast
        ? (theme == UiTheme::AuroraLight
            ? ImVec4(0.0F, 0.14F, 0.66F, 1.0F)
            : ImVec4(0.46F, 0.82F, 1.0F, 1.0F))
        : palette.accentHovered;
    colors[ImGuiCol_NavWindowingHighlight] = palette.accentHovered;
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.44F);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0F, 0.0F, 0.0F,
                                               theme == UiTheme::AuroraLight ? 0.30F : 0.60F);
}

}  // namespace ss
