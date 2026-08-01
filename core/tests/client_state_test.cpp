#include "sonalis/core/client_state.h"

#include <cassert>
#include <chrono>
#include <string>
#include <vector>

using namespace std::chrono_literals;

int main() {
    using sonalis::core::Channel;
    using sonalis::core::ChannelKind;
    using sonalis::core::ClientState;
    using sonalis::core::Message;
    using sonalis::core::ReconnectBackoff;
    using sonalis::core::Room;

    ClientState state;
    const auto now = std::chrono::steady_clock::now();
    state.SignedIn({"user", "arda", "Arda"}, now + 15min);
    assert(!state.ShouldRefresh(now));
    assert(state.ShouldRefresh(now + 14min));

    std::vector<Room> rooms;
    for (int index = 0; index < 120; ++index) rooms.push_back({std::to_string(index), "Room", "member"});
    state.ReplaceRooms(std::move(rooms));
    assert(state.Snapshot().rooms.size() == ClientState::MaximumRooms);
    assert(state.SelectRoom("2"));
    assert(!state.SelectRoom("119"));

    state.ReplaceChannels("2", {{"text", "2", "general", "genel", ChannelKind::Text},
                                 {"voice", "2", "general", "Genel Ses", ChannelKind::Voice}});
    assert(state.SelectChannel("text"));

    std::vector<Message> messages;
    for (int index = 0; index < 340; ++index) {
        messages.push_back({std::to_string(index), "text", "user", "cipher", std::to_string(index),
                            static_cast<std::uint64_t>(index), static_cast<std::uint64_t>(index)});
    }
    state.ReplaceMessages("text", std::move(messages), "before", "after");
    assert(state.Snapshot().messages.size() == ClientState::MaximumMessages);
    state.MergeMessages("text", {{"339", "text", "user", "changed", "339", 339, 339, false, false},
                                  {"new", "text", "user", "new", "new", 400, 400, true, false}},
                        {}, "new-after");
    const auto snapshot = state.Snapshot();
    assert(snapshot.messages.size() == ClientState::MaximumMessages);
    assert(snapshot.messages.back().id == "new");
    assert(snapshot.afterCursor == "new-after");

    state.MarkRead("text");
    state.RefreshStarted();
    state.RefreshFailed(false);
    assert(state.Snapshot().accountState == sonalis::core::AccountState::Offline);
    state.RefreshSucceeded(now + 15min);

    ReconnectBackoff backoff;
    assert(backoff.Next() == 1s);
    assert(backoff.Next() == 2s);
    assert(backoff.Next() == 4s);
    backoff.Reset();
    assert(backoff.Next(1'000) == 1100ms);
    return 0;
}
