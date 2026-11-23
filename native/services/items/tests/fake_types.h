// SPDX-License-Identifier: MIT
// A stand-in for kiln.types (host and Wine tests; no game): a catalog of named types with the rules of
// native/services/types/types.cpp that the storage guard depends on. type_info refuses nothing it holds; type_field
// answers only for a struct (an enum's or bitmask's field_count counts its values, which are not fields) and only when
// the field's type is in the catalog, as the real service does.
#pragma once
#include "services/kiln_types.h"
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fake_types {

struct Field {
    std::string name, type;
    uint64_t offset;
};
struct Type {
    std::string qname, name;
    uint32_t size = 0;
    uint8_t kind = 0;
    uint32_t count = 0; // own fields (struct) or values (enum, bitmask), as the game's descriptor says
    std::vector<Field> fields;
};

struct Catalog {
    std::map<std::string, std::unique_ptr<Type>> types; // by qualified name; the handles stay put
    Type& add(const std::string& qname, uint32_t size, uint8_t kind, uint32_t count = 0,
              std::vector<Field> fields = {}) {
        auto& t = types[qname];
        t = std::make_unique<Type>();
        t->qname = qname;
        const auto at = qname.rfind("::");
        t->name = at == std::string::npos ? qname : qname.substr(at + 2);
        t->size = size;
        t->kind = kind;
        t->count = kind == KILN_TYPE_STRUCT && !count ? static_cast<uint32_t>(fields.size()) : count;
        t->fields = std::move(fields);
        return *t;
    }
    // A struct with these fields (count = the number of fields).
    Type& add_struct(const std::string& qname, uint32_t size, std::vector<Field> fields) {
        return add(qname, size, KILN_TYPE_STRUCT, static_cast<uint32_t>(fields.size()), std::move(fields));
    }
    Type* find(const std::string& qname) {
        auto it = types.find(qname);
        return it == types.end() ? nullptr : it->second.get();
    }
};

inline Catalog* current = nullptr;

inline const Type* of(const KilnType* t) {
    if (!current || !t)
        return nullptr;
    for (const auto& [name, type] : current->types)
        if (reinterpret_cast<const KilnType*>(type.get()) == t)
            return type.get();
    return nullptr;
}
inline const KilnType* handle(const Type* t) { return reinterpret_cast<const KilnType*>(t); }
inline KilnStr ks(const std::string& s) { return {s.data(), s.size()}; }

inline const KilnType* KILN_CALL type_by_name(KilnStr name) {
    if (!current || !name.ptr || !name.len)
        return nullptr;
    const std::string n(name.ptr, name.len);
    if (n.find("::") != std::string::npos)
        return handle(current->find(n));
    if (auto* t = current->find("keen::ecs::" + n))
        return handle(t);
    const Type* only = nullptr;
    for (const auto& [qname, t] : current->types)
        if (t->name == n) {
            if (only)
                return nullptr; // ambiguous
            only = t.get();
        }
    return handle(only);
}
inline KilnResult KILN_CALL type_info(const KilnType* type, KilnTypeInfo* out) {
    if (!out || out->size < sizeof(KilnTypeInfo))
        return KILN_ERR_INVALID_ARGUMENT;
    const Type* t = of(type);
    if (!t)
        return KILN_ERR_NOT_FOUND;
    const uint32_t size = out->size;
    *out = {};
    out->size = size;
    out->name = ks(t->name);
    out->qualified_name = ks(t->qname);
    out->type_size = t->size;
    out->kind = t->kind;
    out->field_count = t->count;
    return KILN_OK;
}
inline KilnResult KILN_CALL type_field(const KilnType* type, uint32_t index, KilnTypeField* out) {
    const Type* t = of(type);
    if (!out || !t)
        return out ? KILN_ERR_NOT_FOUND : KILN_ERR_INVALID_ARGUMENT;
    if (t->kind != KILN_TYPE_STRUCT || index >= t->count || index >= t->fields.size())
        return KILN_ERR_NOT_FOUND;
    const Field& f = t->fields[index];
    const Type* ft = current->find(f.type);
    if (!ft || f.offset > t->size || ft->size > t->size - f.offset)
        return KILN_ERR_NOT_FOUND;
    out->name = ks(f.name);
    out->type = handle(ft);
    out->offset = f.offset;
    return KILN_OK;
}

// A kiln.types table over `catalog` (only the members the storage guard calls).
inline KilnTypesApi api(Catalog* catalog) {
    current = catalog;
    KilnTypesApi a{};
    a.header = {sizeof a, KILN_TYPES_VERSION};
    a.type_by_name = &type_by_name;
    a.type_info = &type_info;
    a.type_field = &type_field;
    return a;
}

} // namespace fake_types
