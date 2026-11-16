// SPDX-License-Identifier: MIT
// kiln.content gamedata: the structural edits of a boot resource patch (gamedata_bootpatch.cpp), made in the copy of the
// resource kiln.assets serves before the patch's field writes:
//   "removeComponents": ["SpawnTime"]        a template's components it does without (the list keeps its order)
//   "addComponents": ["PickupItemZone", ...]  components of the game's types a template has not got, zeroed; the patch's
//                                             "set" fills their fields in ("PickupItemZone.lastAttempt.value")
//   "append": {"<list path>": {"values": [2.5, 3], "aboveLast": true}}
//                                             numbers (or enum members) added to the end of a list of scalars;
//                                             aboveLast: only values greater than the list's last (a sorted list stays sorted)
// The copy only grows: a moved list and a new component's data go behind the resource's own bytes (16-aligned), every
// offset involved is self-relative, and nothing the game's other data points at moves. The components list of a template
// stays sorted by type name, as the game's are (RegisterTemplateInfo; gamedata_clones.cpp add_components).
#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "gamedata.h"

namespace kc::gd {
namespace {

struct Var {
    std::string brief;
    uint32_t hash = 0, size = 0;
    int64_t data = 0;  // offset of the component's data in the copy; -1: a new one
};

size_t align16(size_t n) { return (n + 15) & ~size_t(15); }

// The template's component list (TemplateResource.components, BlobArray of BlobVariant {hash, rel, size}).
bool read_components(const TType* type, const std::vector<uint8_t>& b, size_t* field_at, std::vector<Var>* vars, std::string* err) {
    const TField* cf = type ? type->field("components") : nullptr;
    if (!cf) return *err = "only a template (keen::ecs::TemplateResource) has components", false;
    *field_at = cf->offset;
    if (b.size() < size_t{cf->offset} + 8) return *err = "the template's component list is not readable", false;
    int32_t rel = 0;
    uint32_t count = 0;
    std::memcpy(&rel, b.data() + cf->offset, 4);
    std::memcpy(&count, b.data() + cf->offset + 4, 4);
    const int64_t list_at = static_cast<int64_t>(cf->offset) + rel;
    if (count && (!rel || list_at < 0 || static_cast<uint64_t>(list_at) + count * 12ull > b.size() || count > 4096))
        return *err = "the template's component list is not readable", false;
    vars->clear();
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = static_cast<size_t>(list_at) + i * 12ull;
        Var v;
        int32_t r = 0;
        std::memcpy(&v.hash, b.data() + at, 4);
        std::memcpy(&r, b.data() + at + 4, 4);
        std::memcpy(&v.size, b.data() + at + 8, 4);
        const TType* t = type_hash(v.hash);
        v.brief = t ? t->brief : std::string();
        v.data = static_cast<int64_t>(at) + 4 + r;
        vars->push_back(std::move(v));
    }
    return true;
}

// Writes the list `vars` behind the copy (16-aligned), new components' zeroed data after it, and points the field at it.
void write_components(size_t field_at, std::vector<Var>& vars, std::vector<uint8_t>* b) {
    std::stable_sort(vars.begin(), vars.end(), [](const Var& x, const Var& y) { return x.brief < y.brief; });
    const size_t list_at = align16(b->size());
    size_t data_at = align16(list_at + vars.size() * 12);
    size_t end = data_at;
    for (const Var& v : vars)
        if (v.data < 0) end = align16(end) + v.size;
    b->resize(align16(end), 0);
    for (size_t i = 0; i < vars.size(); ++i) {
        Var& v = vars[i];
        if (v.data < 0) {
            data_at = align16(data_at);
            v.data = static_cast<int64_t>(data_at);
            data_at += v.size;
        }
        const size_t at = list_at + i * 12;
        const int32_t r = static_cast<int32_t>(v.data - static_cast<int64_t>(at + 4));
        std::memcpy(b->data() + at, &v.hash, 4);
        std::memcpy(b->data() + at + 4, &r, 4);
        std::memcpy(b->data() + at + 8, &v.size, 4);
    }
    const int32_t rel = static_cast<int32_t>(static_cast<int64_t>(list_at) - static_cast<int64_t>(field_at));
    const uint32_t count = static_cast<uint32_t>(vars.size());
    std::memcpy(b->data() + field_at, &rel, 4);
    std::memcpy(b->data() + field_at + 4, &count, 4);
}

// What a template needs to exist at all (gamedata_clones.cpp remove_components): never removed.
bool essential(const std::string& brief, bool actor) {
    static const std::set<std::string> kept = {"CurrentTransform", "LastTransform", "Actor", "ActorInput", "NetworkActor", "Velocity", "Faction",
                                               "AnimationGraphResourceComponent2", "ServerAnimationGraphInput", "NetworkAnimationGraphInput",
                                               "ServerAnimationGraphComponent2", "ServerAnimationGraphSlotComponent"};
    return brief == "CurrentTransform" || (actor && kept.count(brief));
}

double scalar_of(const Field& f, const uint8_t* p) {
    switch (f.bytes) {
        case 1: return f.vt == KILN_CV_BOOL ? (p[0] ? 1 : 0) : f.is_signed ? static_cast<int8_t>(p[0]) : p[0];
        case 2: { uint16_t v; std::memcpy(&v, p, 2); return f.is_signed ? static_cast<int16_t>(v) : v; }
        case 4: {
            if (f.vt == KILN_CV_FLOAT) { float v; std::memcpy(&v, p, 4); return v; }
            uint32_t v; std::memcpy(&v, p, 4); return f.is_signed ? static_cast<int32_t>(v) : v;
        }
        case 8: {
            if (f.vt == KILN_CV_FLOAT) { double v; std::memcpy(&v, p, 8); return v; }
            uint64_t v; std::memcpy(&v, p, 8); return f.is_signed ? static_cast<double>(static_cast<int64_t>(v)) : static_cast<double>(v);
        }
        default: return 0;
    }
}

}  // namespace

bool boot_remove_components(const TType* type, const std::vector<std::string>& names, std::vector<uint8_t>* bytes, size_t* removed,
                            std::string* err) {
    *removed = 0;
    if (names.empty()) return true;
    size_t field_at = 0;
    std::vector<Var> vars;
    if (!read_components(type, *bytes, &field_at, &vars, err)) return false;
    const bool actor = std::any_of(vars.begin(), vars.end(), [](const Var& v) { return v.brief == "Actor"; });
    for (const std::string& n : names)
        if (essential(n, actor)) return *err = "\"removeComponents\": " + n + " is what " + (actor ? "an actor" : "any entity") + " needs to live", false;
    const size_t before = vars.size();
    vars.erase(std::remove_if(vars.begin(), vars.end(), [&](const Var& v) { return std::find(names.begin(), names.end(), v.brief) != names.end(); }),
               vars.end());
    *removed = before - vars.size();
    if (!*removed) return true;  // the template has none of them: nothing to change
    // the list keeps its place: entries close up, their data stays where it is (unused)
    int32_t rel = 0;
    std::memcpy(&rel, bytes->data() + field_at, 4);
    const size_t list_at = static_cast<size_t>(static_cast<int64_t>(field_at) + rel);
    for (size_t i = 0; i < vars.size(); ++i) {
        const size_t at = list_at + i * 12;
        const int32_t r = static_cast<int32_t>(vars[i].data - static_cast<int64_t>(at + 4));
        std::memcpy(bytes->data() + at, &vars[i].hash, 4);
        std::memcpy(bytes->data() + at + 4, &r, 4);
        std::memcpy(bytes->data() + at + 8, &vars[i].size, 4);
    }
    const uint32_t left = static_cast<uint32_t>(vars.size());
    std::memcpy(bytes->data() + field_at + 4, &left, 4);
    return true;
}

bool boot_add_components(const TType* type, const std::vector<std::string>& names, std::vector<uint8_t>* bytes, size_t* added,
                         std::string* err) {
    *added = 0;
    if (names.empty()) return true;
    size_t field_at = 0;
    std::vector<Var> vars;
    if (!read_components(type, *bytes, &field_at, &vars, err)) return false;
    for (const std::string& brief : names) {
        if (std::any_of(vars.begin(), vars.end(), [&](const Var& v) { return v.brief == brief; })) continue;  // it has one: kept as it is
        const TType* t = type_named("keen::ecs::" + brief);
        const TType* b = t;
        while (b && b->name != "keen::ecs::Component") b = b->inner();
        if (!t || !b) return *err = "\"addComponents\": \"" + brief + "\" is not a component type of the game", false;
        if (!t->size || t->size > 65536) return *err = "\"addComponents\": \"" + brief + "\" has an unusable size", false;
        vars.push_back({brief, t->hash, t->size, -1});
        ++*added;
    }
    if (*added) write_components(field_at, vars, bytes);
    return true;
}

bool boot_append(Root kind, const TType* root, const std::string& path, const std::vector<gjson>& values, bool above_last,
                 std::vector<uint8_t>* bytes, size_t* appended, std::string* err) {
    *appended = 0;
    std::vector<Resolved> rs;
    std::string why;
    const uintptr_t base = reinterpret_cast<uintptr_t>(bytes->data());
    if (!resolve(kind, Node{root, base}, path, &rs, &why) || rs.size() != 1 || !rs[0].node.t)
        return *err = "\"append\": " + path + ": " + (why.empty() ? "it is not one list of the resource" : why), false;
    const TType* list = rs[0].node.t;
    const TType* el = list->kind == K_BLOBARRAY && list->inner() ? list->inner()->resolved() : nullptr;
    Field f;
    if (!el || !leaf_of(el, &f) || f.ref || f.vt == KILN_CV_GUID || !f.bytes || f.bytes > 8)
        return *err = "\"append\": " + path + " is not a list of numbers, bools or enum members", false;
    if (rs[0].node.addr < base || rs[0].node.addr + 8 > base + bytes->size())
        return *err = "\"append\": " + path + " lies outside the resource's own bytes", false;
    const size_t where = rs[0].node.addr - base;
    const size_t el_size = el->size ? el->size : f.bytes;
    int32_t rel = 0;
    uint32_t count = 0;
    std::memcpy(&rel, bytes->data() + where, 4);
    std::memcpy(&count, bytes->data() + where + 4, 4);
    const int64_t old_at = static_cast<int64_t>(where) + rel;
    if (count && (!rel || old_at < 0 || static_cast<uint64_t>(old_at) + count * el_size > bytes->size()))
        return *err = "\"append\": " + path + ": the list is not readable", false;
    // the new elements' bytes, in order
    std::vector<std::vector<uint8_t>> add;
    bool have_last = count > 0;
    double last = have_last ? scalar_of(f, bytes->data() + old_at + (count - 1) * el_size) : 0;
    const std::vector<uint8_t> zero(16, 0);
    for (const gjson& v : values) {
        uint8_t out[16] = {};
        bool reset = false;
        if (v.is_object()) return *err = "\"append\": " + path + ": a value is a number, a bool or an enum member's name", false;
        if (!parse_leaf(f, v, zero.data(), out, &reset, &why)) return *err = "\"append\": " + path + ": " + why, false;
        const double x = scalar_of(f, out);
        if (above_last && have_last && !(x > last)) continue;
        add.emplace_back(out, out + f.bytes);
        last = x;
        have_last = true;
    }
    if (add.empty()) return true;
    if (count + add.size() > 1000000) return *err = "\"append\": " + path + ": the list would be too long", false;
    const size_t list_at = align16(bytes->size());
    bytes->resize(align16(list_at + (count + add.size()) * el_size), 0);
    if (count) std::memmove(bytes->data() + list_at, bytes->data() + old_at, count * el_size);
    for (size_t i = 0; i < add.size(); ++i) std::memcpy(bytes->data() + list_at + (count + i) * el_size, add[i].data(), add[i].size());
    const int32_t new_rel = static_cast<int32_t>(static_cast<int64_t>(list_at) - static_cast<int64_t>(where));
    const uint32_t new_count = count + static_cast<uint32_t>(add.size());
    std::memcpy(bytes->data() + where, &new_rel, 4);
    std::memcpy(bytes->data() + where + 4, &new_count, 4);
    *appended = add.size();
    return true;
}

}  // namespace kc::gd
