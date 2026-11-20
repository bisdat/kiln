// SPDX-License-Identifier: MIT
// Host test of one kiln.log line: the block between "log-line:begin" and "log-line:end" in native/core/src/log.cpp,
// which test-log-line.sh cuts out of that file as it is (log_line_block.inc) and builds into this file. The exit code is
// the number of checks that failed.
#include <cstdio>
#include <string>
#include <string_view>

namespace kiln_test {
#include "log_line_block.inc"
}  // namespace kiln_test

namespace {
int g_failed = 0;

void check(bool ok, const char* what, const std::string& line) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        std::printf("      got: ");
        for (const char c : line) {
            if (c == '\0') std::printf("<NUL>");
            else if (c == '\n') std::printf("<LF>");
            else if (c == '\r') std::printf("<CR>");
            else std::putchar(c);
        }
        std::printf("\n");
        ++g_failed;
    }
}

std::string line(std::string_view text) { return kiln_test::log_line('I', 9, 5, 7, 42, "my.mod", text); }
}  // namespace

int main() {
    using namespace std::string_view_literals;
    const std::string plain = line("hello");
    check(plain == "[I 09:05:07.042] [my.mod] hello\n", "a plain line as before", plain);

    // KilnApi.log(owner, level, {"a\0b", 3}): both halves, the NUL as the two characters \0, one line
    const std::string nul = line("a\0b"sv);
    check(nul == "[I 09:05:07.042] [my.mod] a\\0b\n", "a text with a NUL keeps both halves, the NUL written as \\0", nul);
    check(nul.find('\0') == std::string::npos, "no raw NUL reaches kiln.log", nul);
    check(nul.find('\n') == nul.size() - 1, "one line", nul);

    const std::string many = line("\0x\0\0"sv);
    check(many == "[I 09:05:07.042] [my.mod] \\0x\\0\\0\n", "a NUL at the start, the end and twice in a row", many);

    const std::string crlf = line("one\r\ntwo"sv);
    check(crlf == "[I 09:05:07.042] [my.mod] one\r\ntwo\n", "CR and LF pass through as they are", crlf);

    const std::string empty = line(""sv);
    check(empty == "[I 09:05:07.042] [my.mod] \n", "an empty text", empty);

    const std::string back = line("C:\\x\\0y"sv);
    check(back == "[I 09:05:07.042] [my.mod] C:\\x\\0y\n", "a backslash is not doubled (the escape is for NUL only)", back);

    std::printf("%s: %d check(s) failed\n", g_failed ? "FAIL" : "ok", g_failed);
    return g_failed;
}
