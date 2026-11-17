// SPDX-License-Identifier: MIT
// kiln.content gamedata: the structural edits of a boot resource patch (gamedata_bootpatch.cpp), made in the copy of the
// resource kiln.assets serves before the patch's field writes:
//   "removeComponents": ["SpawnTime"]        a template's components it does without (the list keeps its order)
//   "addComponents": ["PickupItemZone", ...]  components of the game's types a template has not got, zeroed; the patch's
//                                             "set" fills their fields in ("PickupItemZone.lastAttempt.value")
//   "copyComponents": {"InventoryCraftingStock": "<template GUID>"}
//                                             a component copied whole from another template (its lists, strings,
//                                             optionals and variants with it): added, or the template's own replaced
//   "clear": ["noBuildZones"]                 a list (any element type) made empty
//   "replace": {"<list path>": {"values": [1, 2]}}
//                                             a list of scalars that holds exactly these values
//   "append": {"<list path>": {"values": [2.5, 3], "aboveLast": true}}
//                                             numbers (or enum members) added to the end of a list of scalars;
//                                             aboveLast: only values greater than the list's last (a sorted list stays sorted)
// The copy only grows: a moved list and a new component's data go behind the resource's own bytes (16-aligned), every
// offset involved is self-relative, and nothing the game's other data points at moves. The components list of a template
// stays sorted by type name, as the game's are (RegisterTemplateInfo; gamedata_clones.cpp add_components).
#include <algorithm>
#include <cstdio>
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
bool read_components(const TType* type, const uint8_t* b, size_t len, size_t* field_at, std::vector<Var>* vars, std::string* err) {
    const TField* cf = type ? type->field("components") : nullptr;
    if (!cf) return *err = "only a template (keen::ecs::TemplateResource) has components", false;
    *field_at = cf->offset;
    if (len < size_t{cf->offset} + 8) return *err = "the template's component list is not readable", false;
    int32_t rel = 0;
    uint32_t count = 0;
    std::memcpy(&rel, b + cf->offset, 4);
    std::memcpy(&count, b + cf->offset + 4, 4);
    const int64_t list_at = static_cast<int64_t>(cf->offset) + rel;
    if (count && (!rel || list_at < 0 || static_cast<uint64_t>(list_at) + count * 12ull > len || count > 4096))
        return *err = "the template's component list is not readable", false;
    vars->clear();
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = static_cast<size_t>(list_at) + i * 12ull;
        Var v;
        int32_t r = 0;
        std::memcpy(&v.hash, b + at, 4);
        std::memcpy(&r, b + at + 4, 4);
        std::memcpy(&v.size, b + at + 8, 4);
        const TType* t = type_hash(v.hash);
        v.brief = t ? t->brief : std::string();
        v.data = static_cast<int64_t>(at) + 4 + r;
        vars->push_back(std::move(v));
    }
    return true;
}
bool read_components(const TType* type, const std::vector<uint8_t>& b, size_t* field_at, std::vector<Var>* vars, std::string* err) {
    return read_components(type, b.data(), b.size(), field_at, vars, err);
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

// The list a "clear", "replace" or "append" path names: one BlobArray inside the copy. *where: its offset in the copy.
bool one_list(Root kind, const TType* root, const std::string& op, const std::string& path, const std::vector<uint8_t>& bytes, size_t* where,
              const TType** list, std::string* err) {
    std::vector<Resolved> rs;
    std::string why;
    const uintptr_t base = reinterpret_cast<uintptr_t>(bytes.data());
    if (!resolve(kind, Node{root, base}, path, &rs, &why) || rs.size() != 1 || !rs[0].node.t)
        return *err = "\"" + op + "\": " + path + ": " + (why.empty() ? "it is not one list of the resource" : why), false;
    *list = rs[0].node.t;
    if ((*list)->kind != K_BLOBARRAY) return *err = "\"" + op + "\": " + path + " is not a list", false;
    if (rs[0].node.addr < base || rs[0].node.addr + 8 > base + bytes.size())
        return *err = "\"" + op + "\": " + path + " lies outside the resource's own bytes", false;
    *where = rs[0].node.addr - base;
    return true;
}

// The deep copy of copyComponents: a value of a reflected type from the source template's bytes into the copy. The
// value's own bytes are copied by the caller; fix() then gives every list, string, optional and variant inside it a copy
// of its data behind the copy (16-aligned, as append and addComponents place theirs) and points it there. Every read of
// the source is checked against its length; a kind it cannot copy (the editor's Ds* kinds, an unknown variant type) is
// refused, never guessed.
struct Copier {
    const uint8_t* src = nullptr;
    size_t src_len = 0;
    std::vector<uint8_t>* dst = nullptr;
    std::string* err = nullptr;
    size_t budget = 64u << 20;  // bytes this copy may add (the patched resource is held below 64 MiB anyway)

    template <class T>
    bool rd(int64_t at, T* v) const {
        if (at < 0 || static_cast<uint64_t>(at) + sizeof(T) > src_len) return false;
        std::memcpy(v, src + at, sizeof(T));
        return true;
    }
    template <class T>
    void wr(size_t at, T v) { std::memcpy(dst->data() + at, &v, sizeof(T)); }
    bool fail(const std::string& why) { return *err = why, false; }
    // n bytes of the source at `from`, behind the copy; *at: where they are now
    bool place(int64_t from, size_t n, size_t* at, const std::string& what) {
        if (from < 0 || static_cast<uint64_t>(from) + n > src_len) return fail(what + " lies outside the source template's bytes");
        if (n > budget) return fail(what + " is too large to copy");
        budget -= n;
        *at = align16(dst->size());
        dst->resize(align16(*at + n + 1), 0);  // one zero byte behind it at least (a string's terminator)
        if (n) std::memcpy(dst->data() + *at, src + from, n);
        return true;
    }
    // the value of type t at source offset `from`, already copied to `to`
    bool fix(const TType* t, int64_t from, size_t to, int depth) {
        const TType* r = t ? t->resolved() : nullptr;
        if (!r) return fail("a member of a type the game's reflection does not name");
        if (depth > 32) return fail(r->name + " is nested too deep to copy");
        const uint32_t k = r->kind;
        if ((k >= K_BOOL && k <= K_BM64) || k == K_REF || k == K_GUID) return true;  // plain bytes
        switch (k) {
            case K_STRUCT:
                for (const TField& f : r->fields) {
                    const TType* ft = f.type();
                    if (!ft) return fail(r->name + "." + f.name + " has a type the game's reflection does not name");
                    if (!fix(ft, from + f.offset, to + f.offset, depth + 1)) return false;
                }
                return true;
            case K_ARRAY: {
                const TType* el = r->inner() ? r->inner()->resolved() : nullptr;
                if (!el) return fail(r->name + ": its element type is not known");
                const uint32_t es = el->size ? el->size : (r->count ? r->size / r->count : 0);
                for (uint32_t i = 0; i < r->count && i < 65536; ++i)
                    if (!fix(el, from + int64_t{i} * es, to + size_t{i} * es, depth + 1)) return false;
                return true;
            }
            case K_BLOBARRAY: {
                int32_t rel = 0;
                uint32_t count = 0;
                if (!rd(from, &rel) || !rd(from + 4, &count)) return fail(r->name + " lies outside the source template's bytes");
                if (!rel || !count) return wr<int32_t>(to, 0), true;  // empty: nothing to point at
                const TType* el = r->inner() ? r->inner()->resolved() : nullptr;
                if (!el) return fail(r->name + ": its element type is not known");
                if (count > 1000000) return fail(r->name + " has too many elements to copy");
                const size_t es = el->size ? el->size : 12;
                const int64_t data = from + rel;
                size_t at = 0;
                if (!place(data, count * es, &at, r->name)) return false;
                wr<int32_t>(to, static_cast<int32_t>(static_cast<int64_t>(at) - static_cast<int64_t>(to)));
                for (uint32_t i = 0; i < count; ++i)
                    if (!fix(el, data + int64_t{i} * static_cast<int64_t>(es), at + size_t{i} * es, depth + 1)) return false;
                return true;
            }
            case K_BLOBSTRING: {
                int32_t rel = 0;
                uint32_t len = 0;
                if (!rd(from, &rel) || !rd(from + 4, &len)) return fail(r->name + " lies outside the source template's bytes");
                if (!rel || !len) return wr<int32_t>(to, 0), true;
                if (len > (1u << 20)) return fail("a string of " + std::to_string(len) + " bytes is too long to copy");
                size_t at = 0;
                if (!place(from + rel, len, &at, "a string")) return false;
                wr<int32_t>(to, static_cast<int32_t>(static_cast<int64_t>(at) - static_cast<int64_t>(to)));
                return true;
            }
            case K_BLOBOPT: {
                int32_t rel = 0;
                if (!rd(from, &rel)) return fail(r->name + " lies outside the source template's bytes");
                if (!rel) return true;
                const TType* in = r->inner() ? r->inner()->resolved() : nullptr;
                if (!in || !in->size) return fail(r->name + ": its value's type is not known");
                size_t at = 0;
                if (!place(from + rel, in->size, &at, r->name)) return false;
                wr<int32_t>(to, static_cast<int32_t>(static_cast<int64_t>(at) - static_cast<int64_t>(to)));
                return fix(in, from + rel, at, depth + 1);
            }
            case K_BLOBVAR: {
                uint32_t hash = 0, size = 0;
                int32_t rel = 0;
                if (!rd(from, &hash) || !rd(from + 4, &rel) || !rd(from + 8, &size)) return fail(r->name + " lies outside the source template's bytes");
                if (!hash) return true;
                const TType* vt = type_hash(hash);
                if (!vt && size) return fail(r->name + " holds a type the game's reflection does not name");
                if (size > (16u << 20)) return fail(r->name + " is too large to copy");
                const int64_t data = from + 4 + rel;
                size_t at = 0;
                if (!place(data, size, &at, r->name)) return false;
                wr<int32_t>(to + 4, static_cast<int32_t>(static_cast<int64_t>(at) - static_cast<int64_t>(to + 4)));
                return !vt || !size || fix(vt, data, at, depth + 1);
            }
            default: {
                char kind[16];
                std::snprintf(kind, sizeof kind, "0x%02x", k);
                return fail(r->name + " is of a kind (" + kind + ") a component copy does not take");
            }
        }
    }
};

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
        std::string why;
        const TType* t = component_type(brief, &why);
        if (!t) return *err = "\"addComponents\": " + why, false;
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

bool boot_clear(Root kind, const TType* root, const std::string& path, std::vector<uint8_t>* bytes, size_t* cleared, std::string* err) {
    *cleared = 0;
    size_t where = 0;
    const TType* list = nullptr;
    if (!one_list(kind, root, "clear", path, *bytes, &where, &list, err)) return false;
    int32_t rel = 0;
    uint32_t count = 0;
    std::memcpy(&rel, bytes->data() + where, 4);
    std::memcpy(&count, bytes->data() + where + 4, 4);
    if (!rel || !count) return true;  // empty already
    const int32_t zero = 0;
    std::memcpy(bytes->data() + where, &zero, 4);
    std::memcpy(bytes->data() + where + 4, &zero, 4);
    *cleared = count;
    return true;
}

bool boot_replace(Root kind, const TType* root, const std::string& path, const std::vector<gjson>& values, std::vector<uint8_t>* bytes,
                  size_t* written, std::string* err) {
    *written = 0;
    size_t where = 0;
    const TType* list = nullptr;
    if (!one_list(kind, root, "replace", path, *bytes, &where, &list, err)) return false;
    const TType* el = list->inner() ? list->inner()->resolved() : nullptr;
    Field f;
    if (!el || !leaf_of(el, &f) || f.ref || f.vt == KILN_CV_GUID || !f.bytes || f.bytes > 8)
        return *err = "\"replace\": " + path + " is not a list of numbers, bools or enum members", false;
    if (values.empty() || values.size() > 1000000) return *err = "\"replace\": " + path + ": no values, or too many", false;
    const size_t el_size = el->size ? el->size : f.bytes;
    std::vector<std::vector<uint8_t>> out;
    const std::vector<uint8_t> zero(16, 0);
    std::string why;
    for (const gjson& v : values) {
        uint8_t b[16] = {};
        bool reset = false;
        if (v.is_object()) return *err = "\"replace\": " + path + ": a value is a number, a bool or an enum member's name", false;
        if (!parse_leaf(f, v, zero.data(), b, &reset, &why)) return *err = "\"replace\": " + path + ": " + why, false;
        out.emplace_back(b, b + f.bytes);
    }
    const size_t list_at = align16(bytes->size());
    bytes->resize(align16(list_at + out.size() * el_size), 0);
    for (size_t i = 0; i < out.size(); ++i) std::memcpy(bytes->data() + list_at + i * el_size, out[i].data(), out[i].size());
    const int32_t rel = static_cast<int32_t>(static_cast<int64_t>(list_at) - static_cast<int64_t>(where));
    const uint32_t count = static_cast<uint32_t>(out.size());
    std::memcpy(bytes->data() + where, &rel, 4);
    std::memcpy(bytes->data() + where + 4, &count, 4);
    *written = out.size();
    return true;
}

bool boot_copy_components(const TType* type, const std::vector<CopySource>& copies, std::vector<uint8_t>* bytes, size_t* copied,
                          std::string* err) {
    *copied = 0;
    if (copies.empty()) return true;
    size_t field_at = 0;
    std::vector<Var> vars;
    if (!read_components(type, *bytes, &field_at, &vars, err)) return false;
    const bool actor = std::any_of(vars.begin(), vars.end(), [](const Var& v) { return v.brief == "Actor"; });
    const size_t was = bytes->size();
    struct Undo {  // a refused copy leaves the bytes as they were
        std::vector<uint8_t>* b;
        size_t n;
        bool keep = false;
        ~Undo() { if (!keep) b->resize(n); }
    } undo{bytes, was};
    for (const CopySource& c : copies) {
        const std::string label = "\"copyComponents\": " + c.brief;
        if (essential(c.brief, actor)) return *err = label + " is what " + (actor ? "an actor" : "any entity") + " needs to live: it is not copied over", false;
        std::string why;
        const TType* t = component_type(c.brief, &why);
        if (!t) return *err = "\"copyComponents\": " + why, false;
        if (!c.data) return *err = label + ": MissingResource: this game build has no template " + c.from, false;
        size_t src_field = 0;
        std::vector<Var> src_vars;
        if (!read_components(type, c.data, c.len, &src_field, &src_vars, &why)) return *err = label + ": the template " + c.from + ": " + why, false;
        const auto s = std::find_if(src_vars.begin(), src_vars.end(), [&](const Var& v) { return v.brief == c.brief; });
        if (s == src_vars.end()) return *err = label + ": the template " + c.from + " has no " + c.brief, false;
        if (s->data < 0 || s->size < t->size || s->size > 65536 || static_cast<uint64_t>(s->data) + s->size > c.len)
            return *err = label + ": the " + c.brief + " of " + c.from + " is not readable", false;
        Copier cp{c.data, c.len, bytes, &why};
        size_t at = 0;
        if (!cp.place(s->data, s->size, &at, c.brief) || !cp.fix(t, s->data, at, 0)) return *err = label + " from " + c.from + ": " + why, false;
        const auto mine = std::find_if(vars.begin(), vars.end(), [&](const Var& v) { return v.brief == c.brief; });
        if (mine != vars.end()) {  // it has one: replaced by the copy (its old data stays where it is, unused)
            mine->hash = s->hash, mine->size = s->size, mine->data = static_cast<int64_t>(at);
        } else {
            vars.push_back({c.brief, s->hash, s->size, static_cast<int64_t>(at)});
        }
        ++*copied;
    }
    write_components(field_at, vars, bytes);
    undo.keep = true;
    return true;
}

}  // namespace kc::gd
