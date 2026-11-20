// SPDX-License-Identifier: MIT
// kiln.content registry stage: indexed claims (claims.h).
#include "claims.h"

#include <algorithm>

namespace kc::reg {

bool parse_path(const std::string& path, std::vector<Seg>* out, std::string* err, bool tmpl) {
    out->clear();
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) {
            for (char& ch : cur)
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            out->push_back({cur, false});
        }
        cur.clear();
    };
    for (size_t i = 0; i < path.size(); ++i) {
        const char ch = path[i];
        if (out->size() > kMaxSegs) return out->clear(), *err = "a path of more than " + std::to_string(kMaxSegs) + " segments", false;
        if (ch == '.') {
            if (cur.empty() && (out->empty() || !out->back().index)) return *err = "an empty member name in \"" + path + "\"", false;
            flush();
        } else if (ch == '[') {
            flush();
            const size_t end = path.find(']', i);
            if (end == std::string::npos) return *err = "\"[\" without \"]\" in \"" + path + "\"", false;
            const std::string in = path.substr(i + 1, end - i - 1);
            if (in != "*" && (in.empty() || in.size() > 9 || !std::all_of(in.begin(), in.end(), [](char d) { return d >= '0' && d <= '9'; })))
                return *err = "\"[" + in + "]\" in \"" + path + "\": an index is a number or *", false;
            out->push_back({in == "*" ? in : std::to_string(std::stoul(in)), true});
            i = end;
        } else {
            cur += ch;
        }
    }
    flush();
    if (out->empty()) return *err = "an empty path", false;
    if (out->size() > kMaxSegs) return out->clear(), *err = "a path of more than " + std::to_string(kMaxSegs) + " segments", false;
    // a template's first segment names a component by its type: "#k" is its k-th one of that type, "#0" the first (as none)
    if (tmpl && !(*out)[0].index) {
        std::string& f = (*out)[0].s;
        const size_t h = f.find('#');
        const std::string k = h == std::string::npos ? "" : f.substr(h + 1);
        if (!k.empty() && k.size() <= 9 && std::all_of(k.begin(), k.end(), [](char d) { return d >= '0' && d <= '9'; })) {
            const unsigned long nth = std::stoul(k);
            f = f.substr(0, h) + (nth ? "#" + std::to_string(nth) : "");
        }
    }
    return true;
}

namespace {
// today's fields key: the members without the indices, and the indices in order (gamedata_bootpatch.cpp base_of, index_of)
void split(const std::vector<Seg>& place, std::string* members, std::string* indices) {
    bool any = false;  // text_of joins a member with "." after anything, an index included
    for (const Seg& s : place) {
        if (s.index) *indices += "[" + s.s + "]";
        else *members += (any ? "." : "") + s.s;
        any = true;
    }
}
}  // namespace

uint64_t ClaimIndex::add(const std::string& res, const std::vector<Seg>& place, Claim c) {
    if (place.empty() || place.size() > kMaxSegs) return 0;  // parse_path never gives one; nothing deeper is kept
    std::string members, indices;
    split(place, &members, &indices);
    Node* n = &roots_[res];
    for (const Seg& s : place) {
        auto& k = n->kids[key(s)];
        if (!k) k = std::make_unique<Node>();
        n = k.get();
    }
    c.seq = next_++;
    by_members_[res + "|" + members].push_back({indices, c});
    n->claims.push_back(std::move(c));
    ++count_;
    return n->claims.back().seq;
}

void ClaimIndex::match(const Node* at, const std::vector<Seg>& segs, size_t i, size_t n, std::vector<const Node*>* out) const {
    if (i == n) {
        out->push_back(at);
        return;
    }
    const Seg& s = segs[i];
    if (!s.index) {
        const auto it = at->kids.find(key(s));
        if (it != at->kids.end()) match(it->second.get(), segs, i + 1, n, out);
        return;
    }
    if (s.s == "*") {  // any index here, a "*" claim included
        for (auto it = at->kids.lower_bound("["); it != at->kids.end() && it->first[0] == '['; ++it) match(it->second.get(), segs, i + 1, n, out);
        return;
    }
    for (const std::string& k : {"[" + s.s, std::string("[*")}) {
        const auto it = at->kids.find(k);
        if (it != at->kids.end()) match(it->second.get(), segs, i + 1, n, out);
    }
}

const Claim* ClaimIndex::first(const Node* n, const Pred& pred, const Claim* best) {
    for (const Claim& c : n->claims) {
        if (best && c.seq >= best->seq) break;  // in seq order: nothing later here can win
        if (pred(c)) return &c;
    }
    return best;
}

const Claim* ClaimIndex::subtree(const Node* n, const Pred& pred, const Claim* best) {
    best = first(n, pred, best);
    for (const auto& [k, kid] : n->kids) best = subtree(kid.get(), pred, best);
    return best;
}

const Claim* ClaimIndex::same_place(const std::string& res, const std::vector<Seg>& place, const Pred& pred) const {
    if (place.size() > kMaxSegs) return nullptr;
    const auto r = roots_.find(res);
    if (r == roots_.end()) return nullptr;
    std::vector<const Node*> nodes;
    match(&r->second, place, 0, place.size(), &nodes);
    const Claim* best = nullptr;
    for (const Node* n : nodes) best = first(n, pred, best);
    return best;
}

const Claim* ClaimIndex::list_through(const std::string& res, const std::vector<Seg>& path, const Pred& pred) const {
    if (path.size() > kMaxSegs) return nullptr;
    const auto r = roots_.find(res);
    if (r == roots_.end()) return nullptr;
    const Claim* best = nullptr;
    for (size_t k = 1; k < path.size(); ++k) {
        if (!path[k].index) continue;
        std::vector<const Node*> nodes;
        match(&r->second, path, 0, k, &nodes);
        for (const Node* n : nodes) best = first(n, pred, best);
    }
    return best;
}

const Claim* ClaimIndex::below_list(const std::string& res, const std::vector<Seg>& list, const Pred& pred) const {
    if (list.size() > kMaxSegs) return nullptr;
    const auto r = roots_.find(res);
    if (r == roots_.end()) return nullptr;
    std::vector<const Node*> nodes;
    match(&r->second, list, 0, list.size(), &nodes);
    const Claim* best = nullptr;
    for (const Node* n : nodes)
        for (auto it = n->kids.lower_bound("["); it != n->kids.end() && it->first[0] == '['; ++it) best = subtree(it->second.get(), pred, best);
    return best;
}

const Claim* ClaimIndex::overlap(const std::string& res, const std::vector<Seg>& place, const Pred& pred) const {
    if (place.empty() || place.size() > kMaxSegs) return nullptr;
    std::string members, indices;
    split(place, &members, &indices);
    const auto it = by_members_.find(res + "|" + members);
    if (it == by_members_.end()) return nullptr;
    const bool star = indices.find('*') != std::string::npos;
    for (const auto& [ix, c] : it->second)  // in seq order: the first match is the earliest
        if ((star || ix == indices || ix.find('*') != std::string::npos) && pred(c)) return &c;
    return nullptr;
}

}  // namespace kc::reg
