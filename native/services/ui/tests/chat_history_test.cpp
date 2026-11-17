// SPDX-License-Identifier: MIT
#include "../chat_history.h"
#include <cassert>
#include <string>
#include <vector>
using kui::chat_history::History;

int main() {
    History h(3);
    h.load({"/first", "/second", "/second", "/register abc", "hello"});
    assert(h.count() == 2);
    assert(h.older("unsent") == "/second");
    assert(h.older("/second") == "/first");
    assert(h.older("/first") == "/first");
    assert(h.newer("/first") == "/second");
    assert(h.newer("/second") == "unsent");
    assert(h.newer("unsent") == "unsent");
    assert(!h.submit("/PASSWORD topsecret"));
    assert(!h.submit("/foo --token=abc"));
    assert(!h.submit("/login abc"));
    assert(!h.submit("/bad\nline"));
    assert(!h.submit(std::string(1025, 'a')));
    assert(h.count() == 2);
    assert(!h.submit("normal chat"));
    assert(h.count() == 3);
    assert(h.persisted() == std::vector<std::string>({"/first", "/second"}));
    assert(h.older("draft") == "normal chat");
    h.edited();
    assert(h.older("modified") == "normal chat");
    assert(h.newer("normal chat") == "modified");
    assert(h.submit("/third"));
    assert(h.persisted() == std::vector<std::string>({"/second", "/third"}));
    assert(!h.submit("/third"));  // repeated adjacent command
    assert(h.submit("plain"));  // eviction of a persisted entry changes the disk snapshot
    assert(h.submit("/fourth"));
    assert(h.persisted() == std::vector<std::string>({"/third", "/fourth"}));
    assert(History::valid("/say café"));
    assert(History::valid("/say 你好"));
    assert(!History::valid("\xc0\x80"));
    assert(!History::valid("\xed\xa0\x80"));
    assert(!History::valid("\xf4\x90\x80\x80"));
    assert(History::sensitive("/key ABC"));
    assert(!History::sensitive("/keyboard"));
    // A normal five-byte slash command must be accepted. In-game issue #44
    // logging showed bytes=5/command=1 but count stayed zero.
    History direct;
    assert(History::valid("/help"));
    assert(!History::sensitive("/help"));
    assert(direct.submit("/help"));
    assert(direct.count() == 1);
    assert(direct.older("") == "/help");

    // Exact shape of the 17 Nov in-game failure: the private edit view
    // contained five bytes with a slash prefix and three control bytes.
    // The caller-owned widget exposed the valid five-byte /help.
    const std::string invalid_private{'/', '\x01', '\x02', '\x03', 'p'};
    assert(invalid_private.size() == 5);
    assert(!History::valid(invalid_private));
    History from_widget;
    assert(!from_widget.submit(invalid_private));
    assert(from_widget.count() == 0);
    assert(from_widget.submit("/help"));
    assert(from_widget.count() == 1);
    assert(from_widget.older("draft") == "/help");
    assert(from_widget.newer("/help") == "draft");

}
