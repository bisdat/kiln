// SPDX-License-Identifier: MIT
// kiln.content gamedata: resource patches of a boot generation (chapter 08, S10; kiln.boot collector "patches").
//
// A mod's OnBoot (or its blocks files' "resourcePatches" section) sets fields of a game resource the game keeps what it
// read of at its start (keen::BalancingTable: a write from OnLoad changes what kiln.assets serves, not what the game's
// BalancingRegistry already holds). One document:
//   {"resource": "<GUID>", "type": "keen::BalancingTable", "part": 0, "set": {"<path>": <value>, ...}}
// and, made before the field writes (gamedata_bootstruct.cpp), in this order: "removeComponents" / "addComponents" (a
// template's components, by type name; the template's "set" paths start with a component's name), "copyComponents"
// ({"<component type>": "<template GUID>"}: a component copied whole from another template), "clear" (["<list path>"]: a
// list made empty), "replace" ({"<list path>": {"values": [...]}}: a list of scalars that holds exactly these) and
// "append" ({"<list path>": {"values": [...], "aboveLast": true}}: scalars added to the end of a list).
// path: members by name, "[i]" an element of an existing array, "[*]" every element (gamedata_walk.cpp's forms).
// value: a plain value (set), or {"mul","add","min","max","raiseOnly","lowerOnly"} on the game's own value (parse_leaf);
// bool and enum members take a plain value only.
//
// Two steps, as kiln.knowledge's collector:
//   prepare (kiln.boot's commit, before the game's entry point): the documents are read and checked against the exe's
//     reflection as far as types go (the type exists, the path names members, it ends in a scalar, the value's shape
//     fits; a component is one of the game's; an append's path is a list of scalars); mods that would change one thing
//     two ways are refused, the later one (ruling 7): one field set to different values, one component added (or copied)
//     and removed, one component copied from two templates, one list appended to twice, a component removed that another
//     mod's path starts at, a list appended to and set with [*] by another mod, a list cleared or replaced and changed
//     any other way by another mod (the same clear, or the same values, from both is fine), a "components[i]" path on a
//     template whose components change. The plan is frozen.
//   apply (kiln.assets when_resources_ready: the game's resource service is up and nothing has read through it): each
//     resource is copied, every document's structural edits are made in the copy (removes, adds, appends), then every
//     set, with the game's values as the originals, and the copy is served as an override. A document is all or
//     nothing: when one of its edits fails, the copy is made again from the game's bytes without that document, so the
//     resource is what the other documents make it. What only the bytes can say (the resource is not there, an index
//     past the array, a component the template has not got) is a fault after the publication, once per document:
//     kiln.boot activation_fault (recorded; a required mod stops the start).
// Nothing here waits, and no lock is held across a call into kiln.boot or kiln.assets.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "gamedata.h"
#include "kiln_blocks_json.hpp"
#include "kiln_boot_collector.hpp"
#include "refs.h"
#include "services/kiln_assets.h"
#include "services/kiln_caps.h"

namespace kc {
namespace {

constexpr char kRow[] = "mods.boot.resource_patches";
constexpr char kKind[] = "resourcePatches";
constexpr size_t kMaxSets = 4096;        // changes of one generation: field writes, list appends, components added or removed
constexpr size_t kMaxResource = 64u << 20;  // bytes of one patched resource

struct Set {
    std::string path;
    nlohmann::json value;
};
struct StructOp {
    char op = 0;                      // '-' removeComponents, '+' addComponents, 'c' copyComponents, 'x' clear, 'r' replace, 'a' append
    std::vector<std::string> names;   // components
    std::vector<KilnAssetKey> sources;  // copyComponents: the template each of `names` is copied from
    std::string path;                 // clear, replace, append
    std::vector<gjson> values;
    bool above_last = false;
};
constexpr char kPhases[] = {'-', '+', 'c', 'x', 'r', 'a'};  // the order of the structural edits on one resource
// One document of one mod: applied whole or not at all.
struct DocPatch {
    std::string mod, key;
    std::vector<StructOp> ops;  // its order; applied by kPhases (every document's), before the sets
    std::vector<Set> sets;      // its order
};
struct ResPatch {
    KilnAssetKey res{};
    std::string type, guid;
    bool tmpl = false;              // keen::ecs::TemplateResource: "set" paths start with a component's name
    std::vector<DocPatch> docs;     // envelope order
};
struct BootPlan {
    std::vector<ResPatch> resources;
    size_t sets = 0;  // changes, against kMaxSets
};

// Process-lifetime latch: readiness is one-shot; a later write would miss the game's first read.
std::atomic<bool> g_ready_before_publication{false};
constexpr char kMissedDeadline[] =
    "the game's resource service was already up before the boot generation was published; "
    "the patch cannot be written before the game reads its resources in this start";

bool g_bound = false;       // the collector is registered (a boot profile)
bool g_ready_asked = false; // when_resources_ready is registered
const char* g_off = nullptr;  // why nothing is applied in this process (then staged patches refuse their mod)

const KilnAssetsApi* assets() {
    const auto* a = static_cast<const KilnAssetsApi*>(g_api->service(KilnStr{KILN_ASSETS_SERVICE, sizeof(KILN_ASSETS_SERVICE) - 1}, 1));
    return a && a->header.size >= offsetof(KilnAssetsApi, when_resources_ready) + sizeof(void*) && a->when_resources_ready && a->resource_data &&
                   a->resource_put
               ? a
               : nullptr;
}

void row(uint32_t state, uint32_t reason, const std::string& text) {
    const auto* caps = static_cast<const KilnCapsApi*>(g_api->service(KilnStr{KILN_CAPS_SERVICE, sizeof(KILN_CAPS_SERVICE) - 1}, 1));
    if (caps && caps->report) caps->report(g_owner, KilnStr{kRow, sizeof(kRow) - 1}, state, reason, KilnStr{text.data(), text.size()});
}

bool is_op(const nlohmann::json& v) { return v.is_object(); }

// One path segment: a member name, or an index ("3", "*").
struct Seg {
    std::string s;
    bool index = false;
};
bool segments(const std::string& path, std::vector<Seg>* out, std::string* err) {
    out->clear();
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) out->push_back({cur, false});
        cur.clear();
    };
    for (size_t i = 0; i < path.size(); ++i) {
        const char c = path[i];
        if (c == '.') {
            if (cur.empty() && (out->empty() || !out->back().index)) return *err = "an empty member name in \"" + path + "\"", false;
            flush();
        } else if (c == '[') {
            flush();
            const size_t end = path.find(']', i);
            if (end == std::string::npos) return *err = "\"[\" without \"]\" in \"" + path + "\"", false;
            const std::string in = path.substr(i + 1, end - i - 1);
            if (in != "*" && (in.empty() || in.size() > 9 || !std::all_of(in.begin(), in.end(), [](char d) { return d >= '0' && d <= '9'; })))
                return *err = "\"[" + in + "]\" in \"" + path + "\": an index is a number or *", false;
            out->push_back({in, true});
            i = end;
        } else {
            cur += c;
        }
    }
    flush();
    if (out->empty()) return *err = "an empty path", false;
    return true;
}

std::string folded(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// The path's segments as one text for the conflict keys: member names lower-case (the game's names match ignoring
// case), indices as plain numbers, joined again, so "A.b[03]." and "a.B[3]" are one path. On a template, "#0" of the first
// segment is the first component of its type, as no "#" is.
std::vector<Seg> normalised(std::vector<Seg> segs, bool tmpl) {
    for (Seg& s : segs) {
        if (!s.index) s.s = folded(s.s);
        else if (s.s != "*") s.s = std::to_string(std::stoul(s.s));  // nine digits at most (segments)
    }
    if (tmpl && !segs.empty() && !segs[0].index) {
        std::string& f = segs[0].s;
        const size_t h = f.find('#');
        const std::string k = h == std::string::npos ? "" : f.substr(h + 1);
        if (!k.empty() && k.size() <= 9 && std::all_of(k.begin(), k.end(), [](char d) { return d >= '0' && d <= '9'; })) {
            const unsigned long nth = std::stoul(k);
            f = f.substr(0, h) + (nth ? "#" + std::to_string(nth) : "");
        }
    }
    return segs;
}
std::string text_of(const std::vector<Seg>& segs) {
    std::string t;
    for (const Seg& s : segs) t += s.index ? "[" + s.s + "]" : (t.empty() ? "" : ".") + s.s;
    return t;
}
// One place of a resource: the same members, and at each index the same number or a "*" on either side.
bool same_place(const std::vector<Seg>& a, const std::vector<Seg>& b, size_t n = SIZE_MAX) {
    if (n == SIZE_MAX) {
        if (a.size() != b.size()) return false;
        n = a.size();
    }
    if (a.size() < n || b.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (a[i].index != b[i].index || (a[i].s != b[i].s && !(a[i].index && (a[i].s == "*" || b[i].s == "*")))) return false;
    return true;
}
// A path that goes through an element of the list at `list` ("list[2].x", "list[*]"): it names what a clear or a
// replace of that list takes away or puts elsewhere.
bool through(const std::vector<Seg>& list, const std::vector<Seg>& path) {
    return path.size() > list.size() && path[list.size()].index && same_place(list, path, list.size());
}
const char* verb_of(const std::string& what) {
    return what == "append" ? "appends to" : what == "clear" ? "clears" : what == "replace" ? "replaces" : "sets";
}

// On a template, what a path's first segment names: a member of the template itself ("components", "name"), or a component by
// its type name ("Gravity", "Gravity#1" the second of that type). Without the game's types, "components" and "name" are its members.
struct Head {
    bool member = false;            // the template's own member
    bool components_index = false;  // "components[i]...": a component by its place in the list
    std::string component;          // the component's type name as written, without "#k"; empty for a member
    bool bad_nth = false;           // "#k" is not a number
};
Head head_of(const gd::TType* tmpl, const std::vector<Seg>& segs) {
    Head h;
    if (segs.empty() || segs[0].index) return h.member = true, h;
    const std::string& first = segs[0].s;
    const gd::TField* f = tmpl ? tmpl->resolved()->field(first) : nullptr;
    h.member = f || (!tmpl && (folded(first) == "components" || folded(first) == "name"));
    if (h.member) {
        h.components_index = folded(f ? f->name : first) == "components" && segs.size() > 1 && segs[1].index;
        return h;
    }
    const size_t at = first.find('#');
    h.component = first.substr(0, at);
    if (at != std::string::npos) {
        const std::string k = first.substr(at + 1);
        h.bad_nth = k.empty() || k.size() > 9 || !std::all_of(k.begin(), k.end(), [](char d) { return d >= '0' && d <= '9'; });
    }
    return h;
}

// Where a path's type check starts: the resource's type, or on a template the component its first segment names (and the
// path below it). false + *err: the component is no type of the game's, or the path stops at it.
bool type_root(const gd::TType* type, bool tmpl, const std::vector<Seg>& segs, const gd::TType** root, std::vector<Seg>* rest, std::string* err) {
    *root = type;
    *rest = segs;
    if (!tmpl) return true;
    const Head h = head_of(type, segs);
    if (h.member) return true;
    if (h.bad_nth) return *err = "\"" + segs[0].s + "\": a component's \"#k\" is a number (the k-th of that type, from 0)", false;
    if (!(*root = gd::component_type(h.component, err))) return false;
    rest->erase(rest->begin());
    if (rest->empty()) return *err = "a path on a template names a member of a component (\"" + h.component + ".<member>\")", false;
    return true;
}

// The path against the types alone. true: it ends in `leaf` (checked), or *unknown (a variant or optional on the way:
// only the bytes can say). false + *err: no such member, not a list, not a scalar. list: it has to end in a list of scalars
// instead (an append or a replace), and `leaf` is the list's element; any_list: a list of anything (a clear).
bool check_types(const gd::TType* root, const std::vector<Seg>& segs, Field* leaf, bool* unknown, std::string* err, bool list = false,
                 bool any_list = false) {
    *unknown = false;
    const gd::TType* t = root->resolved();
    std::string at = root->brief;
    for (const Seg& s : segs) {
        if (!t) return *unknown = true, true;
        if (s.index) {
            if (t->kind != gd::K_ARRAY && t->kind != gd::K_BLOBARRAY) return *err = at + " is not a list", false;
            t = t->inner() ? t->inner()->resolved() : nullptr;
            at += "[" + s.s + "]";
            continue;
        }
        if (t->kind != gd::K_STRUCT) return *unknown = true, true;  // a variant, an optional, a config list: the walker's business
        const gd::TField* f = t->field(s.s);
        if (!f) return *err = at + " has no member " + s.s, false;
        t = f->type() ? f->type()->resolved() : nullptr;
        at += "." + f->name;
    }
    if (!t) return *unknown = true, true;
    if (t->kind == gd::K_BLOBOPT || t->kind == gd::K_BLOBVAR) return *unknown = true, true;
    if (any_list) {
        if (t->kind != gd::K_BLOBARRAY) return *err = "NotAList: " + at + " is " + t->name + ", not a list (clear empties a list)", false;
        return true;
    }
    if (list) {
        if (t->kind != gd::K_BLOBARRAY)
            return *err = "NotAList: " + at + " is " + t->name + ", not a list (append and replace take a list of numbers, bools or enum members)", false;
        const gd::TType* el = t->inner() ? t->inner()->resolved() : nullptr;
        if (!el) return *unknown = true, true;
        if (!gd::leaf_of(el, leaf) || leaf->ref || leaf->vt == KILN_CV_GUID || !leaf->bytes || leaf->bytes > 8)
            return *err = "NotScalar: " + at + " is a list of " + el->name + ", not of numbers, bools or enum members", false;
        return true;
    }
    if (!gd::leaf_of(t, leaf) || leaf->ref || leaf->vt == KILN_CV_GUID)
        return *err = "NotScalar: " + at + " is " + t->name + ", not a number, a bool or an enum (resource patches set scalar members)", false;
    return true;
}

// The value's shape for a leaf: an operation object only on numbers.
bool check_value(const Field& f, const nlohmann::json& v, std::string* err) {
    if (is_op(v)) {
        if (f.vt == KILN_CV_BOOL || f.enum_kind != KILN_CONTENT_PLAIN)
            return *err = "OperationOnNonNumber: a bool or enum member takes a plain value, not {mul, add, min, max}", false;
        static const char* const known[] = {"mul", "add", "min", "max", "raiseOnly", "lowerOnly", "set"};
        for (const auto& [k, x] : v.items()) {
            if (std::none_of(std::begin(known), std::end(known), [&](const char* n) { return k == n; }))
                return *err = "unknown operation \"" + k + "\" (mul, add, min, max, raiseOnly, lowerOnly, set)", false;
            if ((k == "raiseOnly" || k == "lowerOnly") ? !x.is_boolean() : !x.is_number())
                return *err = "\"" + k + "\" has the wrong kind of value", false;
        }
        if (v.empty()) return *err = "an empty operation object", false;
        if (v.contains("min") && v.contains("max") && v["min"].get<double>() > v["max"].get<double>())
            return *err = "min must not be greater than max", false;  // parse_leaf uses std::clamp: inverted bounds are undefined
        return true;
    }
    if (v.is_array() && f.enum_kind == KILN_CONTENT_FLAGS) {
        for (const auto& flag : v) {
            if (flag.is_string()) continue;
            // parse_leaf turns a numeric flag index into 1ll << index. Reject invalid shifts before it runs.
            if (!flag.is_number_integer() || flag < 0 || flag > 63)
                return *err = "a flag is a name or an integer index from 0 to 63", false;
        }
        return true;
    }
    if (v.is_boolean() || v.is_number() || v.is_string()) return true;
    return *err = "unsupported value " + v.dump(), false;
}

std::string base_of(const std::string& path) {  // the path without its indices
    std::string b;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '[') {
            const size_t e = path.find(']', i);
            if (e == std::string::npos) break;
            i = e;
        } else {
            b += path[i];
        }
    }
    return b;
}
std::string index_of(const std::string& path) {  // its indices, in order
    std::string x;
    for (size_t i = 0; i < path.size(); ++i)
        if (path[i] == '[') {
            const size_t e = path.find(']', i);
            if (e == std::string::npos) break;
            x += path.substr(i, e - i + 1);
            i = e;
        }
    return x;
}
bool overlap(const std::string& a, const std::string& b) { return a == b || a.find('*') != std::string::npos || b.find('*') != std::string::npos; }

// The "resourcePatches" sections of the mods' blocks files, as documents, in load order (files by name within a mod). The files are
// read as every reader of blocks files reads them (kiln_blocks_json.hpp: comments, trailing commas). A file that is not such JSON is
// another reader's to report (Kiln.Blocks reads the same files), except for a required participant (kiln_boot_collector.hpp,
// entry_error): a blocks file of its own that cannot be read, or that names "resourcePatches" and is not JSON, refuses it (Refusal,
// turned into a refusal by prepare_boot).
std::vector<kiln::boot::Doc> file_docs(const kiln::boot::Input* boot) {
    namespace fs = std::filesystem;
    std::vector<kiln::boot::Doc> out;
    for (const fs::path& dir : mod_folder_list()) {
        std::error_code ec;
        if (!fs::is_directory(dir / L"blocks", ec)) continue;
        const std::string id = kiln::boot::mod_id_of(dir);
        if (id.empty() || (boot && boot->out(id))) continue;
        std::vector<fs::path> files;
        for (auto it = fs::directory_iterator(dir / L"blocks", ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->path().extension() == L".json") files.push_back(it->path());
        std::sort(files.begin(), files.end());
        for (const fs::path& f : files) {
            const std::u8string u = f.filename().u8string();
            const std::string label = "blocks/" + std::string(u.begin(), u.end());
            std::ifstream in(f, std::ios::binary);
            if (!in) {
                if (boot) kiln::boot::entry_error(*boot, id, kKind, label, "kiln.content: " + label + ": cannot be read");
                continue;
            }
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (text.find("resourcePatches") == std::string::npos) continue;
            const nlohmann::json j = kiln::blocks_json::parse_relaxed(text);
            if (!j.is_object() && boot) kiln::boot::entry_error(*boot, id, kKind, label, "kiln.content: " + label + ": not valid JSON (it names resourcePatches)");
            if (!j.is_object() || !j.contains(kKind)) continue;
            if (!j[kKind].is_object()) {
                out.push_back({id, "", nlohmann::json()});  // refused by its shape, with the mod named
                continue;
            }
            for (const auto& [key, doc] : j[kKind].items()) out.push_back({id, key, doc});
        }
    }
    return out;
}

void KILN_CALL destroy_plan(const void* p) { delete static_cast<const BootPlan*>(p); }

KilnResult KILN_CALL prepare_boot(void*, const KilnBootPrepareInputV1* in, KilnBootPreparedV1* out, KilnBootIssueV1* issue) {
    // The game's entry point is held until publication (boot_service.cpp b_commit / after_clr).
    // Read once for this prepare; apply may also run on the resource-initialization thread.
    const bool missed_deadline = g_ready_before_publication.load();
    const kiln::boot::Input input = kiln::boot::parse(in);
    if (!input.ok) return kiln::boot::refuse(out, issue, "", "", "", "kiln.content: the boot envelope is not JSON");
    auto plan = std::make_unique<BootPlan>();
    const bool types = gd::type_named("keen::ItemInfo") != nullptr;  // the exe's reflection is readable before the entry point
    struct Claim {
        std::string index, mod, value;
    };
    // what the documents so far change, by "<resource key>|...": fields set ("|<normalised path without indices>": its indices,
    // the mod, the value), components added or removed ("|<component, lower-case>": "+" / "-"), paths that start at a
    // component ("|<component>": the path, "set" / "append"), documents that add or remove components ("": which), paths
    // that name a component by its place in the list ("": the path)
    std::map<std::string, std::vector<Claim>> fields, components, uses, structure, by_place;
    struct ListClaim {
        std::string res, mod, path;
        std::vector<Seg> place;  // normalised
        std::string what, value;  // reshaped: "clear" / "replace" and the values (dumped)
    };
    // lists appended to; lists set with [*] (the list's place); lists cleared or replaced; every set's place
    std::vector<ListClaim> appended, starred, reshaped, set_places;
    // each mod's files first, then what its OnBoot staged; a key both have stays the file's
    std::vector<kiln::boot::Doc> docs;
    try {
        docs = file_docs(&input);
    } catch (const kiln::boot::Refusal& r) {
        return kiln::boot::refuse(out, issue, r.mod, r.kind, r.key, r.what());
    }
    {
        std::set<std::string> have;
        for (const auto& d : docs) have.insert(d.owner + "/" + d.key);
        for (auto& d : input.docs("patches")) {
            if (!have.insert(d.owner + "/" + d.key).second) {
                LOGI("boot: resource patch %s/%s: the mod's blocks file has this key too: the file's stays", d.owner.c_str(), d.key.c_str());
                continue;
            }
            docs.push_back(std::move(d));
        }
    }
    std::set<std::string> not_boot;
    for (const auto& d : docs) {
        // a mod that takes no part in the generation (no "boot" in its kiln.mod.json): its patches are not applied
        if (!input.owner.count(d.owner)) {
            if (not_boot.insert(d.owner).second)
                LOGW("mod %s has \"resourcePatches\" in a blocks file and takes no part in the boot generation: they are not applied. Add \"boot\": true "
                     "to its kiln.mod.json",
                     d.owner.c_str());
            continue;
        }
        auto refused = [&](const std::string& path, const std::string& why) {
            return kiln::boot::refuse(out, issue, d.owner, kKind, d.key, (path.empty() ? std::string() : "\"" + path + "\": ") + why);
        };
        if (g_off) return refused("", std::string("resource patches are not applied here: ") + g_off);
        const nlohmann::json& doc = d.document;
        auto nonempty = [&](const char* k) {  // present and not empty (a value of the wrong kind is its own member's error)
            return doc.is_object() && doc.contains(k) && !doc[k].is_null() && !((doc[k].is_object() || doc[k].is_array()) && doc[k].empty());
        };
        const bool structural = nonempty("addComponents") || nonempty("removeComponents") || nonempty("append") || nonempty("copyComponents") ||
                                nonempty("clear") || nonempty("replace");
        if (!doc.is_object() || !doc.contains("resource") || !doc["resource"].is_string() || !doc.contains("type") || !doc["type"].is_string() ||
            (doc.contains("set") && !doc["set"].is_object()) || (!nonempty("set") && !structural))
            return refused("", "a resource patch is {\"resource\": \"<GUID>\", \"type\": \"<type name>\", \"set\": {\"<path>\": <value>}} "
                               "(and/or \"addComponents\", \"removeComponents\", \"copyComponents\", \"clear\", \"replace\", \"append\")");
        if (missed_deadline) {
            row(KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE, std::string("resource patch writes were not applied: ") + kMissedDeadline);
            // kiln.boot drops this optional participant and prepares again, or stops for a required one.
            return refused("", kMissedDeadline);
        }
        for (const auto& [k, x] : doc.items())
            if (k != "format" && k != "resource" && k != "type" && k != "part" && k != "set" && k != "removeComponents" && k != "addComponents" &&
                k != "copyComponents" && k != "clear" && k != "replace" && k != "append")
                return refused("", "unknown member \"" + k +
                                       "\" (resource, type, part, set, removeComponents, addComponents, copyComponents, clear, replace, append)");
        ResPatch rp;
        rp.guid = doc["resource"].get<std::string>();
        rp.type = doc["type"].get<std::string>();
        if (!guid_parse(rp.guid, rp.res.guid)) return refused("", "\"resource\": \"" + rp.guid + "\" is not a GUID");
        if (doc.contains("part") && !doc["part"].is_number_unsigned()) return refused("", "\"part\" is a number from 0");
        rp.res.part = doc.value("part", 0u);
        rp.res.type = fnv1a(rp.type);
        const gd::TType* type = types ? gd::type_named(rp.type) : nullptr;
        if (types && (!type || !type->resolved() || type->resolved()->kind != gd::K_STRUCT))
            return refused("", "UnknownType: this game build has no struct type \"" + rp.type + "\" (the qualified name, as keen::BalancingTable)");
        const std::string res_key = guid_text(rp.res.guid) + "/" + rp.type + "/" + std::to_string(rp.res.part);
        rp.tmpl = rp.type == "keen::ecs::TemplateResource";
        DocPatch dp{d.owner, d.key, {}, {}};
        // every change counts against the limit: a field write, a list's append, a component added or removed
        const std::string too_many =
            "more than " + std::to_string(kMaxSets) + " changes (field writes, lists cleared, replaced or appended to, components) in one boot generation";
        auto count = [&]() { return plan->sets < kMaxSets ? (++plan->sets, true) : false; };
        // A path on a template: a component another mod removes is not one to start at, and "components[i]" names another
        // component once the list changes.
        auto template_path = [&](const std::string& path, const std::vector<Seg>& segs, const char* what, std::string* why) {
            const Head h = head_of(types ? type : nullptr, segs);
            if (h.components_index) {
                if (const auto& st = structure[res_key]; !st.empty())
                    return *why = "\"components[i]\" names a component by its place in the list, which mod " + st.front().mod +
                                  " changes (" + st.front().value + " of " + res_key + "); name the component by its type (\"Gravity.isActive\")",
                           false;
                by_place[res_key].push_back({path, d.owner, what});
                return true;
            }
            if (h.member || h.component.empty()) return true;
            const std::string c = folded(h.component);
            for (const Claim& r : components[res_key + "|" + c])
                if (r.mod != d.owner && r.value == "-")
                    return *why = "Conflict: mod " + r.mod + " removes the component " + h.component + " of " + res_key +
                                  " that this path starts at; the later mod is refused",
                           false;
            uses[res_key + "|" + c].push_back({path, d.owner, what});
            return true;
        };
        // the structural edits: what can be checked here is, the bytes when they are applied
        for (const char* which : {"removeComponents", "addComponents"}) {
            if (!doc.contains(which) || (doc[which].is_array() && doc[which].empty())) continue;
            const bool add = which[0] == 'a';
            const std::string label = std::string("\"") + which + "\"";
            if (!doc[which].is_array()) return refused("", label + " is a list of component type names");
            if (!rp.tmpl) return refused("", label + ": only a template (keen::ecs::TemplateResource) has components");
            StructOp op;
            op.op = add ? '+' : '-';
            for (const auto& n : doc[which]) {
                if (!n.is_string() || n.get<std::string>().empty()) return refused("", label + " is a list of component type names");
                const std::string brief = n.get<std::string>();
                if (!count()) return refused("", too_many);
                std::string why;
                if (types && !gd::component_type(brief, &why)) return refused("", label + ": " + why);
                const std::string c = folded(brief);
                if (!add && c == "currenttransform") return refused("", label + ": CurrentTransform is what any entity needs to live");
                // two mods: one adds what the other removes
                auto& list = components[res_key + "|" + c];
                for (const Claim& cl : list)
                    if (cl.mod != d.owner && (add ? cl.value == "-" : cl.value != "-"))
                        return refused("", "Conflict: mod " + cl.mod + (add ? " removes " : cl.value == "+" ? " adds " : " copies ") + brief + " of " +
                                               res_key + "; two mods on one component are not combined");
                // nor is a component taken away under another mod's path
                if (!add)
                    for (const Claim& u : uses[res_key + "|" + c])
                        if (u.mod != d.owner)
                            return refused("", "Conflict: mod " + u.mod + " " + verb_of(u.value) + " \"" + u.index + "\" of " + res_key +
                                                   ", and this patch removes " + brief + "; the later mod is refused");
                list.push_back({"", d.owner, add ? "+" : "-"});
                op.names.push_back(brief);
            }
            if (const auto& ix = by_place[res_key]; !ix.empty())
                return refused("", label + ": mod " + ix.front().mod + " names a component of " + res_key + " by its place in the list (\"" + ix.front().index +
                                       "\"), which this changes; the later mod is refused");
            structure[res_key].push_back({"", d.owner, which});
            dp.ops.push_back(std::move(op));
        }
        // copyComponents: {"<component type>": "<template GUID>"}, a component copied whole from another template
        if (doc.contains("copyComponents") && !(doc["copyComponents"].is_object() && doc["copyComponents"].empty())) {
            const std::string label = "\"copyComponents\"";
            if (!doc["copyComponents"].is_object()) return refused("", label + " is {\"<component type>\": \"<template GUID>\"}");
            if (!rp.tmpl) return refused("", label + ": only a template (keen::ecs::TemplateResource) has components");
            StructOp op;
            op.op = 'c';
            for (const auto& [brief, from] : doc["copyComponents"].items()) {
                KilnAssetKey src{};
                if (brief.empty() || !from.is_string() || !guid_parse(from.get<std::string>(), src.guid))
                    return refused("", label + ": \"" + brief + "\" takes the GUID of the template it is copied from");
                if (std::memcmp(src.guid, rp.res.guid, sizeof src.guid) == 0) return refused("", label + ": " + brief + " is copied from the template it patches");
                src.type = rp.res.type;
                src.part = 0;
                if (!count()) return refused("", too_many);
                std::string why;
                if (types && !gd::component_type(brief, &why)) return refused("", label + ": " + why);
                const std::string c = folded(brief);
                if (c == "currenttransform") return refused("", label + ": CurrentTransform is what any entity needs to live: it is not copied over");
                const std::string mine = "c:" + guid_text(src.guid);
                auto& list = components[res_key + "|" + c];
                for (const Claim& cl : list)
                    if (cl.mod != d.owner && (cl.value == "-" || (cl.value[0] == 'c' && cl.value != mine)))
                        return refused("", "Conflict: mod " + cl.mod + (cl.value == "-" ? " removes " : " copies from another template ") + brief +
                                               " of " + res_key + "; two mods on one component are not combined");
                list.push_back({"", d.owner, mine});
                op.names.push_back(brief);
                op.sources.push_back(src);
            }
            if (const auto& ix = by_place[res_key]; !ix.empty())
                return refused("", label + ": mod " + ix.front().mod + " names a component of " + res_key + " by its place in the list (\"" +
                                       ix.front().index + "\"), which this changes; the later mod is refused");
            structure[res_key].push_back({"", d.owner, "copyComponents"});
            dp.ops.push_back(std::move(op));
        }
        // the list edits, in their order: clear (["<path>"]), replace ({"<path>": {"values": [...]}}), append ({"<path>":
        // {"values": [...], "aboveLast": bool}}). A path names one list ([i] on the way, not [*]).
        for (const char* which : {"clear", "replace", "append"}) {
            if (!doc.contains(which) || ((doc[which].is_object() || doc[which].is_array()) && doc[which].empty())) continue;
            const std::string w = which;
            const bool clear = w == "clear", append = w == "append";
            if (clear ? !doc[which].is_array() : !doc[which].is_object())
                return refused("", clear ? "\"clear\" is a list of list paths ([\"noBuildZones\"])"
                                         : "\"" + w + "\" is {\"<list path>\": {\"values\": [...]" + (append ? ", \"aboveLast\": true" : "") + "}}");
            std::vector<std::pair<std::string, nlohmann::json>> items;
            if (clear) {
                for (const auto& p : doc[which]) {
                    if (!p.is_string() || p.get<std::string>().empty()) return refused("", "\"clear\" is a list of list paths ([\"noBuildZones\"])");
                    items.emplace_back(p.get<std::string>(), nlohmann::json());
                }
            } else {
                for (const auto& [path, spec] : doc[which].items()) items.emplace_back(path, spec);
            }
            for (const auto& [path, spec] : items) {
                std::string err;
                std::vector<Seg> segs;
                if (!segments(path, &segs, &err)) return refused(path, err);
                if (std::any_of(segs.begin(), segs.end(), [](const Seg& s) { return s.index && s.s == "*"; }))
                    return refused(path, "\"" + w + "\": the path names one list ([i], not [*])");
                if (!clear) {
                    if (!spec.is_object() || !spec.contains("values") || !spec["values"].is_array() || spec["values"].empty() ||
                        (append && spec.contains("aboveLast") && !spec["aboveLast"].is_boolean()))
                        return refused(path, append ? "\"append\": a list path takes {\"values\": [<numbers, bools or enum names>], \"aboveLast\": true|false}"
                                                    : "\"replace\": a list path takes {\"values\": [<numbers, bools or enum names>]} (\"clear\" empties a list)");
                    for (const auto& [k, x] : spec.items())
                        if (k != "values" && !(append && k == "aboveLast"))
                            return refused(path, "\"" + w + "\": unknown member \"" + k + "\" (" + (append ? "values, aboveLast" : "values") + ")");
                    for (const auto& v : spec["values"])
                        if (!(v.is_number() || v.is_boolean() || v.is_string()))
                            return refused(path, "\"" + w + "\": a value is a number, a bool or an enum member's name");
                }
                if (!count()) return refused(path, too_many);
                if (rp.tmpl && !template_path(path, segs, which, &err)) return refused(path, err);
                // the list and its values against the types (a variant or an optional on the way: the bytes say)
                Field el;
                bool unknown = !types;
                const gd::TType* root = nullptr;
                std::vector<Seg> rest;
                if (types && (!type_root(type, rp.tmpl, segs, &root, &rest, &err) || !check_types(root, rest, &el, &unknown, &err, !clear, clear)))
                    return refused(path, "\"" + w + "\": " + err);
                StructOp op;
                op.op = clear ? 'x' : append ? 'a' : 'r';
                op.path = path;
                op.above_last = append && spec.value("aboveLast", false);
                if (!clear)
                    for (const auto& v : spec["values"]) {
                        op.values.push_back(gjson::parse(v.dump()));
                        uint8_t bytes[16] = {}, zero[16] = {};
                        bool reset = false;
                        if (!unknown && !gd::parse_leaf(el, op.values.back(), zero, bytes, &reset, &err)) return refused(path, "\"" + w + "\": " + err);
                    }
                // two mods on one list
                const std::vector<Seg> place = normalised(segs, rp.tmpl);
                const std::string dump = clear ? std::string() : spec["values"].dump();
                for (const ListClaim& r : reshaped) {
                    if (r.res != res_key || r.mod == d.owner) continue;
                    if (same_place(r.place, place) && (append || r.what != w || r.value != dump))
                        return refused(path, "Conflict: mod " + r.mod + " " + verb_of(r.what) + " this list of " + res_key + " (\"" + r.path +
                                                 "\"); two mods on one list are not combined (the same " + r.what + " from both is)");
                    if (through(r.place, place) || (!append && through(place, r.place)))
                        return refused(path, "Conflict: mod " + r.mod + " " + verb_of(r.what) + " \"" + r.path + "\" of " + res_key +
                                                 ", which this path goes through or holds; the later mod is refused");
                }
                if (append) {
                    for (const ListClaim& a : appended)
                        if (a.res == res_key && a.mod != d.owner && same_place(a.place, place))
                            return refused(path, "Conflict: mod " + a.mod + " appends to this list of " + res_key + " too (\"" + a.path +
                                                     "\"); two mods on one list are not combined");
                    for (const ListClaim& s : starred)
                        if (s.res == res_key && s.mod != d.owner && same_place(s.place, place))
                            return refused(path, "Conflict: mod " + s.mod + " sets every element of this list of " + res_key + " (\"" + s.path +
                                                     "\"), which would change what is appended here too; the later mod is refused");
                    appended.push_back({res_key, d.owner, path, place, "", ""});
                } else {
                    for (const auto* claims : {&appended, &set_places})
                        for (const ListClaim& o : *claims)
                            if (o.res == res_key && o.mod != d.owner && (same_place(o.place, place) || through(place, o.place)))
                                return refused(path, "Conflict: mod " + o.mod + (claims == &appended ? " appends to \"" : " sets \"") + o.path + "\" of " +
                                                         res_key + ", and this patch " + verb_of(w) + " that list; the later mod is refused");
                    reshaped.push_back({res_key, d.owner, path, place, w, dump});
                }
                dp.ops.push_back(std::move(op));
            }
        }
        const nlohmann::json no_sets = nlohmann::json::object();
        for (const auto& [path, value] : (doc.contains("set") ? doc["set"] : no_sets).items()) {
            if (!count()) return refused(path, too_many);
            std::string err;
            std::vector<Seg> segs;
            if (!segments(path, &segs, &err)) return refused(path, err);
            if (rp.tmpl && !template_path(path, segs, "set", &err)) return refused(path, err);
            // a template's path starts at a component (by its type) or at a member of the template
            Field leaf;
            bool unknown = !types;
            const gd::TType* root = nullptr;
            std::vector<Seg> rest;
            if (types && (!type_root(type, rp.tmpl, segs, &root, &rest, &err) || !check_types(root, rest, &leaf, &unknown, &err)))
                return refused(path, err);
            if (!unknown && !check_value(leaf, value, &err)) return refused(path, err);
            if (unknown && is_op(value) && value.empty()) return refused(path, "an empty operation object");
            const std::vector<Seg> place = normalised(segs, rp.tmpl);
            const std::string norm = text_of(place);
            // two mods, one field
            auto& list = fields[res_key + "|" + base_of(norm)];
            const std::string ix = index_of(norm), dump = value.dump();
            for (const Claim& c : list)
                if (c.mod != d.owner && overlap(c.index, ix) && (c.value != dump || c.index != ix || is_op(value)))
                    return refused(path, "Conflict: mod " + c.mod + " sets this field of " + res_key + " too (" + c.value +
                                             "); two mods on one field are not combined");
            list.push_back({ix, d.owner, dump});
            // a list another mod clears or replaces: its elements are not the game's any more
            for (const ListClaim& r : reshaped)
                if (r.res == res_key && r.mod != d.owner && through(r.place, place))
                    return refused(path, "Conflict: mod " + r.mod + " " + verb_of(r.what) + " the list \"" + r.path + "\" of " + res_key +
                                             " this path goes through; the later mod is refused");
            set_places.push_back({res_key, d.owner, path, place, "set", ""});
            // [*] over a list another mod appends to would change the appended values too: the later mod is refused
            // ([i] names the same element either way, and is not)
            for (size_t j = 0; j < place.size(); ++j) {
                if (!place[j].index || place[j].s != "*") continue;
                const std::vector<Seg> listp(place.begin(), place.begin() + static_cast<std::ptrdiff_t>(j));
                for (const ListClaim& a : appended)
                    if (a.res == res_key && a.mod != d.owner && same_place(a.place, listp))
                        return refused(path, "Conflict: mod " + a.mod + " appends to this list of " + res_key + " (\"" + a.path +
                                                 "\"), and [*] would change the appended values too; the later mod is refused (name elements by index)");
                starred.push_back({res_key, d.owner, path, listp, "", ""});
            }
            dp.sets.push_back({path, value});
        }
        auto same = std::find_if(plan->resources.begin(), plan->resources.end(), [&](const ResPatch& o) {
            return std::memcmp(&o.res, &rp.res, sizeof rp.res) == 0;
        });
        if (same == plan->resources.end()) {
            rp.docs.push_back(std::move(dp));
            plan->resources.push_back(std::move(rp));
        } else {
            same->docs.push_back(std::move(dp));
        }
    }
    LOGI("boot generation %llu prepared: %zu resource patch change(s) on %zu resource(s)%s", static_cast<unsigned long long>(input.generation),
         plan->sets, plan->resources.size(), types || !plan->sets ? "" : " (types are checked when they are applied)");
    return kiln::boot::prepared(out, plan.release(), &destroy_plan);
}

// The templates copyComponents copies from, as served before any resource patch of this start is written (read once, at
// the start of apply: a patch on a source template does not carry over into the copies). Key: guid_text.
using Sources = std::map<std::string, std::vector<uint8_t>>;

// One build of a resource's copy from the game's bytes, with every document that is not left out: every document's
// removeComponents, then addComponents, copyComponents, clear, replace and append (kPhases; each on the copy as the one
// before left it), then every document's "set", with the game's values as the originals. false: document *bad failed
// (*why says how) and this build is worth nothing. *notes: what each change did, logged for the build that is served.
bool build(const ResPatch& rp, const gd::TType* type, const uint8_t* data, size_t len, const std::vector<char>& left_out, const Sources& sources,
           std::vector<uint8_t>* copy, size_t* here, std::vector<std::string>* notes, size_t* bad, std::string* why) {
    const std::string what = rp.type + " " + guid_text(rp.res.guid);
    copy->assign(data, data + len);
    *here = 0;
    notes->clear();
    const gd::Root root_kind = rp.tmpl ? gd::Root::Template : gd::Root::Plain;
    for (const char phase : kPhases) {
        for (size_t i = 0; i < rp.docs.size(); ++i) {
            if (left_out[i]) continue;
            const DocPatch& d = rp.docs[i];
            for (const StructOp& o : d.ops) {
                if (o.op != phase) continue;
                std::string err;
                size_t n = 0;
                std::vector<gd::CopySource> from;
                for (size_t k = 0; o.op == 'c' && k < o.names.size(); ++k) {
                    const std::string g = guid_text(o.sources[k].guid);
                    const auto it = sources.find(g);
                    from.push_back({o.names[k], g, it == sources.end() ? nullptr : it->second.data(), it == sources.end() ? 0 : it->second.size()});
                }
                const bool ok = o.op == '-' ? gd::boot_remove_components(type->resolved(), o.names, copy, &n, &err)
                              : o.op == '+' ? gd::boot_add_components(type->resolved(), o.names, copy, &n, &err)
                              : o.op == 'c' ? gd::boot_copy_components(type->resolved(), from, copy, &n, &err)
                              : o.op == 'x' ? gd::boot_clear(root_kind, type->resolved(), o.path, copy, &n, &err)
                              : o.op == 'r' ? gd::boot_replace(root_kind, type->resolved(), o.path, o.values, copy, &n, &err)
                                            : gd::boot_append(root_kind, type->resolved(), o.path, o.values, o.above_last, copy, &n, &err);
                if (!ok) return *bad = i, *why = err, false;
                if (copy->size() > kMaxResource)
                    return *bad = i, *why = "the patched " + what + " would be over " + std::to_string(kMaxResource >> 20) + " MiB", false;
                // a clear or a replace changes the list even when it ends as long as it was
                *here += o.op == 'x' || o.op == 'r' ? std::max<size_t>(n, 1) : n;
                const std::string list = " \"" + o.path + "\": ";
                notes->push_back("boot: resource patch " + d.mod + "/" + d.key + ": " + what +
                                 (o.op == 'a'   ? list + std::to_string(n) + " value(s) appended"
                                  : o.op == 'x' ? list + "cleared (" + std::to_string(n) + " element(s) were there)"
                                  : o.op == 'r' ? list + "replaced by " + std::to_string(n) + " value(s)"
                                                : ": " + std::to_string(n) + " component(s) " +
                                                      (o.op == '+' ? "added" : o.op == 'c' ? "copied" : "removed")));
            }
        }
    }
    const uintptr_t lo = reinterpret_cast<uintptr_t>(copy->data()), hi = lo + copy->size();
    for (size_t i = 0; i < rp.docs.size(); ++i) {
        if (left_out[i]) continue;
        const DocPatch& d = rp.docs[i];
        for (const Set& s : d.sets) {
            auto failed = [&](const std::string& e) { return *bad = i, *why = "\"" + s.path + "\": " + e, false; };
            std::vector<gd::Resolved> found;
            std::string err;
            if (!gd::resolve(root_kind, gd::Node{type->resolved(), lo}, s.path, &found, &err)) return failed(err.empty() ? "no such member in " + what : err);
            if (found.empty()) {
                if (s.path.find('*') == std::string::npos) return failed(err.empty() ? "IndexPastEnd: no such element in " + what : err);
                notes->push_back("boot: resource patch " + d.mod + "/" + d.key + " \"" + s.path + "\": the list is empty in " + what + ": nothing to change");
                continue;
            }
            // every element is computed before any is written
            std::vector<std::pair<uintptr_t, std::vector<uint8_t>>> writes;
            std::string bad_value;
            for (const gd::Resolved& r : found) {
                Field f;
                if (!r.node.t || !gd::leaf_of(r.node.t, &f) || f.ref || f.vt == KILN_CV_GUID) {
                    bad_value = "NotScalar: " + r.path + " is not a number, a bool or an enum";
                    break;
                }
                if (!check_value(f, s.value, &bad_value)) break;
                if (r.node.addr < lo || r.node.addr + f.bytes > hi) {
                    bad_value = r.path + " lies outside the resource's own bytes";
                    break;
                }
                uint8_t out[16] = {};
                bool reset = false;
                if (!gd::parse_leaf(f, gjson::parse(s.value.dump()), reinterpret_cast<const uint8_t*>(r.node.addr), out, &reset, &bad_value)) break;
                if (reset) continue;  // {"reset": true}: the game's own value stays
                writes.emplace_back(r.node.addr, std::vector<uint8_t>(out, out + f.bytes));
            }
            if (!bad_value.empty()) return failed(bad_value);
            for (const auto& [addr, bytes] : writes) std::memcpy(reinterpret_cast<void*>(addr), bytes.data(), bytes.size());
            *here += writes.size();
            notes->push_back("boot: resource patch " + d.mod + "/" + d.key + ": " + what + " \"" + s.path + "\" = " + s.value.dump() + " (" +
                             std::to_string(writes.size()) + " value(s))");
        }
    }
    return true;
}

// kiln.assets when_resources_ready: the game's resource service is up, nothing has read through it yet.
void KILN_CALL apply(void*) {
    const auto* plan = static_cast<const BootPlan*>(kiln::boot::published(g_api, "patches"));
    if (!plan) {
        g_ready_before_publication.store(true);
        return;
    }
    if (plan->resources.empty()) return;
    LOGI("boot: the game's resource service is up: resource patches of %zu resource(s) are written into copies of the bytes kiln.assets serves now", plan->resources.size());
    const KilnAssetsApi* A = assets();
    size_t written = 0, failed = 0;
    // one fault per document, naming its mod and key: for a required mod kiln.boot ends the process inside the call
    auto fault = [&](const DocPatch& d, const std::string& why) {
        ++failed;
        LOGE("boot: resource patch %s/%s: %s; nothing of this patch was applied", d.mod.c_str(), d.key.c_str(), why.c_str());
        kiln::boot::activation_fault(g_api, g_owner, d.mod, kKind, d.key, why + "; nothing of this patch was applied");
    };
    // the templates copyComponents copies from, read before any patched copy is served
    Sources sources;
    for (const ResPatch& rp : plan->resources)
        for (const DocPatch& d : rp.docs)
            for (const StructOp& o : d.ops)
                for (size_t k = 0; o.op == 'c' && k < o.sources.size(); ++k) {
                    const std::string g = guid_text(o.sources[k].guid);
                    const void* p = nullptr;
                    size_t n = 0;
                    if (sources.count(g) || !A || A->resource_data(&o.sources[k], &p, &n) != KILN_OK || !p || n > kMaxResource) continue;
                    sources[g].assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
                }
    for (const ResPatch& rp : plan->resources) {
        const std::string what = rp.type + " " + guid_text(rp.res.guid);
        const void* data = nullptr;
        size_t len = 0;
        const gd::TType* type = gd::type_named(rp.type);
        std::string whole;
        if (!A) whole = "kiln.assets' resources are not available";
        else if (!type || !type->resolved() || type->resolved()->kind != gd::K_STRUCT) whole = "UnknownType: this game build has no struct type \"" + rp.type + "\"";
        else if (A->resource_data(&rp.res, &data, &len) != KILN_OK || !data) whole = "MissingResource: this game build has no " + what;
        else if (len < type->resolved()->size || len > kMaxResource)
            whole = "the resource " + what + " is " + std::to_string(len) + " bytes, which is not a " + rp.type;
        if (!whole.empty()) {
            for (const DocPatch& d : rp.docs) fault(d, whole);
            continue;
        }
        // a document that fails is left out and the copy is built again from the game's bytes: the resource is then what
        // the other documents make it (each failure leaves one more out, so this ends)
        std::vector<char> left_out(rp.docs.size(), 0);
        std::vector<uint8_t> copy;
        std::vector<std::string> notes;
        size_t here = 0;
        for (;;) {
            size_t bad = 0;
            std::string why;
            if (build(rp, type, static_cast<const uint8_t*>(data), len, left_out, sources, &copy, &here, &notes, &bad, &why)) break;
            left_out[bad] = 1;
            fault(rp.docs[bad], why);
        }
        for (const std::string& n : notes) LOGI("%s", n.c_str());
        if (!here) continue;
        const KilnResult rc = A->resource_put(g_owner, &rp.res, copy.data(), copy.size(), KILN_ASSET_PUT_OVERRIDE);
        if (rc != KILN_OK) {
            for (size_t i = 0; i < rp.docs.size(); ++i)
                if (!left_out[i]) fault(rp.docs[i], "kiln.assets refused the patched " + what + " (" + std::to_string(rc) + ")");
            continue;
        }
        written += here;
    }
    LOGI("boot: resource patches applied before the game read its resources: %zu value(s) in %zu resource(s)%s", written, plan->resources.size(),
         failed ? (", " + std::to_string(failed) + " patch(es) not applied (boot.json)").c_str() : "");
    if (failed) row(KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE, std::to_string(failed) + " resource patch(es) of this start were not applied (state/boot.json)");
}

}  // namespace

// kiln.content's bind (content.cpp, after gd_boot): the collector of a boot profile and the row.
void gd_boot_patches_bind() {
    const auto* caps = static_cast<const KilnCapsApi*>(g_api->service(KilnStr{KILN_CAPS_SERVICE, sizeof(KILN_CAPS_SERVICE) - 1}, 1));
    const KilnAssetsApi* A = assets();
    if (!A) g_off = "kiln.assets' resources are off on this build";
    else if (kiln_caps_held(caps, KilnStr{kRow, sizeof(kRow) - 1})) g_off = "mods.boot.resource_patches is held until the client checks pass";
    g_bound = kiln::boot::collect(g_api, g_owner, "patches", &prepare_boot, nullptr);
    if (g_bound && !g_off) g_ready_asked = A->when_resources_ready(&apply, nullptr) == KILN_OK;
    if (g_bound && !g_off && !g_ready_asked) g_off = "kiln.assets' resources are off on this build";
    if (g_off && !kiln_caps_held(caps, KilnStr{kRow, sizeof(kRow) - 1}))
        row(KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_SERVICE_MISSING, g_off);
    else if (!g_off)
        row(g_bound ? KILN_CAP_AVAILABLE : KILN_CAP_UNAVAILABLE, g_bound ? KILN_CAP_REASON_NONE : KILN_CAP_REASON_POLICY,
            g_bound ? "" : "no boot phase in this process (no enabled mod has \"boot\" in its kiln.mod.json)");
    if (!g_bound) {
        // no boot phase: a blocks file's patches would be read by nobody, so say it once per mod
        std::set<std::string> said;
        try {
            for (const auto& d : file_docs(nullptr))
                if (said.insert(d.owner).second)
                    LOGW("mod %s has \"resourcePatches\" in a blocks file, and this start has no boot phase: they are not applied. Add \"boot\": true to "
                         "its kiln.mod.json",
                         d.owner.c_str());
        } catch (const std::exception&) {
        }
    }
    if (g_bound) LOGI("boot profile: resource patches are applied when the game's resource service is up%s%s", g_off ? "; off: " : "", g_off ? g_off : "");
}

}  // namespace kc
