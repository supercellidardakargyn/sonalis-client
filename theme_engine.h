#pragma once

#include <cstddef>
#include <cstdint>

#include "settings.h"

struct ImGuiStyle;

namespace ss {

struct ResourceBudget {
    std::size_t imageCacheBytes{};
    std::size_t workingSetTargetBytes{};
    std::size_t workingSetWarningBytes{};
    std::size_t maximumResolvedMessages{};
    std::uint32_t focusedVoiceFrameMs{};
    bool transitions{};
    bool avatars{};
    bool enhancedVoiceStage{};
};

[[nodiscard]] ResourceBudget BudgetFor(ResourceProfile profile) noexcept;
void ApplyClientTheme(ImGuiStyle& style,
                      UiTheme theme,
                      ResourceProfile profile,
                      bool rightToLeft,
                      float dpiScale,
                      float textScale,
                      int density,
                      bool highContrast,
                      int colorVisionMode,
                      float customAccentR,
                      float customAccentG,
                      float customAccentB) noexcept;

}  // namespace ss
