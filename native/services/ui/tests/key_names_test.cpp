// SPDX-License-Identifier: MIT
// Host test of key_names.h (GitHub issue 33): every code a binding can hold has one name of its own, and a punctuation
// binding is the key at its place on any layout (the key left of 1 is scan code 0x29 whatever virtual key the layout
// gives it), while letters and the rest stay virtual keys. Kiln.Ui's half, and the check that both tables agree, is
// managed/tests/Kiln.Ui.Tests/KeyNameTests.cs.
#include "../key_names.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace kui::keys;

namespace {
int g_fails = 0, g_checks = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    ++g_checks;
    if (!ok) ++g_fails;
}

std::string of(uint32_t vk) {
    char b[32];
    name(vk, b, sizeof b);
    return b;
}

void names() {
    std::set<std::string> seen;
    bool all = true, unique = true;
    for (uint32_t vk = 1; vk <= 0x103; ++vk) {
        const auto n = of(vk);
        if (n.empty()) all = false;
        if (!seen.insert(n).second) {
            unique = false;
            std::printf("  two codes are named %s\n", n.c_str());
        }
    }
    check(all, "every code 1..255 and the wheel has a name");
    check(unique, "no two codes share a name");
    check(of(0xDF) == "Oem8" && of(0xE2) == "IntlBackslash" && of(0xC0) == "Backquote" && of(0xFE) == "OemClear",
          "the OEM keys are named (VK_OEM_8 was Key223 in dev.27)");
    check(of(0x07) == "Key7" && of(0xFF) == "Key255" && of(0xE8) == "Key232", "a code without a name is Key<decimal>");
    check(of(0x70) == "F1" && of(0x87) == "F24" && of(0x41) == "A" && of(0x30) == "0" && of(0x65) == "Numpad5" &&
              of(0x2D) == "Insert" && of(0x100) == "WheelUp",
          "function keys, letters, digits, the keypad and the table");
    char tiny[8];
    volatile size_t cut = 4;  // not folded: gcc would warn about the cut it is asked to make
    name(0xDF, tiny, cut);
    check(std::strcmp(tiny, "Oem") == 0, "a short buffer gets a cut, terminated name");
    for (const auto& n : kNames) {
        const std::string s = n.name;
        if (s.size() > 3 && s.rfind("Key", 0) == 0 && s[3] >= '0' && s[3] <= '9') {
            check(false, "no table name looks like a Key<number>");
            return;
        }
    }
    check(true, "no table name looks like a Key<number>");
}

void places() {
    // the key left of 1 on four layouts, and a key injected without a scan code
    check(press_code(0xC0, 0x29) == 0xC0, "US: VK_OEM_3 at 0x29 is Backquote");
    check(press_code(0xDF, 0x29) == 0xC0, "UK: VK_OEM_8 at 0x29 is Backquote (issue 33)");
    check(press_code(0xDC, 0x29) == 0xC0, "German: VK_OEM_5 at 0x29 is Backquote");
    check(press_code(0xDE, 0x29) == 0xC0, "French: VK_OEM_7 at 0x29 is Backquote");
    check(press_code(0xC0, 0) == 0xC0, "no scan code: the virtual key");
    // UK's ' key sends VK_OEM_3 at the place US has '
    check(press_code(0xC0, 0x28) == 0xDE, "UK: VK_OEM_3 at 0x28 is Quote, not Backquote");
    check(!matches(0xC0, 0xC0, 0x28) && matches(0xDE, 0xC0, 0x28), "a Backquote binding does not fire on UK's ' key");
    check(matches(0xC0, 0xDF, 0x29) && matches(0xC0, 0xDC, 0x29) && matches(0xC0, 0xC0, 0), "a Backquote binding fires");
    // a dev.27 save of Key223 (a code that names no place) still fires on that virtual key
    check(matches(0xDF, 0xDF, 0x29) && !matches(0xDF, 0xC0, 0x29), "Key223 fires on VK_OEM_8 wherever it is");
    // letters stay virtual keys where a layout moves them (Dvorak's S at US ;, AZERTY's M there too)
    check(press_code('S', 0x27) == 'S' && matches('S', 'S', 0x27) && !matches(0xBA, 'S', 0x27),
          "a letter at a punctuation place is the letter");
    // a punctuation code at a place that has none (AZERTY's , at US M) is its virtual key
    check(press_code(0xBC, 0x32) == 0xBC && matches(0xBC, 0xBC, 0x32), "an OEM key away from the places is its code");
    // the keypad's / (E0 35) is not Slash (35)
    check(press_code(0x6F, 0x135) == 0x6F && !matches(0xBF, 0x6F, 0x135), "an E0 key is not a place");
    check(press_code(0xE2, 0x56) == 0xE2 && press_code(0xDC, 0x56) == 0xE2, "the ISO key is IntlBackslash on any layout");
    bool named = true;
    for (const auto& p : kPlaces) {
        if (!oem(p.vk) || !is_place(p.vk) || p.scan >= 0x100) named = false;
        for (const auto& q : kPlaces)
            if (&p != &q && (p.scan == q.scan || p.vk == q.vk)) named = false;
    }
    check(named, "places are distinct, plain scan codes with OEM codes");
}
}  // namespace

int main() {
    names();
    places();
    std::printf("%s: key_names %d checks, %d failed\n", g_fails ? "FAIL" : "PASS", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
