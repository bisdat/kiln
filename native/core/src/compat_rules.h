// SPDX-License-Identifier: MIT
// Compatibility rules (schema/compat-rules.schema.json, docs/kiln/engines/README.md "Closed-source DLL mods"): parsing,
// merging the shipped and the player's file, glob matching and evaluation. Pure logic over a few Win32 string calls: the
// test native/core/tests/compat_test.cpp runs it under Wine against schema/fixtures/compat-rules/cases.json, the same
// file the launcher's Rust evaluator is held to.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kiln::compat {

struct Matcher {
    enum class Kind { Module, Engine, Mod, Symbol };
    Kind kind = Kind::Module;
    std::string value;
};
const char* matcher_key(Matcher::Kind kind);       // "module" | "engine" | "mod" | "symbol"
std::string matcher_text(const Matcher& m);        // `module xhl*.dll`, for logs

struct Rule {
    std::string id;
    std::vector<Matcher> when;     // empty: the rule always fires
    std::string action;            // warn | late | disable | before | defer | original-code
    bool has_subject = false;      // required for every action except warn
    Matcher subject;               // defer also sees exact package-relative companion paths
    // before (dev.29): the DLL that loads after the subject. The launcher orders the two in chain.ini
    // (the target late, the subject early); the core reports whether the order held. A core before dev.29 drops the rule
    // ("unknown key \"target\"") and keeps the others.
    bool has_target = false;
    Matcher target;             // defer: shroudtopia:<id> enabled in this profile
    uint32_t max_wait_ms = 0;      // defer only: 1-30000 ms after Shroudtopia starts
    std::string message;
    std::string source;            // shipped | user
};

struct ParseResult {
    bool ok = false;                       // false: the whole file is ignored (`error` says why)
    std::string error;
    std::vector<Rule> rules;               // the rules that validated, in file order
    int dropped = 0;                       // rules that did not validate (their reasons are in `problems`)
    std::vector<std::string> problems;
};
// One file's text (UTF-8, a BOM is skipped). Plain JSON; `source` is "shipped" or "user".
ParseResult parse_rules(std::string_view text, const char* source);

// The player's rules over the shipped ones: a rule with an id already there replaces it in place, other rules are
// appended. `replaced` (optional) gets the ids that were replaced.
std::vector<Rule> merge_rules(std::vector<Rule> shipped, const std::vector<Rule>& user, std::vector<std::string>* replaced);

// A module matcher against a DLL file name: case-insensitive (Unicode, as Windows file names), `*` matches any run of
// characters (also none), `?` exactly one, everything else matches itself. The whole name must match.
bool glob_match(std::string_view pattern, std::string_view name);

// What is present in one start.
struct Facts {
    std::vector<std::string> modules;  // DLL file names (chain entries that are on, modules loaded in the process)
    std::vector<std::string> engines;  // engine words
    std::vector<std::string> mods;     // "<engine>:<id>" of the enabled mods of the profile (kiln:<id> = native:<id>)
    std::vector<std::string> symbols;  // sigdb symbols with an overlap
};
bool present(const Matcher& m, const Facts& facts);
// The rules whose every `when` matcher is present (all of them when `when` is empty), in rule order.
std::vector<const Rule*> evaluate(const std::vector<Rule>& rules, const Facts& facts);

// First matching original-code rule for this Shroudtopia mod, after the start conditions.
const Rule* original_code_rule(const std::vector<Rule>& rules, const Facts& start, std::string_view mod);

// First matching native wait rule; target must also be enabled in this start.
const Rule* native_defer_rule(const std::vector<Rule>& rules, const Facts& start, const Facts& native, bool host_available);

}  // namespace kiln::compat
