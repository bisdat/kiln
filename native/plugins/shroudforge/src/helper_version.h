// SPDX-License-Identifier: MIT
// kiln-shroudforge: whether ShroudForge's own app (<game>/shroudforge/shroudforge.exe) may open its Modloader and World
// Editor windows for the runtime Kiln runs. No Windows code, so the host test (tests/test-helper-version.sh) builds it on
// Linux.
//
// The app checks ShroudForge's settings against the settings schema built into it, and a schema refuses what it does not
// know: an app older than the runtime refuses the newer settings that runtime writes ("Additional properties are not
// allowed ('uiTargets' was unexpected)", a player's 1.7.9 app beside the bundled 1.8.1 runtime, 0.2.0-dev.30.5), and its
// windows fail. The app's version is the one its install wrote into <game>/shroudforge/version.json (upstream's release
// manifest, dev/build/package_release.ps1, which ships shroudforge.exe in the same archive; the exe carries an icon
// resource only, no version resource). It is read, never run.
#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ksf {

// A version as ShroudForge's version.json has it: "1.8.1", "1.8.1-dev.44571709069" (an optional leading "v" and any
// "+build" part are ignored). `ok` is false for text that is no version: anything but ASCII letters, digits, '.', '-' and
// '+' (after white space at either end is cut), or longer than 64 characters. `text` is the version as written, white
// space cut: safe for a log line, since only those characters pass.
struct SfVersion {
    std::vector<unsigned long long> numbers;
    std::vector<std::string> pre;  // the pre-release's dot-separated identifiers; empty for a release
    std::string text;
    bool ok = false;
};

inline SfVersion parse_sf_version(std::string_view text) {
    SfVersion v;
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    if (text.size() > 64) return {};
    for (const char c : text) {
        const bool allowed = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' ||
                             c == '-' || c == '+';
        if (!allowed) return {};
    }
    v.text = std::string(text);
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.remove_prefix(1);
    if (const auto plus = text.find('+'); plus != std::string_view::npos) text = text.substr(0, plus);
    std::string_view core = text, pre;
    if (const auto dash = text.find('-'); dash != std::string_view::npos) {
        core = text.substr(0, dash);
        pre = text.substr(dash + 1);
        if (pre.empty()) return {};
    }
    if (core.empty()) return {};
    for (std::size_t at = 0; at <= core.size();) {
        const auto dot = std::min(core.find('.', at), core.size());
        const auto part = core.substr(at, dot - at);
        if (part.empty() || part.size() > 18) return {};
        unsigned long long n = 0;
        for (const char c : part) {
            if (c < '0' || c > '9') return {};
            n = n * 10 + static_cast<unsigned long long>(c - '0');
        }
        v.numbers.push_back(n);
        at = dot + 1;
    }
    for (std::size_t at = 0; !pre.empty() && at <= pre.size();) {
        const auto dot = std::min(pre.find('.', at), pre.size());
        if (dot == at) return {};
        v.pre.emplace_back(pre.substr(at, dot - at));
        at = dot + 1;
    }
    v.ok = true;
    return v;
}

// <0 when a is older than b, 0 when they are the same version, >0 when a is newer. Semantic-versioning order: the numbers
// first (a missing number is 0, so 1.8 is 1.8.0), then a pre-release is older than its release; pre-release identifiers
// compare as numbers when both are digits, a number before a word, words as text, and more identifiers are newer.
inline int compare_sf_versions(const SfVersion& a, const SfVersion& b) {
    for (std::size_t i = 0; i < std::max(a.numbers.size(), b.numbers.size()); ++i) {
        const auto x = i < a.numbers.size() ? a.numbers[i] : 0, y = i < b.numbers.size() ? b.numbers[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    if (a.pre.empty() != b.pre.empty()) return a.pre.empty() ? 1 : -1;
    const auto digits = [](const std::string& s) {
        return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
    };
    for (std::size_t i = 0; i < std::min(a.pre.size(), b.pre.size()); ++i) {
        const auto &x = a.pre[i], &y = b.pre[i];
        if (x == y) continue;
        const bool xd = digits(x), yd = digits(y);
        if (xd && yd) {
            if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;  // no leading zeros in a version's numbers
            return x < y ? -1 : 1;
        }
        if (xd != yd) return xd ? -1 : 1;
        return x < y ? -1 : 1;
    }
    if (a.pre.size() != b.pre.size()) return a.pre.size() < b.pre.size() ? -1 : 1;
    return 0;
}

// The line kiln.log gets instead of ShroudForge's windows, when its app (version `app`, from its version.json) is older
// than the runtime Kiln runs (version `runtime`) or its version cannot be read; empty: start the windows as before (the
// same or a newer app, or a runtime whose own version is no version, which Kiln never selects).
inline std::string helper_windows_refusal(std::string_view app, std::string_view runtime) {
    const SfVersion r = parse_sf_version(runtime);
    if (!r.ok) return {};
    const SfVersion a = parse_sf_version(app);
    const std::string tail = " than the ShroudForge Kiln runs (" + r.text +
                             "): update ShroudForge to use its Modloader and World Editor windows. Mods still load.";
    if (!a.ok) return "ShroudForge's own app (version unknown) may be older" + tail;
    if (compare_sf_versions(a, r) < 0) return "ShroudForge's own app (version " + a.text + ") is older" + tail;
    return {};
}

}  // namespace ksf
