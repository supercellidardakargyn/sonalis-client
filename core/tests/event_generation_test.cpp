#include "sonalis/core/event_generation.h"

#include <cassert>
#include <cstdint>

int main() {
    sonalis::core::EventGeneration state;
    std::uint64_t token = 99;
    assert(!state.TryBegin(token));
    assert(token == 0);

    assert(state.MarkDirty() == 1);
    assert(state.TryBegin(token));
    assert(token == 1);
    assert(!state.TryBegin(token));
    assert(state.MarkDirty() == 2);
    assert(state.Complete(1));
    assert(state.TryBegin(token));
    assert(token == 2);
    assert(!state.Complete(2));
    assert(!state.InFlight());

    (void)state.MarkDirty();
    state.Reset();
    assert(state.Current() == 0);
    assert(!state.TryBegin(token));
    return 0;
}
