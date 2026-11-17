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
    assert(!h.submit("plain"));
    assert(h.submit("/fourth"));
    assert(h.persisted() == std::vector<std::string>({"/third", "/fourth"}));
    assert(History::valid("/say café"));
    assert(History::valid("/say 你好"));
    assert(!History::valid("\xc0\x80"));
    assert(!History::valid("\xed\xa0\x80"));
    assert(!History::valid("\xf4\x90\x80\x80"));
    assert(History::sensitive("/key ABC"));
    assert(!History::sensitive("/keyboard"));
}
