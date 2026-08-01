#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace ss {

enum class Language : unsigned char {
    English, Turkish, German, French, Spanish, Portuguese, Italian, Russian,
    Arabic, Japanese, Korean, SimplifiedChinese,
};

struct LanguageOption {
    Language language;
    std::string_view code;
    std::string_view nativeName;
};

enum class TextId : std::size_t {
    AccountLogin, PasswordNote, ControlPlane, UsernameOrEmail, Password, Connecting, Login, CreateAccount,
    Voice, Rooms, Messages, Settings, Logout, ExitApplication, VoiceRooms, RoomManagement, Conversations,
    AudioSettings, Room, NoRooms, InviteCode, JoinWithCode, RefreshRooms, NewRoom, RoomName, CreateRoom,
    MembersAndManagement, SearchUsername, Search, AudioDevices, Microphone, Output, RefreshDevices,
    SmartTransmission, AutomaticVad, PushToTalk, ContinuousTransmission, Sensitivity, Connect, Disconnect,
    ApplicationSettings, About, HideMembers, Members, UnmuteMicrophone, MuteMicrophone, UnmuteAll, MuteAll,
    LocalControlsOnly, NoOtherUsers, Mute, EncryptedRoomMessages, EncryptedDirectMessage, WriteMessage, Send,
    Ready, VoiceSessionActive, LanguageLabel, LanguageChanged, UpToDate, Checking, UpdateAvailable,
    Downloading, ReadyToInstall, CheckFailed, Later, StartInstallation,
    Refreshing, RefreshMembers, ServerDenoiseForRoom, ServerDenoiseEligibility, DurationHours, MaximumUses,
    CreateInvite, Copy, MemberRole, ModeratorRole, AdministratorRole, Ban, Cancel, SendFriendRequest,
    RefreshFriends, Accept, Reject, RemoveFriend, Block, MarkRead, DirectP2P, DirectP2PPrivacy,
    TestEncryptedVoice, UsersLabel, LiveDeviceChanges, ContinuousWarning, SwitchToAutomaticVad, RuntimeStatus,
    ExportDiagnostics, MicrophoneLabel, BackToRoomMessages, RefreshMessages, Reply, Edit, Delete, ModeratorRemove,
    RemoveMessage, ModerationReasonRequired, SomeoneTyping, UpdateReadyTitle, NoDecryptableMessages, Count,
};

[[nodiscard]] const std::array<LanguageOption, 12>& SupportedLanguages() noexcept;
[[nodiscard]] Language ParseLanguage(std::string_view code) noexcept;
[[nodiscard]] std::string_view LanguageCode(Language language) noexcept;
[[nodiscard]] const char* LanguageDisplayName(Language language) noexcept;
[[nodiscard]] const char* Translate(Language language, TextId id) noexcept;
[[nodiscard]] bool IsRightToLeft(Language language) noexcept;

}  // namespace ss
