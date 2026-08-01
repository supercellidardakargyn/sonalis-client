#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "audio_engine.h"
#include "network.h"
#include "settings.h"
#include "platform_api.h"
#include "update_service.h"
#include "message_crypto.h"
#include "media_crypto.h"
#include "guardian_local_scanner.h"
#include "native_device_signer.h"
#include "background_worker.h"
#include "localization.h"
#include "horizon_layout.h"
#include "performance.h"
#include "sonalis/core/event_generation.h"

namespace ss {

enum class ClientPage : std::uint8_t { Home, Voice, Rooms, Messages, Settings };
enum class HomeSection : std::uint8_t { Friends, Pending, Notifications };
enum class ClientLayout : std::uint8_t { Compact, Wide };
enum class SettingsSection : std::uint8_t {
    Account,
    Appearance,
    Notifications,
    Audio,
    Privacy,
    Language,
    Updates,
    Diagnostics,
    About,
};

class AppUi final {
public:
    AppUi();
    ~AppUi();
    AppUi(const AppUi&) = delete;
    AppUi& operator=(const AppUi&) = delete;

    void Initialize();
    void SetWindowHandle(HWND window) noexcept;
    void SetWindowPlacement(int x, int y, int width, int height, bool maximized) noexcept;
    void SetLogoTexture(std::uint64_t textureId) noexcept;
    void SetStartupWarning(std::string message);
    void SetPendingInvite(std::string code);
    void SetPendingRoom(std::string roomId);
    void Tick();
    void Render();
    void Shutdown();
    [[nodiscard]] bool IsActive() const noexcept;
    [[nodiscard]] std::uint32_t RefreshIntervalMs(bool focused, bool visible) const noexcept;
    [[nodiscard]] HANDLE RedrawEvent() const noexcept;
    void ConsumeRedraw() noexcept;
    [[nodiscard]] bool IsMicrophoneMuted() const noexcept;
    [[nodiscard]] bool IsStartupUpdateGateActive() const noexcept;
    void ToggleMicrophoneMuted() noexcept;
    [[nodiscard]] std::string ActiveRoomName() const;
    [[nodiscard]] Language ActiveLanguage() const noexcept;
    [[nodiscard]] UiTheme ActiveTheme() const noexcept;
    [[nodiscard]] ResourceProfile ActiveResourceProfile() const noexcept;
    [[nodiscard]] float ActiveTextScale() const noexcept;
    [[nodiscard]] int ActiveUiDensity() const noexcept;
    [[nodiscard]] bool ActiveHighContrast() const noexcept;
    [[nodiscard]] int ActiveColorVisionMode() const noexcept;
    [[nodiscard]] float ActiveCustomAccentR() const noexcept;
    [[nodiscard]] float ActiveCustomAccentG() const noexcept;
    [[nodiscard]] float ActiveCustomAccentB() const noexcept;
    bool ConsumeFontReloadRequest() noexcept;

private:
    void BeginApplicationInitialization();
    [[nodiscard]] bool HandleStartupUpdateGate();
    void RenderStartupUpdateGate(float dpiScale);
    void ToggleOutputMuted() noexcept;
    void Connect();
    void Disconnect();
    void RestartAudioDevices();
    void RefreshDevices();
    void SaveSettings();
    void FlushDiagnosticTelemetry();
    void CopySettingsToBuffers();
    void RefreshPlatformRooms();
    void RefreshRoomOverview();
    void RefreshSocial();
    void RefreshAccountSecurity();
    void RefreshGuardian();
    void RefreshClientExperience();
    void UpdateGuardianPreferences();
    void RefreshRoomMembers();
    void QueuePlatformAction(std::function<bool(std::string&)> action,
                             std::string successMessage,
                             bool refreshRooms,
                             bool refreshSocial,
                             bool refreshMembers,
                             bool refreshOverview = false,
                             std::function<void()> onSuccess = {});
    void RefreshChat(bool loadOlder = false);
    void RefreshPins();
    void OpenDirectChat(const PlatformFriend& friendEntry);
    void SendChatMessage();
    void SendChatEvent(std::string eventType, std::string targetMessageId, std::string text,
                       std::string reaction = {}, std::string moderationReason = {},
                       std::vector<std::string> attachmentIds = {});
    void SelectChatAttachment();
    void DownloadChatAttachment(std::string attachmentId);
    void SubmitMediaReport();
    void WithdrawLastMediaReport();
    void StartRealtime();
    void HandleRealtimeEvents();
    void ResetSessionView(std::string message);
    void HandleWorkerException(std::string message);
    void WipeChatLines() noexcept;
    void RenderLogin();
    void ShowWindowsNotification(const std::string& title, const std::string& body);
    void DismissWindowsNotification() noexcept;
    static int FindDevice(const std::vector<AudioDeviceInfo>& devices, const std::string& id);
    static bool DeviceCombo(const char* label,
                            const std::vector<AudioDeviceInfo>& devices,
                            int& selectedIndex,
                            bool enabled);
    [[nodiscard]] const char* T(TextId id) const noexcept;

    SettingsStore store_;
    AppSettings settings_;
    NetworkClient network_;
    PlatformApi platform_;
    UpdateService updater_;
    MessageCrypto messageCrypto_;
    NativeDeviceSigner nativeDeviceSigner_;
    BackgroundWorker worker_;
    RealtimeClient realtime_;
    AudioEngine audio_;
    std::vector<AudioDeviceInfo> inputDevices_;
    std::vector<AudioDeviceInfo> outputDevices_;
    int inputIndex_{0};
    int outputIndex_{0};
    std::array<char, 256> serverBuffer_{};
    std::array<char, 96> nicknameBuffer_{};
    std::array<char, 128> roomBuffer_{};
    std::array<char, 254> loginBuffer_{};
    std::array<char, 129> passwordBuffer_{};
    std::array<char, 32> inviteBuffer_{};
    std::array<char, 32> generatedInviteBuffer_{};
    std::array<char, 65> createRoomNameBuffer_{};
    std::array<char, 501> createRoomDescriptionBuffer_{};
    std::array<char, 33> friendSearchBuffer_{};
    std::array<char, 501> banReasonBuffer_{};
    std::array<char, 8193> chatBuffer_{};
    std::array<char, 129> chatSearchBuffer_{};
    std::array<char, 3001> restrictionAppealBuffer_{};
    std::vector<PlatformRoom> platformRooms_;
    std::optional<PlatformRoomOverview> roomOverview_;
    std::vector<PlatformMember> roomMembers_;
    std::vector<PlatformFriend> friends_;
    std::vector<PlatformFriend> friendSearchResults_;
    std::vector<PlatformNotification> notifications_;
    std::vector<PlatformSession> accountSessions_;
    std::vector<PlatformClientDevice> accountDevices_;
    std::optional<MediaSafetyConfig> mediaSafetyConfig_;
    MediaPreferences mediaPreferences_;
    std::vector<AccountRestriction> restrictions_;
    std::string appealRestrictionId_;
    std::vector<PlatformConversation> conversations_;
    std::string pendingRoomId_;
    bool pendingRoomAutoConnect_{false};
    int selectedRoomIndex_{0};
    std::string selectedChannelId_;
    std::string selectedChannelType_;
    std::string activeVoiceChannelId_;
    std::string chatChannelId_;
    std::array<char, 33> createCategoryNameBuffer_{};
    std::array<char, 33> createChannelNameBuffer_{};
    std::array<char, 33> manageChannelNameBuffer_{};
    std::string channelManageAction_;
    std::string channelManageTargetId_;
    std::string channelManageTargetLabel_;
    int createChannelType_{0};
    int createChannelContentRating_{0};
    int createChannelMediaPolicy_{1};
    bool createChannelLocalScan_{true};
    int channelManageContentRating_{0};
    int channelManageMediaPolicy_{1};
    bool channelManageLocalScan_{true};
    int inviteExpiresHours_{168};
    int inviteMaxUses_{10};
    std::string uiMessage_;
    std::string serverDenoiseReason_{"client_not_requested"};
    std::string chatConversationId_;
    std::string directConversationId_;
    std::string directConversationLabel_;
    std::string banTargetUserId_;
    std::string banTargetLabel_;
    int chatEpoch_{1};
    std::array<std::uint8_t, 32> chatKey_{};
    bool chatKeyReady_{false};
    std::uint64_t chatSequence_{0};
    enum class ChatDeliveryState : std::uint8_t { Sent, Pending, Failed };
    struct ChatLine {
        std::string id;
        std::string senderId;
        std::string sender;
        std::string text;
        std::string createdAt;
        std::string replyTo;
        bool edited{};
        std::array<std::uint16_t, 6> reactions{};
        std::vector<std::string> attachmentIds;
        ChatDeliveryState delivery{ChatDeliveryState::Sent};
    };
    struct PendingChatAttachment {
        std::string id;
        std::string name;
        std::string conversationId;
        std::string channelId;
        std::uint64_t encryptedBytes{};
    };
    std::vector<ChatLine> chatLines_;
    std::vector<std::string> pinnedMessageIds_;
    std::string chatBeforeCursor_;
    std::string chatAfterCursor_;
    bool chatRefreshPending_{false};
    sonalis::core::EventGeneration chatRefreshGeneration_;
    bool chatLoadOlderRequested_{false};
    bool chatHasMore_{false};
    bool chatSendPending_{false};
    bool mediaUploadPending_{false};
    bool mediaDownloadPending_{false};
    bool mediaReportPending_{false};
    std::vector<PendingChatAttachment> pendingChatAttachments_;
    GuardianLocalScanner guardianLocalScanner_;
    ClientExperiencePolicy experiencePolicy_;
    bool pinsRefreshPending_{false};
    bool showPinnedMessages_{false};
    bool focusChatSearch_{false};
    std::string replyTargetId_;
    std::string editTargetId_;
    std::string moderationTargetId_;
    std::array<char, 501> moderationMessageReasonBuffer_{};
    std::string reportAttachmentId_;
    std::string reportMessageId_;
    std::string reportUserId_;
    std::string lastSubmittedMediaReportId_;
    std::array<char, 1001> mediaReportNoteBuffer_{};
    int mediaReportReasonIndex_{};
    bool showMediaReportReceipt_{false};
    std::uint64_t lastTypingSentMs_{};
    bool typingSent_{false};
    std::uint64_t logoTexture_{};
    bool initialized_{false};
    bool applicationInitializationStarted_{false};
    bool startupUpdateGate_{true};
    bool startupInstallerLaunchAttempted_{false};
    std::string startupUpdateFailure_;
    HANDLE redrawEvent_{};
    bool loginPending_{false};
    bool roomsRefreshPending_{false};
    bool overviewRefreshPending_{false};
    bool socialRefreshPending_{false};
    bool accountSecurityRefreshPending_{false};
    bool accountSecurityLoaded_{false};
    bool guardianRefreshPending_{false};
    bool guardianPreferenceUpdatePending_{false};
    bool clientExperienceRefreshPending_{false};
    bool connectPending_{false};
    bool voiceSleeping_{false};
    bool voiceCanSpeak_{true};
    bool audioRestartPending_{false};
    bool platformActionPending_{false};
    bool roomMembersRefreshPending_{false};
    bool updateDownloadPending_{false};
    bool updateInstallPromptPending_{false};
    bool manualUpdateCheckPending_{false};
    bool diagnosticUploadPending_{false};
    bool terminalSessionHandled_{false};
    UpdateState observedUpdateState_{UpdateState::Idle};
    ClientPage activePage_{ClientPage::Home};
    HomeSection homeSection_{HomeSection::Friends};
    ClientLayout layout_{ClientLayout::Compact};
    SettingsSection settingsSection_{SettingsSection::Appearance};
    HorizonLayoutMetrics horizonLayout_{};
    HorizonLayoutClass previousHorizonLayoutClass_{HorizonLayoutClass::Standard};
    bool horizonLayoutInitialized_{false};
    bool showChannelPanel_{true};
    bool showMemberPanel_{true};
    bool resetSettingsNavigationScroll_{true};
    std::uint64_t lastFallbackSyncMs_{};
    std::unordered_map<std::string, std::uint64_t> typingUntilMs_;
    std::unordered_map<std::string, std::string> presenceByUser_;
    std::unordered_map<std::string, std::string> customStatusByUser_;
    std::unordered_map<std::string, std::string> chatDrafts_;
    std::string ownPresence_{"online"};
    std::array<char, 81> customStatusBuffer_{};
    std::uint64_t lastPresenceCheckMs_{};
    std::uint64_t lastVoiceDiagnosticsLogMs_{};
    std::uint64_t nextDiagnosticUploadAttemptMs_{};
    std::uint64_t lastMemorySampleMs_{};
    std::uint8_t diagnosticUploadFailures_{};
    ProcessMemorySnapshot processMemory_{};
    bool memoryBudgetWarning_{};
    HWND windowHandle_{};
    bool notificationIconVisible_{false};
    std::uint64_t notificationIconExpiresMs_{};
    std::string pendingNotificationConversation_;
    bool pendingNotificationDirect_{false};
    bool pendingNotificationAll_{false};
    std::atomic_bool messageCryptoReady_{false};
    std::atomic_bool voiceMeterVisible_{false};
    Language language_{Language::Turkish};
    std::atomic_bool fontReloadRequested_{false};
};

}  // namespace ss
