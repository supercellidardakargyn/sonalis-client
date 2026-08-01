#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sonalis::core {

enum class AccountState : std::uint8_t { SignedOut, Active, Refreshing, Offline, Expired };
enum class ChannelKind : std::uint8_t { Text, Voice };

struct Account final {
    std::string id;
    std::string username;
    std::string nickname;
};

struct Room final {
    std::string id;
    std::string name;
    std::string role;
    std::uint32_t unreadCount{};
    std::uint32_t mentionCount{};
};

struct Channel final {
    std::string id;
    std::string roomId;
    std::string categoryId;
    std::string name;
    ChannelKind kind{ChannelKind::Text};
    std::uint32_t unreadCount{};
    std::uint32_t mentionCount{};
};

struct Message final {
    std::string id;
    std::string channelId;
    std::string senderId;
    std::string ciphertext;
    std::string cursor;
    std::uint64_t clientSequence{};
    std::uint64_t createdAtMs{};
    bool pending{};
    bool failed{};
};

struct ClientSnapshot final {
    AccountState accountState{AccountState::SignedOut};
    Account account;
    std::vector<Room> rooms;
    std::vector<Channel> channels;
    std::vector<Message> messages;
    std::string selectedRoomId;
    std::string selectedChannelId;
    std::string beforeCursor;
    std::string afterCursor;
    std::uint64_t generation{};
};

// Bounded, platform-independent projection used by all native shells. Network
// and crypto workers publish complete snapshots; UI threads copy one immutable
// projection and never hold platform socket/audio locks.
class ClientState final {
public:
    static constexpr std::size_t MaximumRooms = 100;
    static constexpr std::size_t MaximumChannels = 100;
    static constexpr std::size_t MaximumMessages = 300;

    void SignedIn(Account account, std::chrono::steady_clock::time_point accessExpiresAt);
    void SignedOut() noexcept;
    void RefreshStarted() noexcept;
    void RefreshSucceeded(std::chrono::steady_clock::time_point accessExpiresAt) noexcept;
    void RefreshFailed(bool terminal) noexcept;
    [[nodiscard]] bool ShouldRefresh(std::chrono::steady_clock::time_point now,
                                     std::chrono::seconds margin = std::chrono::seconds(60)) const noexcept;

    void ReplaceRooms(std::vector<Room> rooms);
    bool SelectRoom(std::string roomId);
    void ReplaceChannels(std::string roomId, std::vector<Channel> channels);
    bool SelectChannel(std::string channelId);
    void ReplaceMessages(std::string channelId, std::vector<Message> messages,
                         std::string beforeCursor, std::string afterCursor);
    void MergeMessages(std::string channelId, std::vector<Message> messages,
                       std::string beforeCursor, std::string afterCursor);
    void MarkRead(std::string channelId) noexcept;
    [[nodiscard]] ClientSnapshot Snapshot() const;

private:
    void Changed() noexcept { ++generation_; }
    void ClearRoomProjection() noexcept;
    void ClearMessageProjection() noexcept;

    AccountState accountState_{AccountState::SignedOut};
    Account account_;
    std::vector<Room> rooms_;
    std::vector<Channel> channels_;
    std::vector<Message> messages_;
    std::string selectedRoomId_;
    std::string selectedChannelId_;
    std::string beforeCursor_;
    std::string afterCursor_;
    std::chrono::steady_clock::time_point accessExpiresAt_{};
    std::uint64_t generation_{};
};

class ReconnectBackoff final {
public:
    explicit ReconnectBackoff(std::chrono::milliseconds minimum = std::chrono::seconds(1),
                              std::chrono::milliseconds maximum = std::chrono::seconds(30)) noexcept;
    [[nodiscard]] std::chrono::milliseconds Next(std::uint32_t jitterBasisPoints = 0) noexcept;
    void Reset() noexcept { attempts_ = 0; }
    [[nodiscard]] std::uint32_t Attempts() const noexcept { return attempts_; }

private:
    std::chrono::milliseconds minimum_;
    std::chrono::milliseconds maximum_;
    std::uint32_t attempts_{};
};

}  // namespace sonalis::core
