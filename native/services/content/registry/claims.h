// SPDX-License-Identifier: MIT
// kiln.content registry stage: indexed claims (registry P1 slice S4; P1-PLAN.md section 5.3).
//
// "Who else touches this place of this resource": the question a boot resource patch's prepare asks for every change
// (gamedata_bootpatch.cpp: two mods on one field, on one list, a path through a list another mod clears...). Today it scans
// every earlier claim in full, so a generation's cost grows with the square of its changes. ClaimIndex keeps the claims of
// one resource in a trie of normalised path segments (a member by its lower-case name, an index by its number, "*" as a
// child of its own), so each question walks one path: O(path length), plus the answers.
//
// The answers are the earliest claim (lowest seq, the order claims were added) that matches: a loop over a vector in
// insertion order returned that one first, so a refusal built from it names the same mod and path as before.
// Plain data: no game memory, no lock.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace kc::registry {

struct Seg {
    std::string s;  // a member name (lower-case), an index ("3"), or "*"
    bool index = false;
};

// A path as written ("a.B[03].c[*]") into normalised segments (the rules of gamedata_bootpatch.cpp's segments() and
// normalised(): names lower-case, indices as plain numbers, "*" kept). false + *err: not a path.
// tmpl: a keen::ecs::TemplateResource path, whose first segment names a component by its type: "Health#0" is "health" (the
// first component of that type), "Health#01" is "health#1" (normalised()'s fold). At most kMaxSegs segments.
constexpr size_t kMaxSegs = 64;
bool parse_path(const std::string& path, std::vector<Seg>* out, std::string* err, bool tmpl = false);

enum class ClaimKind : uint8_t { Set, Append, Clear, Replace, Starred, Registry };

struct Claim {
    uint64_t seq = 0;  // set by add(): the order claims were made
    std::string mod;
    ClaimKind kind = ClaimKind::Set;
    std::string path;   // as written, for messages
    std::string value;  // the value or values as text (two equal clears or replaces are not a conflict)
};

class ClaimIndex {
public:
    using Pred = std::function<bool(const Claim&)>;

    // A claim at "place" of resource "res". Returns its seq; 0 (not kept) for an empty place or one of more than kMaxSegs.
    uint64_t add(const std::string& res, const std::vector<Seg>& place, Claim c);

    // The earliest claim at the same place as "place" (an index "*" on either side matches any index) that pred accepts.
    const Claim* same_place(const std::string& res, const std::vector<Seg>& place, const Pred& pred) const;
    // The earliest claim at a list L that "path" goes through: path is longer than L, path[|L|] is an index and path's
    // first |L| segments are the same place as L (today's through(L, path)).
    const Claim* list_through(const std::string& res, const std::vector<Seg>& path, const Pred& pred) const;
    // The earliest claim at a place P that goes through the list at "list" (today's through(list, P)).
    const Claim* below_list(const std::string& res, const std::vector<Seg>& list, const Pred& pred) const;
    // The earliest claim on the same members as "place" whose indices are the same, or either of which has a "*" anywhere
    // (today's fields check: overlap() over base_of / index_of, gamedata_bootpatch.cpp).
    const Claim* overlap(const std::string& res, const std::vector<Seg>& place, const Pred& pred) const;

    size_t size() const { return count_; }

private:
    struct Node {
        std::map<std::string, std::unique_ptr<Node>> kids;  // ".name", "[3", "[*"
        std::vector<Claim> claims;                          // made exactly here, in seq order
    };
    static std::string key(const Seg& s) { return (s.index ? "[" : ".") + s.s; }
    // the nodes whose place is the same place as segs[0..n)
    void match(const Node* at, const std::vector<Seg>& segs, size_t i, size_t n, std::vector<const Node*>* out) const;
    static const Claim* first(const Node* n, const Pred& pred, const Claim* best);
    static const Claim* subtree(const Node* n, const Pred& pred, const Claim* best);

    std::unordered_map<std::string, Node> roots_;
    // today's fields check keys a claim by its members without indices: "<res>|<members>" -> (its indices, the claim), in seq order
    std::unordered_map<std::string, std::vector<std::pair<std::string, Claim>>> by_members_;
    uint64_t next_ = 1;
    size_t count_ = 0;
};

}  // namespace kc::registry
