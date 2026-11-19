// SPDX-License-Identifier: MIT
// Host test of the block between "boot-json:begin" and "boot-json:end" in native/core/src/boot.cpp, which
// test-boot-json.sh cuts out of that file as it is (boot_json_block.inc) and builds into this file with the host's C++
// compiler. The exit code is the number of checks that failed.
#include <cstdio>
#include <exception>
#include <string>

#include "nlohmann/json.hpp"

namespace kiln_test {
#include "boot_json_block.inc"
}  // namespace kiln_test

namespace {
using nlohmann::json;
int g_failed = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

// boot_json_text(doc), or the exception's text with a "threw: " prefix.
std::string text_of(const json& doc) {
    try {
        return kiln_test::boot_json_text(doc);
    } catch (const std::exception& e) {
        return std::string("threw: ") + e.what();
    }
}

void boot_json() {
    const json good = {{"format", 1}, {"safeIdle", nullptr}, {"paths", {{"game", "C:/Spiele/Enshrouded"}}}};
    check(text_of(good) == good.dump(2) + "\n", "a UTF-8 document: dump(2) and a newline, as before");

    // A JSON parser's error copies the offending byte into boot's safe-idle text; a Windows message in the ANSI code
    // page (a German "für" in Windows-1252) can reach a chain entry's error. Neither may end Kiln's boot.
    const json latin1 = {{"format", 1},
                         {"safeIdle", "build record: parse error at line 1, column 9: invalid string: ill-formed UTF-8 byte; last read: '\"\xE4'"},
                         {"chain", json::array({{{"path", "mods/x.dll"}, {"error", "Zugriff f\xFCr x.dll verweigert"}}})}};
    const std::string t = text_of(latin1);
    check(t.rfind("threw: ", 0) != 0, "a document with bytes that are not UTF-8 is written, not thrown");
    check(t.find("\xEF\xBF\xBD") != std::string::npos, "each bad byte becomes U+FFFD");
    check(!t.empty() && t.back() == '\n', "the text ends with a newline");
    check(!json::parse(t.rfind("threw: ", 0) == 0 ? std::string("x") : t, nullptr, false).is_discarded(),
          "the text written is JSON a reader can parse");
    check(t.find("Zugriff f\xEF\xBF\xBDr x.dll") != std::string::npos, "the rest of the text is kept");
}
}  // namespace

int main() {
    boot_json();
    std::printf("%s: %d check(s) failed\n", g_failed ? "FAIL" : "ok", g_failed);
    return g_failed;
}
