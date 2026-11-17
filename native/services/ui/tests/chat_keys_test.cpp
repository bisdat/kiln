// SPDX-License-Identifier: MIT
// Pure host test: native chat history consumes arrows only with its own edit focused.
#include "../chat_keys.h"
#include <cassert>

int main() {
    kui::chat_keys::Router q;
    kui::chat_keys::Action action{};
    assert(!q.key(0x26, 0, true));
    assert(!q.take(action));
    q.focus(true);
    assert(!q.key(0x26, 1, true)); // Ctrl+Up not reserved yet
    assert(!q.key(0x41, 0, true)); // other keys still reach the game
    assert(q.key(0x26, 0, true));
    assert(q.key(0x28, 0, true));
    assert(!q.key(0x26, 0, false)); // release is forwarded
    assert(q.take(action) && action == kui::chat_keys::Action::Older);
    assert(q.take(action) && action == kui::chat_keys::Action::Newer);
    assert(!q.take(action));
    assert(q.key(0x26, 0, true));
    q.focus(false);
    assert(!q.take(action)); // closing discards stale input
    q.focus(true);
    assert(!q.take(action));
    assert(q.key(0x28, 0, true));
    assert(q.take(action) && action == kui::chat_keys::Action::Newer);
}
