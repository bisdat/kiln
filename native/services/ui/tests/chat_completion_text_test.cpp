// SPDX-License-Identifier: MIT
// Pure host regression for native completion snapshots and clear-to-empty.
#include "../chat_completion_text.h"
#include <cassert>
#include <string>

int main() {
    using kui::chat_completion_text::valid;
    assert(valid(""));
    assert(valid(" "));
    assert(valid("   ")); // must clear suggestions after erasing a command
    assert(valid("/he"));
    assert(valid("/say café"));
    assert(valid("/say 😀"));
    assert(valid("/" + std::string(255, 'a')));
    assert(!valid("/" + std::string(256, 'a')));
    assert(!valid("/bad\tinput"));
    assert(!valid("/bad\ninput"));
    assert(!valid(std::string("/\xc3", 2)));
    assert(!valid(std::string("/\x80", 2)));
    assert(!valid(std::string("/\xc0\x80", 3)));
    assert(!valid(std::string("/\xed\xa0\x80", 4)));
    assert(!valid(std::string("/\xf4\x90\x80\x80", 5)));
}
