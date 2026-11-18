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

    // What ends a browse: typed characters and the keys that edit without one; never Enter, Escape or a selection.
    using kui::chat_keys::edits_text;
    using kui::chat_keys::inserts_text;
    assert(inserts_text('a') && inserts_text(' ') && inserts_text('\t') && inserts_text(0x4f60));
    assert(!inserts_text('\r') && !inserts_text(0x1b) && !inserts_text(0x08) && !inserts_text(0x7f));
    assert(!inserts_text(0x01) && !inserts_text(0x03) && !inserts_text(0x16));  // Ctrl+A, Ctrl+C, Ctrl+V characters
    assert(edits_text(0x08, 0) && edits_text(0x2E, 0) && edits_text(0x2D, 2));    // Backspace, Delete, Shift+Insert
    assert(edits_text('V', 1) && edits_text('X', 1) && edits_text('Z', 1) && edits_text('Y', 1));
    assert(!edits_text('A', 1) && !edits_text('C', 1) && !edits_text(0x2D, 1));  // select all, copy, Ctrl+Insert copy
    assert(!edits_text('V', 0) && !edits_text(0x26, 0) && !edits_text(0x28, 0));

    // Phase 1b: no completion candidates must leave native Tab alone.
    q.completion(true, false, false);
    assert(!q.key(0x09, 0, true));
    assert(!q.key(0x09, 2, true));
    assert(!q.take_tab_character());
    assert(!q.key(0x1b, 0, true)); // Escape remains vanilla with the list closed
    assert(q.key(0x26, 0, true));  // passive suggestion list leaves history active
    assert(q.take(action) && action == kui::chat_keys::Action::Older);

    // An offered completion activates the Tab and Shift+Tab handoff.
    q.completion(true, true, false);
    assert(q.key(0x09, 0, true));
    assert(q.take_tab_character());     // drop only the paired WM_CHAR '\\t'
    assert(!q.take_tab_character());
    assert(q.key(0x09, 2, true));
    assert(q.take_tab_character());
    assert(!q.key(0x09, 0, false));  // never steal Tab key release
    assert(q.take(action) && action == kui::chat_keys::Action::CompleteNext);
    assert(q.take(action) && action == kui::chat_keys::Action::CompletePrevious);
    assert(!q.take(action));

    // Once Tab activates the panel, arrows navigate it instead of history.
    q.completion(true, true, true);
    assert(q.key(0x26, 0, true));
    assert(q.key(0x28, 0, true));
    assert(q.key(0x26, 1, true));  // Ctrl+Up explicitly means history now
    assert(q.key(0x28, 1, true));  // Ctrl+Down restores history draft
    assert(q.key(0x1b, 0, true));  // Esc dismisses the active panel only
    assert(!q.key(0x1b, 0, false)); // and its release reaches Enshrouded
    assert(q.take(action) && action == kui::chat_keys::Action::SelectOlder);
    assert(q.take(action) && action == kui::chat_keys::Action::SelectNewer);
    assert(q.take(action) && action == kui::chat_keys::Action::Older);
    assert(q.take(action) && action == kui::chat_keys::Action::Newer);
    assert(q.take(action) && action == kui::chat_keys::Action::Dismiss);
    assert(!q.take(action));

    // No Alt/Ctrl Tab interception, and never intercept unfocused gameplay.
    assert(!q.key(0x09, 1, true));
    assert(!q.key(0x09, 4, true));
    assert(!q.key(0x26, 4, true));
    q.focus(false);
    assert(!q.key(0x09, 0, true));
    assert(!q.take_tab_character());
    assert(!q.take(action));
    q.focus(true);
    assert(!q.key(0x09, 0, true)); // stale suggestions were cleared on blur

    // Completion may operate without separately enabling chat history.
    q.history(false);
    q.completion(true, true, true);
    assert(!q.key(0x26, 1, true)); // Ctrl+Up unavailable without history
    assert(q.key(0x26, 0, true));
    assert(q.take(action) && action == kui::chat_keys::Action::SelectOlder);
    q.completion(false, false, false);
    assert(!q.key(0x26, 0, true));
    assert(!q.key(0x09, 0, true));
    q.history(true);
}
