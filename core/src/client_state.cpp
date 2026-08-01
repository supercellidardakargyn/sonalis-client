#include "sonalis/core/client_state.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>

namespace sonalis::core {
namespace {

template <typename T>
void TrimTail(std::vector<T>& values, const std::size_t maximum) {
    if (values.size() > maximum) values.erase(values.begin(), values.end() - static_cast<std::ptrdiff_t>(maximum));
}

bool Newer(const Message& left, const Message& right) noexcept {
    if (left.createdAtMs != right.createdAtMs) return left.createdAtMs < right.createdAtMs;
    return left.id < right.id;
}

}  // namespace

void ClientState::SignedIn(Account account, const std::chrono::steady_clock::time_point accessExpiresAt) {
    account_ = std::move(account);
    accessExpiresAt_ = accessExpiresAt;
    accountState_ = AccountState::Active;
    Changed();
}

void ClientState::SignedOut() noexcept {
    accountState_ = AccountState::SignedOut;
    account_ = {};
    rooms_.clear();
    ClearRoomProjection();
    accessExpiresAt_ = {};
    Changed();
}

void ClientState::RefreshStarted() noexcept {
    if (accountState_ == AccountState::Active || accountState_ == AccountState::Offline) {
        accountState_ = AccountState::Refreshing;
        Changed();
    }
}

void ClientState::RefreshSucceeded(const std::chrono::steady_clock::time_point accessExpiresAt) noexcept {
    accessExpiresAt_ = accessExpiresAt;
    accountState_ = AccountState::Active;
    Changed();
}

void ClientState::RefreshFailed(const bool terminal) noexcept {
    accountState_ = terminal ? AccountState::Expired : AccountState::Offline;
    if (terminal) accessExpiresAt_ = {};
    Changed();
}

bool ClientState::ShouldRefresh(const std::chrono::steady_clock::time_point now,
                                const std::chrono::seconds margin) const noexcept {
    return accountState_ == AccountState::Active && accessExpiresAt_ != std::chrono::steady_clock::time_point{}
        && now + margin >= accessExpiresAt_;
}

void ClientState::ReplaceRooms(std::vector<Room> rooms) {
    if (rooms.size() > MaximumRooms) rooms.resize(MaximumRooms);
    rooms_ = std::move(rooms);
    const bool selectedExists = std::any_of(rooms_.begin(), rooms_.end(), [this](const Room& room) {
        return room.id == selectedRoomId_;
    });
    if (!selectedExists) ClearRoomProjection();
    Changed();
}

bool ClientState::SelectRoom(std::string roomId) {
    if (std::none_of(rooms_.begin(), rooms_.end(), [&roomId](const Room& room) { return room.id == roomId; })) {
        return false;
    }
    if (selectedRoomId_ != roomId) {
        selectedRoomId_ = std::move(roomId);
        channels_.clear();
        selectedChannelId_.clear();
        ClearMessageProjection();
        Changed();
    }
    return true;
}

void ClientState::ReplaceChannels(std::string roomId, std::vector<Channel> channels) {
    if (roomId != selectedRoomId_) return;
    channels.erase(std::remove_if(channels.begin(), channels.end(), [&roomId](const Channel& channel) {
        return channel.roomId != roomId;
    }), channels.end());
    if (channels.size() > MaximumChannels) channels.resize(MaximumChannels);
    channels_ = std::move(channels);
    const bool selectedExists = std::any_of(channels_.begin(), channels_.end(), [this](const Channel& channel) {
        return channel.id == selectedChannelId_;
    });
    if (!selectedExists) {
        selectedChannelId_.clear();
        ClearMessageProjection();
    }
    Changed();
}

bool ClientState::SelectChannel(std::string channelId) {
    if (std::none_of(channels_.begin(), channels_.end(), [&channelId](const Channel& channel) {
        return channel.id == channelId;
    })) return false;
    if (selectedChannelId_ != channelId) {
        selectedChannelId_ = std::move(channelId);
        ClearMessageProjection();
        Changed();
    }
    return true;
}

void ClientState::ReplaceMessages(std::string channelId, std::vector<Message> messages,
                                  std::string beforeCursor, std::string afterCursor) {
    if (channelId != selectedChannelId_) return;
    messages.erase(std::remove_if(messages.begin(), messages.end(), [&channelId](const Message& message) {
        return message.channelId != channelId;
    }), messages.end());
    std::sort(messages.begin(), messages.end(), Newer);
    messages.erase(std::unique(messages.begin(), messages.end(), [](const Message& left, const Message& right) {
        return left.id == right.id;
    }), messages.end());
    TrimTail(messages, MaximumMessages);
    messages_ = std::move(messages);
    beforeCursor_ = std::move(beforeCursor);
    afterCursor_ = std::move(afterCursor);
    Changed();
}

void ClientState::MergeMessages(std::string channelId, std::vector<Message> messages,
                                std::string beforeCursor, std::string afterCursor) {
    if (channelId != selectedChannelId_) return;
    std::unordered_map<std::string, std::size_t> positions;
    positions.reserve(messages_.size() + messages.size());
    for (std::size_t index = 0; index < messages_.size(); ++index) positions.emplace(messages_[index].id, index);
    for (auto& message : messages) {
        if (message.channelId != channelId || message.id.empty()) continue;
        const auto found = positions.find(message.id);
        if (found == positions.end()) {
            positions.emplace(message.id, messages_.size());
            messages_.push_back(std::move(message));
        } else {
            messages_[found->second] = std::move(message);
        }
    }
    std::sort(messages_.begin(), messages_.end(), Newer);
    TrimTail(messages_, MaximumMessages);
    if (!beforeCursor.empty()) beforeCursor_ = std::move(beforeCursor);
    if (!afterCursor.empty()) afterCursor_ = std::move(afterCursor);
    Changed();
}

void ClientState::MarkRead(const std::string channelId) noexcept {
    if (channelId.empty()) return;
    for (auto& channel : channels_) {
        if (channel.id == channelId) {
            channel.unreadCount = 0;
            channel.mentionCount = 0;
        }
    }
    Changed();
}

ClientSnapshot ClientState::Snapshot() const {
    return {accountState_, account_, rooms_, channels_, messages_, selectedRoomId_, selectedChannelId_,
            beforeCursor_, afterCursor_, generation_};
}

void ClientState::ClearRoomProjection() noexcept {
    selectedRoomId_.clear();
    channels_.clear();
    selectedChannelId_.clear();
    ClearMessageProjection();
}

void ClientState::ClearMessageProjection() noexcept {
    messages_.clear();
    beforeCursor_.clear();
    afterCursor_.clear();
}

ReconnectBackoff::ReconnectBackoff(std::chrono::milliseconds minimum,
                                   std::chrono::milliseconds maximum) noexcept
    : minimum_(std::max(minimum, std::chrono::milliseconds(1))),
      maximum_(std::max(maximum, minimum_)) {}

std::chrono::milliseconds ReconnectBackoff::Next(const std::uint32_t jitterBasisPoints) noexcept {
    const std::uint32_t exponent = std::min(attempts_, 30U);
    ++attempts_;
    const std::uint64_t multiplier = std::uint64_t{1} << exponent;
    const std::uint64_t base = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(maximum_.count()),
        static_cast<std::uint64_t>(minimum_.count()) * multiplier);
    const std::uint64_t boundedJitter = std::min<std::uint64_t>(jitterBasisPoints, 2'500U);
    const std::uint64_t value = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(maximum_.count()), base + (base * boundedJitter / 10'000U));
    return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(value));
}

}  // namespace sonalis::core
