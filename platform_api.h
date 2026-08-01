#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "credential_vault.h"
#include "diagnostics.h"
#include "http_client.h"
#include "message_crypto.h"
#include "realtime_client.h"
#include "settings.h"

namespace ss {

struct PlatformUser { std::string id; std::string username; std::string nickname; std::string role; std::string plan; };
struct PlatformRoom {
    std::string id; std::string name; std::string description; std::string role; std::string nodeState;
    bool serverDenoiseEnabled{};
};
struct RoomInvite { std::string code; std::string url; std::string deepLink; };
struct PlatformMember { std::string id; std::string username; std::string nickname; std::string role; };
struct PlatformChannelCategory { std::string id; std::string name; int position{}; };
struct PlatformChannel {
    std::string id; std::string categoryId; std::string type; std::string slug; std::string name; std::string topic;
    int position{}; int voiceUserLimit{}; int slowModeSeconds{}; std::uint32_t unreadCount{}; std::uint32_t mentionCount{};
    std::uint32_t pinCount{};
    std::string notificationMode{"mentions"}; std::string muteUntil;
    std::string contentRating{"sfw"}; std::string mediaPostingPolicy{"members"};
    bool localMediaScanRequired{true};
};
struct PlatformRoomOverview {
    std::string roomId; std::string name; std::string description; std::string role;
    std::vector<PlatformChannelCategory> categories;
    std::vector<PlatformChannel> channels;
    std::vector<PlatformMember> members;
};
struct PlatformFriend {
    std::string id; std::string username; std::string nickname; std::string state; std::string requesterId;
};
struct PlatformNotification {
    std::string id; std::string kind; std::string title; std::string body; bool read{}; std::string createdAt;
};
struct PlatformSession {
    std::string id; std::string deviceName; std::string userAgent; std::string createdAt; std::string lastSeenAt;
};
struct PlatformClientDevice {
    std::string id; std::string platform; std::string clientVersion; std::string lastSeenAt;
    std::string certificateExpiresAt; std::uint32_t activeSessions{};
};
struct VoiceGrant {
    std::string grant; std::string host; std::string roomId; std::string channelId;
    unsigned short port{25565}; bool serverDenoise{}; int bitrate{24000};
    std::string certificateFingerprint;
    std::string serverDenoiseReason;
    std::string denoiseMode;
    std::string routeType;
    bool p2pEnabled{};
    bool canSpeak{true};
    std::string stunHost; unsigned short stunPort{3478};
};
struct NativeLoginChallenge { std::string id; std::string challenge; };
struct PlatformConversation {
    std::string id; std::string kind; std::string roomId; std::string roomName; int currentEpoch{1};
    std::uint32_t unreadCount{}; std::string lastMessageId; std::string lastMessageAt;
    std::string directPeerId; std::string directPeerUsername; std::string directPeerNickname;
};
struct MessageDevice {
    std::string deviceId; std::string userId; std::string encryptionPublicKey; std::string signingPublicKey;
    bool activeRecipient{true};
};
struct KeyEnvelopeUpload { std::string recipientType; std::string recipientId; std::string envelope; };
struct EncryptedPlatformMessage {
    std::string id; std::string conversationId; std::string senderId; std::string deviceId; int epoch{1};
    std::uint64_t clientSequence{}; std::string eventType{"message"}; std::string ciphertext; std::string nonce;
    std::string signature; std::string replyTo; std::string createdAt;
    std::string targetMessageId; std::string reaction; std::string moderationReason; std::uint32_t characterCount{};
    std::string channelId;
    int signatureVersion{};
    std::vector<std::string> mentions;
    std::vector<std::string> attachmentIds;
};
struct MessageSyncPage {
    std::vector<EncryptedPlatformMessage> messages;
    std::string beforeCursor;
    std::string afterCursor;
    bool hasMore{};
};
struct MediaSafetyConfig {
    std::string moderationPublicKey;
    std::size_t maximumImageBytes{8U * 1024U * 1024U};
    bool privateMediaReportingAvailable{};
    bool mayShowSensitiveMedia{};
};
struct MediaPreferences {
    SensitiveMediaMode sensitiveMediaMode{SensitiveMediaMode::Block};
    bool allowNonFriendMedia{};
    bool lockedForMinor{};
};
struct MediaReportReceipt {
    std::string id;
    std::string status;
    bool evidenceUploadRequired{};
    std::string moderationPublicKey;
};
struct AccountRestriction {
    std::string id;
    std::string scope;
    std::string reasonCode;
    std::string startsAt;
    std::string expiresAt;
    bool appealPending{};
};
struct MediaAttachmentDraft {
    std::string id;
    std::string conversationId;
    std::string channelId;
    std::uint64_t sizeBytes{};
    std::string mimeHint;
    std::string ciphertextSha256;
    std::string metadataCiphertext;
    std::string metadataNonce;
    std::string localScanModel;
    std::string localScanVerdict{"safe"};
    std::string localScanDigest;
};
struct MediaUploadGrant {
    std::string id;
    std::string uploadUrl;
    std::map<std::string, std::string> requiredHeaders;
    std::string expiresAt;
    std::uint64_t maximumObjectBytes{};
};
struct MediaDownloadGrant {
    std::string id;
    std::string downloadUrl;
    std::string expiresAt;
    std::uint64_t sizeBytes{};
    std::string ciphertextSha256;
    std::string metadataCiphertext;
    std::string metadataNonce;
    std::string mimeHint;
};
struct GuardianClientModel {
    std::string id;
    std::string name;
    std::string platform;
    std::string architecture;
    std::string engine;
    std::string channel;
    std::string version;
    std::string artifactUrl;
    std::uint64_t artifactSize{};
    std::string sha256;
    std::string signatureBase64;
    std::string minimumClientVersion;
    std::map<std::string, std::string> labelMap;
    float reviewThreshold{0.45F};
    float rejectThreshold{0.85F};
    float criticalThreshold{0.95F};
    int rolloutPercent{};
    bool required{};
};
struct ClientExperiencePolicy {
    int policyVersion{1};
    UiTheme defaultTheme{UiTheme::AuroraDark};
    bool allowUserThemeChoice{true};
    ResourceProfile defaultResourceProfile{ResourceProfile::Balanced};
    bool allowUserResourceProfileChoice{true};
    bool animationsEnabled{true};
    bool backgroundEffectsEnabled{true};
    bool customAccentsEnabled{true};
    std::string accentHex{"#1F8FFF"};
    int focusedVoiceFps{15};
    int unfocusedFps{4};
    std::size_t maximumResolvedMessages{300};
    std::size_t maximumImageCacheMb{32};
    bool mediaAttachmentsEnabled{true};
    bool enhancedPresenceEnabled{true};
    bool quickSwitcherEnabled{true};
    bool experimentalAecEnabled{};
};

enum class PlatformSessionState : std::uint8_t {
    SignedOut,
    Active,
    Expired,
    UpdateRequired,
};

class PlatformApi final {
public:
    void SetOrigin(std::string origin);
    void SetClientDeviceId(std::string deviceId);
    [[nodiscard]] std::optional<NativeLoginChallenge> RequestNativeLoginChallenge(
        const std::string& signingPublicKey, const std::string& keyAlgorithm, std::string& error);
    bool Login(const std::string& login, const std::string& password,
               const std::string& signingPublicKey, const std::string& keyAlgorithm, const NativeLoginChallenge& challenge,
               const std::string& signature, std::string& error);
    bool RestoreSession(std::string& error);
    void Logout() noexcept;
    [[nodiscard]] bool IsAuthenticated() const noexcept;
    [[nodiscard]] PlatformSessionState SessionState() const noexcept;
    [[nodiscard]] PlatformUser User() const;
    [[nodiscard]] std::vector<PlatformRoom> Rooms(std::string& error);
    [[nodiscard]] std::optional<PlatformRoomOverview> RoomOverview(const std::string& roomId, std::string& error);
    bool CreateChannelCategory(const std::string& roomId, const std::string& name, std::string& error);
    bool RenameChannelCategory(const std::string& roomId, const std::string& categoryId,
                               const std::string& name, std::string& error);
    bool DeleteChannelCategory(const std::string& roomId, const std::string& categoryId, std::string& error);
    bool CreateRoomChannel(const std::string& roomId, const std::string& categoryId, const std::string& type,
                           const std::string& name, const std::string& contentRating,
                           const std::string& mediaPostingPolicy, bool localMediaScanRequired,
                           std::string& error);
    bool RenameRoomChannel(const std::string& roomId, const std::string& channelId,
                           const std::string& name, std::string& error);
    bool UpdateRoomChannelSafety(const std::string& roomId, const std::string& channelId,
                                 const std::string& contentRating, const std::string& mediaPostingPolicy,
                                 bool localMediaScanRequired, std::string& error);
    bool DeleteRoomChannel(const std::string& roomId, const std::string& channelId, std::string& error);
    [[nodiscard]] std::vector<PlatformMember> RoomMembers(const std::string& roomId, std::string& error);
    bool JoinRoomCode(const std::string& code, std::string& error);
    bool CreateRoom(const std::string& name, const std::string& description,
                    std::string& createdRoomId, std::string& error);
    bool UpdateRoomDenoise(const std::string& roomId, bool enabled, std::string& error);
    bool UpdateRoomMemberRole(const std::string& roomId, const std::string& userId,
                              const std::string& role, std::string& error);
    bool BanRoomMember(const std::string& roomId, const std::string& userId,
                       const std::string& reason, std::string& error);
    [[nodiscard]] std::optional<RoomInvite> CreateRoomInvite(const std::string& roomId,
                                                             int expiresInHours,
                                                             int maxUses,
                                                             std::string& error);
    [[nodiscard]] std::optional<VoiceGrant> RequestVoiceGrant(const std::string& roomId, bool serverDenoise,
                                                               bool p2pEnabled, std::string& error,
                                                               const std::string& channelId = {});
    [[nodiscard]] std::vector<PlatformFriend> Friends(std::string& error);
    [[nodiscard]] std::vector<PlatformFriend> SearchUsers(const std::string& query, std::string& error);
    bool SendFriendRequest(const std::string& username, std::string& error);
    bool AcceptFriendRequest(const std::string& userId, std::string& error);
    bool DismissFriendRequest(const std::string& userId, std::string& error);
    bool RemoveFriend(const std::string& userId, std::string& error);
    bool BlockUser(const std::string& userId, std::string& error);
    [[nodiscard]] std::optional<std::string> OpenDirectConversation(const std::string& userId, std::string& error);
    [[nodiscard]] std::vector<PlatformNotification> Notifications(std::string& error);
    bool MarkNotificationRead(const std::string& notificationId, std::string& error);
    [[nodiscard]] std::vector<PlatformSession> Sessions(std::string& error);
    bool RevokeSession(const std::string& sessionId, std::string& error);
    [[nodiscard]] std::vector<PlatformClientDevice> ClientDevices(std::string& error);
    bool RevokeClientDevice(const std::string& deviceId, std::string& error);
    bool EnsureMessageDevice(const MessageCrypto& crypto, std::string& error);
    [[nodiscard]] std::optional<RealtimeGrant> RequestRealtimeGrant(std::string& error);
    [[nodiscard]] std::vector<PlatformConversation> Conversations(std::string& error);
    [[nodiscard]] std::vector<MessageDevice> ConversationDevices(const std::string& conversationId, std::string& error);
    [[nodiscard]] std::optional<std::string> ConversationKeyEnvelope(const std::string& conversationId, int epoch,
                                                                     const std::string& deviceId, std::string& error);
    [[nodiscard]] std::optional<std::string> LegalEscrowPublicKey(std::string& error);
    bool UploadKeyEnvelopes(const std::string& conversationId, int epoch, bool initialize,
                            const std::vector<KeyEnvelopeUpload>& envelopes, std::string& error);
    bool SendEncryptedMessage(const EncryptedPlatformMessage& message, std::string& error);
    [[nodiscard]] std::optional<std::uint64_t> DeviceMessageSequence(const std::string& conversationId,
                                                                     const std::string& deviceId, std::string& error);
    [[nodiscard]] std::vector<EncryptedPlatformMessage> SyncMessages(const std::string& conversationId, std::string& error);
    [[nodiscard]] MessageSyncPage SyncMessagePage(const std::string& conversationId, const std::string& afterCursor,
                                                  const std::string& beforeCursor, std::string& error,
                                                  const std::string& channelId = {});
    bool MarkConversationRead(const std::string& conversationId, const std::string& messageId, std::string& error);
    bool MarkChannelRead(const std::string& channelId, const std::string& messageId, std::string& error);
    [[nodiscard]] std::vector<std::string> ChannelPinIds(const std::string& channelId, std::string& error);
    bool PinChannelMessage(const std::string& channelId, const std::string& messageId, std::string& error);
    bool UnpinChannelMessage(const std::string& channelId, const std::string& messageId, std::string& error);
    bool SetChannelNotifications(const std::string& channelId, const std::string& mode,
                                 std::string& error, int muteMinutes = 0);
    [[nodiscard]] std::optional<MediaUploadGrant> InitiateMediaAttachment(
        const MediaAttachmentDraft& draft, std::string& error);
    bool UploadMediaAttachment(const MediaUploadGrant& grant, const std::wstring& sourcePath,
                               std::string& error);
    bool CompleteMediaAttachment(const std::string& id, std::string& error);
    [[nodiscard]] std::optional<MediaDownloadGrant> MediaAttachmentDownload(
        const std::string& id, std::string& error);
    bool DownloadMediaAttachment(const MediaDownloadGrant& grant, const std::wstring& targetPath,
                                 std::string& error);
    [[nodiscard]] std::optional<GuardianClientModel> LatestGuardianClientModel(
        const std::string& channel, std::string& error);
    bool DownloadGuardianClientModel(const GuardianClientModel& model, const std::wstring& targetPath,
                                     std::string& error);
    [[nodiscard]] std::optional<ClientExperiencePolicy> ClientExperience(std::string& error);
    [[nodiscard]] std::optional<MediaSafetyConfig> MediaConfig(std::string& error);
    [[nodiscard]] std::optional<MediaPreferences> GetMediaPreferences(std::string& error);
    bool UpdateMediaPreferences(const MediaPreferences& preferences, std::string& error);
    [[nodiscard]] std::optional<MediaReportReceipt> CreateMediaReport(
        const std::string& reportedUserId,
        const std::string& assetId,
        const std::string& targetType,
        const std::string& targetId,
        const std::string& reason,
        const std::string& note,
        const std::string& evidenceSha256,
        const std::string& idempotencyKey,
        std::string& error);
    bool UploadMediaReportEvidence(const std::string& reportId,
                                   std::span<const std::uint8_t> ciphertext,
                                   const std::string& ciphertextSha256,
                                   const std::string& signature,
                                   const std::string& deviceId,
                                   const std::string& contentMime,
                                   std::string& error);
    [[nodiscard]] std::vector<AccountRestriction> Restrictions(std::string& error);
    bool AppealRestriction(const std::string& restrictionId, const std::string& statement, std::string& error);
    bool WithdrawMediaReport(const std::string& reportId, const std::string& reason, std::string& error);
    bool ReportDiagnosticErrors(std::span<const DiagnosticErrorEvent> events, std::string& error);
    [[nodiscard]] std::string Origin() const;

private:
    enum class RefreshOutcome : std::uint8_t {
        Success,
        SessionExpired,
        UpdateRequired,
        TransientFailure,
    };

    RefreshOutcome Refresh(std::string& error, bool force = false);
    HttpResponse Authorized(const std::wstring& method,
                            const std::string& path,
                            std::string_view body = {},
                            const std::map<std::wstring, std::wstring>& additionalHeaders = {});
    static std::string ErrorFrom(const HttpResponse& response);
    static HttpResponse RefreshFailureResponse(RefreshOutcome outcome);
    void ClearSession(bool clearVault) noexcept;
    void StoreAuthenticatedSession(std::string accessToken, std::string deviceLicense, int expiresInSeconds);

    HttpClient http_;
    CredentialVault vault_;
    mutable std::mutex mutex_;
    std::mutex refreshMutex_;
    std::condition_variable refreshCv_;
    bool refreshInProgress_{};
    RefreshOutcome lastRefreshOutcome_{RefreshOutcome::TransientFailure};
    std::string lastRefreshError_;
    std::string origin_{"https://sonalis.tr"};
    std::string clientDeviceId_;
    std::string accessToken_;
    std::string deviceLicense_;
    PlatformUser user_;
    PlatformSessionState sessionState_{PlatformSessionState::SignedOut};
    std::chrono::steady_clock::time_point accessTokenExpiresAt_{};
};

}  // namespace ss
