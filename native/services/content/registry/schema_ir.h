// SPDX-License-Identifier: MIT
// kiln.content registry stage: the generated schema (schema.g.h, tools/registrygen/registrygen.py) as the edit IR's
// descriptor (ir.h's Schema). One FieldSpec per generated row: a scalar leaf ("maxStack", "damage.fire"), a list of plain
// values ("perks", "altarsPerFlameLevel") or a leaf of a list's elements ("inputs[].count"). Kinds map as:
//   int -> Int, float and time (seconds) -> Float, bool -> Bool, enum -> Enum, ref -> Id,
//   flags -> List of Enum (the set of names; at most one of each), list of plain values -> List.
// A list of entries with fields of their own (recipes.inputs, drops.entries) has no FieldSpec yet: ir.h's FieldType has
// no element with fields. unsupported_fields() names them so that the IR (S3) can take them up; their element leaves are
// in the schema already.
#pragma once
#include <string>
#include <vector>

#include "ir.h"
#include "schema.g.h"

namespace kc::reg {

namespace schema_detail {

inline std::vector<std::string> split_names(const char* s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = s; *p; ++p) {
        if (*p == '|') out.push_back(cur), cur.clear();
        else cur += *p;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

inline bool scalar_type(gen::Kind k, FieldType* out) {
    switch (k) {
        case gen::Kind::Int: return *out = FieldType::Int, true;
        case gen::Kind::Float:
        case gen::Kind::Time: return *out = FieldType::Float, true;
        case gen::Kind::Bool: return *out = FieldType::Bool, true;
        case gen::Kind::Enum: return *out = FieldType::Enum, true;
        case gen::Kind::Ref: return *out = FieldType::Id, true;
        default: return false;
    }
}

inline Applies applies_of(gen::Applies a) {
    return a == gen::Applies::Now ? Applies::Now : a == gen::Applies::WorldLoad ? Applies::WorldLoad : Applies::Restart;
}

}  // namespace schema_detail

// The qualified fields the generated schema has and ir.h cannot describe yet.
inline const std::vector<std::string>& unsupported_fields() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> out;
        for (const gen::Field& f : gen::kFields)
            if (f.kind == gen::Kind::List && f.element == gen::Kind::Struct) out.push_back(std::string(f.registry) + "." + f.path);
        return out;
    }();
    return v;
}

inline Schema generated_schema() {
    using namespace schema_detail;
    Schema s;
    for (const gen::Registry& r : gen::kRegistries) {
        RegistrySpec& reg = s.registries[r.name];
        reg.name = r.name;
        reg.add = r.add;
        reg.remove = r.remove[0] != 0;
        for (size_t i = r.first; i < r.first + r.count; ++i) {
            const gen::Field& f = gen::kFields[i];
            FieldSpec x;
            x.name = f.path;
            x.read_only = f.read_only;
            x.applies = applies_of(f.applies);
            if (f.has_min) x.min = f.min;
            if (f.has_max) x.max = f.max;
            x.max_items = f.max_items;
            x.enum_values = split_names(f.values);
            if (f.kind == gen::Kind::List) {
                if (!scalar_type(f.element, &x.element)) continue;  // a list of entries with fields: unsupported_fields()
                x.type = FieldType::List;
            } else if (f.kind == gen::Kind::Flags) {
                x.type = FieldType::List, x.element = FieldType::Enum;
                x.max_items = static_cast<size_t>(x.enum_values.size());
            } else if (!scalar_type(f.kind, &x.type)) {
                continue;
            }
            reg.fields[x.name] = std::move(x);
        }
    }
    return s;
}

}  // namespace kc::reg
