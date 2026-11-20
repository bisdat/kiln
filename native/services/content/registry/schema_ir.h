// SPDX-License-Identifier: MIT
// kiln.content registry stage: the generated schema (schema.g.h, tools/registrygen/registrygen.py) as the edit IR's
// descriptor (ir.h's Schema). One FieldSpec per generated row: a scalar leaf ("maxStack", "damage.fire"), a list ("perks",
// "inputs"), a leaf of a list's elements ("inputs[].count"); then link_elements() joins the leaves to their list (element
// Struct). Kinds map as: int -> Int, float and time (seconds) -> Float, bool -> Bool, enum -> Enum, flags -> Flags,
// ref -> Id (its registry in FieldSpec::ref, so references are checked), list -> List (its element's kind; Struct for
// elements with fields). The schema's optional refs, indexed lists of fixed length, each registry's version and raw flag
// and the schema format are carried over.
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

inline FieldType type_of(gen::Kind k) {
    switch (k) {
        case gen::Kind::Int: return FieldType::Int;
        case gen::Kind::Float:
        case gen::Kind::Time: return FieldType::Float;
        case gen::Kind::Bool: return FieldType::Bool;
        case gen::Kind::Enum: return FieldType::Enum;
        case gen::Kind::Flags: return FieldType::Flags;
        case gen::Kind::Ref: return FieldType::Id;
        case gen::Kind::List: return FieldType::List;
        case gen::Kind::Struct: return FieldType::Struct;
    }
    return FieldType::Int;
}

inline Applies applies_of(gen::Applies a) {
    return a == gen::Applies::Now ? Applies::Now : a == gen::Applies::WorldLoad ? Applies::WorldLoad : Applies::Restart;
}

}  // namespace schema_detail

inline Schema generated_schema() {
    using namespace schema_detail;
    Schema s;
    s.format = static_cast<int>(gen::kRegistrySchema);
    for (const gen::Registry& r : gen::kRegistries) {
        RegistrySpec& reg = s.registries[r.name];
        reg.name = r.name;
        reg.version = r.version;
        reg.add = r.add;
        reg.remove = r.remove[0] != 0;
        reg.raw = r.raw;
        for (size_t i = r.first; i < r.first + r.count; ++i) {
            const gen::Field& f = gen::kFields[i];
            FieldSpec x;
            x.name = f.path;
            x.type = type_of(f.kind);
            x.element = type_of(f.element);
            x.read_only = f.read_only;
            x.applies = applies_of(f.applies);
            if (f.has_min) x.min = f.min;
            if (f.has_max) x.max = f.max;
            x.max_items = f.max_items;
            x.enum_values = split_names(f.values);
            x.ref = f.ref;
            x.optional = f.optional;
            x.indexed = f.ops == gen::Ops::Indexed;
            x.fixed_length = f.fixed_length;
            if (f.kind == gen::Kind::Flags) x.max_items = x.enum_values.size();
            reg.fields[x.name] = std::move(x);
        }
    }
    link_elements(&s);
    return s;
}

}  // namespace kc::reg
