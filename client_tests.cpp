#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>

#include "opus_codec.h"
#include "protocol.h"
#include "device_identity.h"
#include "localization.h"
#include "json_contract.h"
#include "audio_engine.h"
#include "horizon_layout.h"
#include "theme_engine.h"
#include "update_service.h"

static_assert(ss::AudioEngine::kMaximumMixedPeers >= 64,
              "The allocation-free mixer must cover every supported room tier");

int main() {
    const ss::ResourceBudget economyBudget = ss::BudgetFor(ss::ResourceProfile::Economy);
    const ss::ResourceBudget balancedBudget = ss::BudgetFor(ss::ResourceProfile::Balanced);
    const ss::ResourceBudget visualBudget = ss::BudgetFor(ss::ResourceProfile::Visual);
    assert(economyBudget.workingSetTargetBytes < balancedBudget.workingSetTargetBytes);
    assert(balancedBudget.workingSetTargetBytes < visualBudget.workingSetTargetBytes);
    assert(economyBudget.workingSetWarningBytes > economyBudget.workingSetTargetBytes);
    assert(balancedBudget.workingSetWarningBytes > balancedBudget.workingSetTargetBytes);
    assert(visualBudget.workingSetWarningBytes > visualBudget.workingSetTargetBytes);
    assert(economyBudget.maximumResolvedMessages < balancedBudget.maximumResolvedMessages);
    assert(balancedBudget.maximumResolvedMessages == visualBudget.maximumResolvedMessages);
    assert(ss::ParseReleaseVersion("4.1.0") == std::optional(std::array<std::uint32_t, 3>{4, 1, 0}));
    assert(!ss::ParseReleaseVersion("4.1"));
    assert(!ss::ParseReleaseVersion("4.1.0-canary"));
    assert(!ss::ParseReleaseVersion("4.1.0.1"));
    assert(ss::IsNewerReleaseVersion("4.1.1", "4.1.0"));
    assert(!ss::IsNewerReleaseVersion("4.1.0", "4.1.0"));
    assert(!ss::IsNewerReleaseVersion("4.0.9", "4.1.0"));
    assert(std::wstring_view(ss::kAutomaticUpdateInstallerArguments).find(L"/VERYSILENT") != std::wstring_view::npos);
    assert(std::wstring_view(ss::kAutomaticUpdateInstallerArguments).find(L"/NORESTART") != std::wstring_view::npos);
    assert(std::wstring_view(ss::kAutomaticUpdateInstallerArguments).find(L"/RESTARTSONALIS") != std::wstring_view::npos);
    ss::protocol::ServerAddress address;
    std::string error;
    assert(ss::protocol::ParseServerAddress("voice.example.com", address, error));
    assert(address.port == 25565);
    assert(ss::protocol::ParseServerAddress("127.0.0.1:30000", address, error));
    assert(address.port == 30000);

    std::array<float, 4> samples{0.5F, -0.5F, 0.5F, -0.5F};
    assert(std::abs(ss::ComputeRms(samples.data(), static_cast<int>(samples.size())) - 0.5F) < 0.0001F);
    assert(std::abs(ss::RmsToDbfs(0.5F) - -6.0206F) < 0.001F);
    assert(ss::RmsToDbfs(0.0F) == -96.0F);
    assert(ss::RmsToMeter(0.001F) >= 0.0F);
    assert(ss::SmoothMeterLevel(0.1F, 0.8F) > 0.1F);
    const float fallingMeter = ss::SmoothMeterLevel(0.8F, 0.1F);
    assert(fallingMeter < 0.8F);
    assert(fallingMeter > 0.1F);
    const float limited = ss::AdvanceBlockLimiterGain(1.0F, 2.0F);
    assert(limited > 0.48F && limited < 0.50F);
    const float recovering = ss::AdvanceBlockLimiterGain(limited, 0.5F);
    assert(recovering > limited && recovering <= limited + 0.0251F);
    assert(ss::AdvanceBlockLimiterGain(1.0F, 0.5F) == 1.0F);

    std::array<std::uint8_t, ss::protocol::kTokenBytes> token{};
    assert(ss::protocol::DecodeHexToken("000102030405060708090a0b0c0d0e0f", token));

    const std::string bindingA = ss::DeviceBindingIdFromMachineGuid("ABCDEF01-2345-6789-ABCD-EF0123456789");
    const std::string bindingSame = ss::DeviceBindingIdFromMachineGuid("abcdef01-2345-6789-abcd-ef0123456789");
    const std::string bindingB = ss::DeviceBindingIdFromMachineGuid("ABCDEF01-2345-6789-ABCD-EF0123456790");
    assert(bindingA.size() == 36);
    assert(bindingA == bindingSame);
    assert(bindingA != bindingB);
    assert(bindingA[8] == '-' && bindingA[13] == '-' && bindingA[18] == '-' && bindingA[23] == '-');

    const auto& languages = ss::SupportedLanguages();
    assert(languages.size() == 12);
    for (std::size_t languageIndex = 0; languageIndex < languages.size(); ++languageIndex) {
        const auto& language = languages[languageIndex];
        assert(!language.code.empty());
        assert(!language.nativeName.empty());
        assert(ss::LanguageDisplayName(language.language)[0] != '\0');
        assert(ss::ParseLanguage(language.code) == language.language);
        for (std::size_t textIndex = 0; textIndex < static_cast<std::size_t>(ss::TextId::Count); ++textIndex) {
            const char* translated = ss::Translate(language.language, static_cast<ss::TextId>(textIndex));
            assert(translated != nullptr && translated[0] != '\0');
        }
        for (std::size_t otherIndex = languageIndex + 1; otherIndex < languages.size(); ++otherIndex) {
            assert(language.code != languages[otherIndex].code);
        }
    }
    assert(ss::IsRightToLeft(ss::Language::Arabic));
    assert(!ss::IsRightToLeft(ss::Language::English));

    const nlohmann::json legacyRoom{{"serverDenoiseEnabled", 1}, {"archivedAt", nullptr}};
    assert(ss::JsonBooleanOr(legacyRoom, "serverDenoiseEnabled"));
    assert(ss::JsonStringOr(legacyRoom, "archivedAt").empty());
    const nlohmann::json emptyMessagePage{
        {"beforeCursor", nullptr}, {"afterCursor", nullptr}, {"hasMore", false}, {"messages", nlohmann::json::array()}};
    assert(ss::JsonStringOr(emptyMessagePage, "beforeCursor").empty());
    assert(!ss::JsonBooleanOr(emptyMessagePage, "hasMore"));
    const nlohmann::json legacyDevice{{"activeRecipient", "0"}};
    assert(!ss::JsonBooleanOr(legacyDevice, "activeRecipient", true));
    const nlohmann::json numericString{{"unreadCount", "42"}};
    assert(ss::JsonIntegerOr<std::uint32_t>(numericString, "unreadCount") == 42U);

    ss::HorizonLayoutRequest compactRequest{};
    compactRequest.viewportWidthPixels = 960.0F;
    compactRequest.viewportHeightPixels = 640.0F;
    const ss::HorizonLayoutMetrics compactLayout = ss::CalculateHorizonLayout(compactRequest);
    if (compactLayout.layoutClass != ss::HorizonLayoutClass::Compact ||
        !compactLayout.communityRail.IsInline() || !compactLayout.channelPanel.IsDrawer() ||
        !compactLayout.memberPanel.IsDrawer() || !compactLayout.contentPanel.IsInline() ||
        compactLayout.bottomBarHeight < 50.0F || compactLayout.bottomBarHeight > 60.0F ||
        compactLayout.compactComposer != (compactLayout.contentPanel.bounds.width < 640.0F)) {
        return 10;
    }

    ss::HorizonLayoutRequest standardRequest{};
    standardRequest.viewportWidthPixels = 1280.0F;
    standardRequest.viewportHeightPixels = 800.0F;
    const ss::HorizonLayoutMetrics standardLayout = ss::CalculateHorizonLayout(standardRequest);
    if (standardLayout.layoutClass != ss::HorizonLayoutClass::Standard ||
        !standardLayout.channelPanel.IsInline() || !standardLayout.memberPanel.IsInline() ||
        standardLayout.contentPanel.bounds.width < standardLayout.minimumContentWidth ||
        standardLayout.panelGap < 5.0F || standardLayout.panelGap > 7.0F ||
        standardLayout.bottomBarHeight < 50.0F || standardLayout.bottomBarHeight > 60.0F ||
        standardLayout.channelPanel.bounds.width < 370.0F) {
        return 11;
    }

    ss::HorizonLayoutRequest wideRequest{};
    wideRequest.viewportWidthPixels = 1920.0F;
    wideRequest.viewportHeightPixels = 1080.0F;
    const ss::HorizonLayoutMetrics wideLayout = ss::CalculateHorizonLayout(wideRequest);
    if (wideLayout.layoutClass != ss::HorizonLayoutClass::Wide ||
        !wideLayout.channelPanel.IsInline() || !wideLayout.memberPanel.IsInline()) {
        return 12;
    }

    ss::HorizonLayoutRequest economyWideRequest = wideRequest;
    economyWideRequest.resourceProfile = ss::ResourceProfile::Economy;
    const ss::HorizonLayoutMetrics economyWideLayout = ss::CalculateHorizonLayout(economyWideRequest);
    if (!economyWideLayout.memberPanel.IsDrawer() || !economyWideLayout.reducedVisuals) {
        return 13;
    }

    ss::HorizonLayoutRequest scaledRequest{};
    scaledRequest.viewportWidthPixels = 7680.0F;
    scaledRequest.viewportHeightPixels = 4320.0F;
    scaledRequest.dpiScale = 4.0F;
    const ss::HorizonLayoutMetrics scaledLayout = ss::CalculateHorizonLayout(scaledRequest);
    if (scaledLayout.layoutClass != ss::HorizonLayoutClass::Wide ||
        std::abs(scaledLayout.logicalWidth - 1920.0F) >= 0.001F ||
        std::abs(scaledLayout.logicalHeight - 1080.0F) >= 0.001F) {
        return 14;
    }

    ss::HorizonLayoutRequest ultraRequest{};
    ultraRequest.viewportWidthPixels = 15360.0F;
    ultraRequest.viewportHeightPixels = 8640.0F;
    ultraRequest.resourceProfile = ss::ResourceProfile::Visual;
    ultraRequest.utilityPanelRequested = true;
    const ss::HorizonLayoutMetrics ultraLayout = ss::CalculateHorizonLayout(ultraRequest);
    const ss::HorizonLayoutMetrics repeatedUltraLayout = ss::CalculateHorizonLayout(ultraRequest);
    if (ultraLayout.layoutClass != ss::HorizonLayoutClass::Ultra ||
        !ultraLayout.channelPanel.IsInline() || !ultraLayout.memberPanel.IsInline() ||
        !ultraLayout.utilityPanel.IsInline() ||
        ultraLayout.preferredMessageWidth > ultraLayout.maximumMessageWidth ||
        ultraLayout.contentPanel.bounds.width != repeatedUltraLayout.contentPanel.bounds.width ||
        ultraLayout.memberPanel.bounds.x != repeatedUltraLayout.memberPanel.bounds.x ||
        ultraLayout.bottomBar.bounds.y + ultraLayout.bottomBar.bounds.height > ultraLayout.logicalHeight) {
        return 15;
    }
    return 0;
}
