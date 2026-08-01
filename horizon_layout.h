#pragma once

#include <cstdint>

#include "settings.h"

namespace ss {

// Horizon layout sizes are expressed in device-independent pixels (DIPs).
// The input viewport is the physical client area; dpiScale converts it to DIPs.
enum class HorizonLayoutClass : std::uint8_t {
    Compact,
    Standard,
    Wide,
    Studio,
    Ultra,
};

enum class HorizonDensity : std::uint8_t {
    Compact,
    Comfortable,
    Relaxed,
};

enum class PanelPresentation : std::uint8_t {
    Hidden,
    Inline,
    Drawer,
};

struct HorizonRect {
    float x{};
    float y{};
    float width{};
    float height{};
};

struct HorizonPanelLayout {
    HorizonRect bounds{};
    PanelPresentation presentation{PanelPresentation::Hidden};

    [[nodiscard]] constexpr bool IsVisible() const noexcept {
        return presentation != PanelPresentation::Hidden;
    }

    [[nodiscard]] constexpr bool IsInline() const noexcept {
        return presentation == PanelPresentation::Inline;
    }

    [[nodiscard]] constexpr bool IsDrawer() const noexcept {
        return presentation == PanelPresentation::Drawer;
    }
};

struct HorizonLayoutRequest {
    float viewportWidthPixels{1240.0F};
    float viewportHeightPixels{780.0F};
    float dpiScale{1.0F};
    HorizonDensity density{HorizonDensity::Comfortable};
    ResourceProfile resourceProfile{ResourceProfile::Balanced};
    bool channelPanelRequested{true};
    bool memberPanelRequested{true};
    bool utilityPanelRequested{false};
};

struct HorizonLayoutMetrics {
    HorizonLayoutClass layoutClass{HorizonLayoutClass::Standard};
    HorizonDensity density{HorizonDensity::Comfortable};
    ResourceProfile resourceProfile{ResourceProfile::Balanced};

    float dpiScale{1.0F};
    float logicalWidth{1240.0F};
    float logicalHeight{780.0F};
    float densityScale{1.0F};
    float outerPadding{};
    float panelGap{};
    float contentPadding{};
    float headerHeight{};
    float bottomBarHeight{};
    float minimumContentWidth{};
    float preferredMessageWidth{};
    float maximumMessageWidth{};

    HorizonPanelLayout communityRail{};
    HorizonPanelLayout channelPanel{};
    HorizonPanelLayout contentPanel{};
    HorizonPanelLayout memberPanel{};
    HorizonPanelLayout utilityPanel{};
    HorizonPanelLayout bottomBar{};

    bool showNavigationLabels{true};
    bool useTwoColumnHome{};
    bool useThreeColumnHome{};
    bool compactComposer{};
    bool reducedVisuals{};
};

[[nodiscard]] HorizonDensity HorizonDensityFromSetting(int density) noexcept;
[[nodiscard]] HorizonLayoutMetrics CalculateHorizonLayout(const HorizonLayoutRequest& request) noexcept;

}  // namespace ss
