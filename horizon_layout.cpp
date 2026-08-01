#include "horizon_layout.h"

#include <algorithm>
#include <cmath>

namespace ss {
namespace {

constexpr float kMinimumDpiScale = 1.0F;
constexpr float kMaximumDpiScale = 4.0F;
constexpr float kFallbackWidthPixels = 1240.0F;
constexpr float kFallbackHeightPixels = 780.0F;
constexpr float kMinimumViewportPixels = 1.0F;
constexpr float kMaximumViewportWidthPixels = 15360.0F;
constexpr float kMaximumViewportHeightPixels = 8640.0F;

[[nodiscard]] float FiniteOr(float value, float fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}

[[nodiscard]] float DensityScale(HorizonDensity density) noexcept {
    switch (density) {
    case HorizonDensity::Compact:
        return 0.94F;
    case HorizonDensity::Comfortable:
        return 1.0F;
    case HorizonDensity::Relaxed:
        return 1.12F;
    }
    return 1.0F;
}

[[nodiscard]] HorizonLayoutClass Classify(float logicalWidth) noexcept {
    if (logicalWidth < 1100.0F) {
        return HorizonLayoutClass::Compact;
    }
    if (logicalWidth < 1440.0F) {
        return HorizonLayoutClass::Standard;
    }
    if (logicalWidth < 2560.0F) {
        return HorizonLayoutClass::Wide;
    }
    if (logicalWidth < 5120.0F) {
        return HorizonLayoutClass::Studio;
    }
    return HorizonLayoutClass::Ultra;
}

[[nodiscard]] float ClassOuterPadding(HorizonLayoutClass layoutClass) noexcept {
    switch (layoutClass) {
    case HorizonLayoutClass::Compact:
        return 8.0F;
    case HorizonLayoutClass::Standard:
        return 10.0F;
    case HorizonLayoutClass::Wide:
        return 12.0F;
    case HorizonLayoutClass::Studio:
        return 16.0F;
    case HorizonLayoutClass::Ultra:
        return 20.0F;
    }
    return 10.0F;
}

[[nodiscard]] float ClassPanelGap(HorizonLayoutClass layoutClass) noexcept {
    switch (layoutClass) {
    case HorizonLayoutClass::Compact:
        return 5.0F;
    case HorizonLayoutClass::Standard:
        return 6.0F;
    case HorizonLayoutClass::Wide:
        return 8.0F;
    case HorizonLayoutClass::Studio:
        return 10.0F;
    case HorizonLayoutClass::Ultra:
        return 12.0F;
    }
    return 6.0F;
}

[[nodiscard]] float NavigationWidth(HorizonLayoutClass layoutClass) noexcept {
    switch (layoutClass) {
    case HorizonLayoutClass::Compact:
        return 404.0F;
    case HorizonLayoutClass::Standard:
        return 380.0F;
    case HorizonLayoutClass::Wide:
        return 424.0F;
    case HorizonLayoutClass::Studio:
        return 448.0F;
    case HorizonLayoutClass::Ultra:
        return 480.0F;
    }
    return 380.0F;
}

[[nodiscard]] float MemberWidth(HorizonLayoutClass layoutClass) noexcept {
    switch (layoutClass) {
    case HorizonLayoutClass::Compact:
        return 288.0F;
    case HorizonLayoutClass::Standard:
        return 240.0F;
    case HorizonLayoutClass::Wide:
        return 304.0F;
    case HorizonLayoutClass::Studio:
        return 328.0F;
    case HorizonLayoutClass::Ultra:
        return 360.0F;
    }
    return 288.0F;
}

[[nodiscard]] float UtilityWidth(HorizonLayoutClass layoutClass) noexcept {
    return layoutClass == HorizonLayoutClass::Ultra ? 400.0F : 344.0F;
}

[[nodiscard]] float MinimumContentWidth(HorizonLayoutClass layoutClass) noexcept {
    switch (layoutClass) {
    case HorizonLayoutClass::Compact:
        return 420.0F;
    case HorizonLayoutClass::Standard:
        return 540.0F;
    case HorizonLayoutClass::Wide:
        return 620.0F;
    case HorizonLayoutClass::Studio:
        return 720.0F;
    case HorizonLayoutClass::Ultra:
        return 840.0F;
    }
    return 540.0F;
}

[[nodiscard]] bool CanFitPanel(float remainingContentWidth,
                               float panelWidth,
                               float gap,
                               float minimumContentWidth) noexcept {
    return remainingContentWidth - panelWidth - gap >= minimumContentWidth;
}

[[nodiscard]] HorizonRect MakeRect(float x, float y, float width, float height) noexcept {
    return HorizonRect{x, y, std::max(0.0F, width), std::max(0.0F, height)};
}

}  // namespace

HorizonDensity HorizonDensityFromSetting(int density) noexcept {
    if (density <= 0) {
        return HorizonDensity::Compact;
    }
    if (density >= 2) {
        return HorizonDensity::Relaxed;
    }
    return HorizonDensity::Comfortable;
}

HorizonLayoutMetrics CalculateHorizonLayout(const HorizonLayoutRequest& request) noexcept {
    HorizonLayoutMetrics result{};

    const float dpiScale = std::clamp(FiniteOr(request.dpiScale, 1.0F),
                                      kMinimumDpiScale,
                                      kMaximumDpiScale);
    const float viewportWidthPixels = std::clamp(FiniteOr(request.viewportWidthPixels,
                                                          kFallbackWidthPixels),
                                                 kMinimumViewportPixels,
                                                 kMaximumViewportWidthPixels);
    const float viewportHeightPixels = std::clamp(FiniteOr(request.viewportHeightPixels,
                                                           kFallbackHeightPixels),
                                                  kMinimumViewportPixels,
                                                  kMaximumViewportHeightPixels);
    const float logicalWidth = viewportWidthPixels / dpiScale;
    const float logicalHeight = viewportHeightPixels / dpiScale;
    const HorizonLayoutClass layoutClass = Classify(logicalWidth);
    const float densityScale = DensityScale(request.density);

    result.layoutClass = layoutClass;
    result.density = request.density;
    result.resourceProfile = request.resourceProfile;
    result.dpiScale = dpiScale;
    result.logicalWidth = logicalWidth;
    result.logicalHeight = logicalHeight;
    result.densityScale = densityScale;
    result.outerPadding = ClassOuterPadding(layoutClass) * densityScale;
    result.panelGap = ClassPanelGap(layoutClass) * densityScale;
    result.contentPadding = std::clamp(18.0F * densityScale, 16.0F, 24.0F);
    result.headerHeight = std::clamp(64.0F * densityScale, 58.0F, 76.0F);
    // Keep the persistent voice/status dock compact. Its controls remain
    // 38 DIPs tall, leaving a comfortable vertical inset without sacrificing
    // an unnecessary strip of conversation space.
    result.bottomBarHeight = std::clamp(54.0F * densityScale, 50.0F, 64.0F);
    result.minimumContentWidth = MinimumContentWidth(layoutClass);
    result.maximumMessageWidth = layoutClass == HorizonLayoutClass::Ultra
        ? 1240.0F
        : (layoutClass == HorizonLayoutClass::Studio ? 1160.0F : 1040.0F);

    const float workspaceY = result.outerPadding;
    const float workspaceHeight = std::max(0.0F,
        logicalHeight - (result.outerPadding * 2.0F) - result.bottomBarHeight - result.panelGap);
    // The rail remains lightweight, but its hit targets and focus rings should
    // not touch the edges even at Windows text scaling above 100%.
    const float railWidth = std::clamp(82.0F * densityScale, 76.0F, 98.0F);
    float cursorX = result.outerPadding;

    result.communityRail = HorizonPanelLayout{
        MakeRect(cursorX, workspaceY, railWidth, workspaceHeight),
        PanelPresentation::Inline,
    };
    cursorX += railWidth + result.panelGap;

    const float rightEdge = std::max(cursorX, logicalWidth - result.outerPadding);
    float contentRight = rightEdge;
    float availableContentWidth = std::max(0.0F, contentRight - cursorX);

    const float requestedNavigationWidth = NavigationWidth(layoutClass) * densityScale;
    if (request.channelPanelRequested && layoutClass != HorizonLayoutClass::Compact &&
        CanFitPanel(availableContentWidth, requestedNavigationWidth, result.panelGap,
                    result.minimumContentWidth)) {
        result.channelPanel = HorizonPanelLayout{
            MakeRect(cursorX, workspaceY, requestedNavigationWidth, workspaceHeight),
            PanelPresentation::Inline,
        };
        cursorX += requestedNavigationWidth + result.panelGap;
        availableContentWidth = std::max(0.0F, contentRight - cursorX);
    } else if (request.channelPanelRequested) {
        const float drawerWidth = std::min(requestedNavigationWidth,
                                           std::max(0.0F, logicalWidth - railWidth -
                                               (result.outerPadding * 2.0F) - result.panelGap));
        result.channelPanel = HorizonPanelLayout{
            MakeRect(result.outerPadding + railWidth + result.panelGap,
                     workspaceY,
                     drawerWidth,
                     workspaceHeight),
            PanelPresentation::Drawer,
        };
    }

    const float requestedUtilityWidth = UtilityWidth(layoutClass) * densityScale;
    const bool utilityMayBeInline = layoutClass == HorizonLayoutClass::Ultra ||
        (layoutClass == HorizonLayoutClass::Studio &&
         request.resourceProfile == ResourceProfile::Visual);
    if (request.utilityPanelRequested && utilityMayBeInline &&
        CanFitPanel(availableContentWidth, requestedUtilityWidth, result.panelGap,
                    result.minimumContentWidth)) {
        contentRight -= requestedUtilityWidth + result.panelGap;
        availableContentWidth = std::max(0.0F, contentRight - cursorX);
        result.utilityPanel = HorizonPanelLayout{
            MakeRect(contentRight + result.panelGap,
                     workspaceY,
                     requestedUtilityWidth,
                     workspaceHeight),
            PanelPresentation::Inline,
        };
    } else if (request.utilityPanelRequested) {
        const float drawerWidth = std::min(requestedUtilityWidth,
                                           std::max(0.0F, logicalWidth -
                                               (result.outerPadding * 2.0F)));
        result.utilityPanel = HorizonPanelLayout{
            MakeRect(logicalWidth - result.outerPadding - drawerWidth,
                     workspaceY,
                     drawerWidth,
                     workspaceHeight),
            PanelPresentation::Drawer,
        };
    }

    const float requestedMemberWidth = MemberWidth(layoutClass) * densityScale;
    bool memberMayBeInline = layoutClass == HorizonLayoutClass::Standard ||
        layoutClass == HorizonLayoutClass::Wide ||
        layoutClass == HorizonLayoutClass::Studio ||
        layoutClass == HorizonLayoutClass::Ultra;
    if (request.resourceProfile == ResourceProfile::Economy &&
        layoutClass != HorizonLayoutClass::Studio &&
        layoutClass != HorizonLayoutClass::Ultra) {
        memberMayBeInline = false;
    }
    if (request.memberPanelRequested && memberMayBeInline &&
        CanFitPanel(availableContentWidth, requestedMemberWidth, result.panelGap,
                    result.minimumContentWidth)) {
        contentRight -= requestedMemberWidth + result.panelGap;
        availableContentWidth = std::max(0.0F, contentRight - cursorX);
        result.memberPanel = HorizonPanelLayout{
            MakeRect(contentRight + result.panelGap,
                     workspaceY,
                     requestedMemberWidth,
                     workspaceHeight),
            PanelPresentation::Inline,
        };
    } else if (request.memberPanelRequested) {
        const float drawerWidth = std::min(requestedMemberWidth,
                                           std::max(0.0F, logicalWidth -
                                               (result.outerPadding * 2.0F)));
        result.memberPanel = HorizonPanelLayout{
            MakeRect(logicalWidth - result.outerPadding - drawerWidth,
                     workspaceY,
                     drawerWidth,
                     workspaceHeight),
            PanelPresentation::Drawer,
        };
    }

    result.contentPanel = HorizonPanelLayout{
        MakeRect(cursorX, workspaceY, availableContentWidth, workspaceHeight),
        PanelPresentation::Inline,
    };
    result.bottomBar = HorizonPanelLayout{
        MakeRect(result.outerPadding,
                 logicalHeight - result.outerPadding - result.bottomBarHeight,
                 std::max(0.0F, logicalWidth - (result.outerPadding * 2.0F)),
                 result.bottomBarHeight),
        PanelPresentation::Inline,
    };

    const float readableWidth = std::max(0.0F,
        result.contentPanel.bounds.width - (result.contentPadding * 2.0F));
    result.preferredMessageWidth = std::min(readableWidth, result.maximumMessageWidth);
    result.showNavigationLabels = logicalWidth >= 960.0F;
    result.useTwoColumnHome = result.contentPanel.bounds.width >= 980.0F;
    result.useThreeColumnHome = result.contentPanel.bounds.width >= 1480.0F;
    result.compactComposer = result.contentPanel.bounds.width < 640.0F;
    result.reducedVisuals = request.resourceProfile == ResourceProfile::Economy;

    return result;
}

}  // namespace ss
