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
    assert(q.key(0x09, 0, true)); // duplicate raw/legacy down: swallow, no extra action
    assert(q.take_tab_character());
    assert(!q.key(0x09, 0, false)); // real key-up permits the next press
    assert(q.key(0x09, 2, true));
    assert(q.take_tab_character());
    assert(!q.key(0x09, 0, false));  // never steal Tab key release
    assert(q.take(action) && action == kui::chat_keys::Action::CompleteNext);
    assert(q.take(action) && action == kui::chat_keys::Action::CompletePrevious);
    assert(!q.take(action)); // duplicated down cannot cycle twice

    // When Proton supplies WM_CHAR before any usable Tab keydown, the text
    // fallback routes it via key(). If a delayed raw keydown follows, it
    // must not add a second completion action for the same press.
    assert(q.key(0x09, 0, true));
    assert(q.take_tab_character());
    assert(q.key(0x09, 0, true)); // delayed raw keydown
    assert(q.take_tab_character());
    assert(q.take(action) && action == kui::chat_keys::Action::CompleteNext);
    assert(!q.take(action));
    assert(!q.key(0x09, 0, false));


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

    // Phase 1c: search is gated by history, verified chat focus and its
    // independent feature switch; it never steals gameplay or key releases.
    q.search_enabled(true);
    q.focus(true);
    q.completion(true, true, true);
    assert(q.key('R', 1, true)); // Ctrl+R opens search, even over completion
    assert(q.searching());
    assert(q.key('R', 1, true)); // raw + legacy duplicate, no second action
    assert(!q.key('R', 1, false));
    assert(q.take(action) && action == kui::chat_keys::Action::SearchStart);
    assert(!q.take(action));
    assert(q.search_character('a'));
    uint32_t cp = 0;
    assert(q.take(action, &cp) && action == kui::chat_keys::Action::SearchCharacter && cp == 'a');
    assert(q.key(0x08, 0, true)); // Backspace changes the private query
    assert(q.key(0x26, 0, true)); // Up selects newer, not history
    assert(q.key(0x28, 0, true)); // Down selects older, not history
    assert(q.take(action) && action == kui::chat_keys::Action::SearchBackspace);
    assert(q.take(action) && action == kui::chat_keys::Action::SearchNewer);
    assert(q.take(action) && action == kui::chat_keys::Action::SearchOlder);
    assert(q.key('R', 1, true));
    assert(!q.key('R', 1, false));
    assert(q.key('R', 3, true)); // Ctrl+Shift+R moves newer
    assert(!q.key('R', 3, false));
    assert(q.take(action) && action == kui::chat_keys::Action::SearchOlder);
    assert(q.take(action) && action == kui::chat_keys::Action::SearchNewer);
    assert(q.key(0x09, 0, true)); // Tab accepts, not completion
    assert(q.key(0x09, 0, true)); // duplicate press still only one action
    assert(q.take_tab_character()); // paired WM_CHAR Tab swallowed
    assert(!q.key(0x09, 0, false));
    assert(q.take(action) && action == kui::chat_keys::Action::SearchAccept);
    assert(!q.take(action));

    // Enter's paired WM_CHAR must be swallowed even if UI accepted and closed
    // search before that char arrived. Otherwise the game could send a command.
    assert(q.key(0x0d, 0, true));
    assert(q.take(action) && action == kui::chat_keys::Action::SearchAccept);
    q.search(false);
    assert(!q.key(0x0d, 0, false)); // Proton may deliver the character after key-up
    assert(q.search_character(0x0d)); // the accepted Enter must not send
    assert(!q.search_character(0x0d));
    assert(!q.key(0x0d, 0, true)); // a new ordinary Enter remains native
    assert(!q.search_character(0x0d));
    assert(!q.key(0x0d, 0, false));
    assert(!q.key(0x1b, 0, true)); // native chat still owns Esc

    assert(q.key('R', 1, true));
    assert(!q.key('R', 1, false));
    assert(q.take(action) && action == kui::chat_keys::Action::SearchStart);
    assert(q.key(0x1b, 0, true)); // Esc cancels search
    assert(q.take(action) && action == kui::chat_keys::Action::SearchCancel);
    q.search(false);
    assert(!q.key(0x1b, 0, false)); // paired WM_CHAR may arrive after key-up
    assert(q.search_character(0x1b)); // must not close the game's chat
    assert(!q.search_character(0x1b));
    assert(!q.key(0x1b, 0, true)); // a subsequent native Escape is untouched
    assert(!q.search_character(0x1b));
    assert(!q.key(0x1b, 0, false));

    assert(q.key('R', 1, true));
    assert(!q.key('R', 1, false));
    assert(q.take(action) && action == kui::chat_keys::Action::SearchStart);
    q.ime_started(); // fail closed, IME owns Enter/Escape
    assert(!q.searching());
    assert(q.take(action) && action == kui::chat_keys::Action::SearchCancel);
    assert(!q.key(0x0d, 0, true));
    assert(!q.key(0x1b, 0, true));
    assert(q.key('R', 1, true));
    assert(!q.key('R', 1, false));
    assert(q.take(action) && action == kui::chat_keys::Action::SearchStart);
    q.focus(false);
    assert(!q.searching() && !q.take(action));
    assert(!q.key('R', 1, true));
    q.focus(true);
    q.search_enabled(false);
    assert(!q.key('R', 1, true));
    q.history(false);
    q.search_enabled(true);
    assert(!q.key('R', 1, true)); // no reverse search when ui.chatHistory is off
}
