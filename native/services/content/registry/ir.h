// SPDX-License-Identifier: MIT
// kiln.content registry stage: the edit IR (registry P1 slice S3, plan: registry-p05 P1-PLAN.md section 4).
//
// One IR document is one batch of edits of one mod in one stage, the same from a data pack's "registry" section and from
// C# (Kiln.Registry serialises its calls into it):
//   {"ir": 1, "mod": "<id>", "modVersion": "<v>", "stage": "add|alter|final", "source": "<file or code:...>",
//    "edits": [ {"n": 0, "registry": "items", "select": {"id": "game:material_*", "where": {...}, "has": [...], "hasNot": [...]},
//                "field": "maxStack", "op": {"mul": 4, "max": 9999} | <plain value>, "exclusive": false, "yield": false},
//               {"registry": "recipes", "select": {...}, "field": "inputs", "list": {"append": [...]} },
//               {"registry": "items", "entry": "remove", "select": {"id": "game:x"}},
//               {"registry": "items", "entry": "add", "id": "<mod>:<name>", "like": "game:y", "fields": {"maxStack": 500}} ]}
// The op vocabulary and its evaluation are today's data packs' (packs.cpp parse_value): r = clamp(v * mul + add, min, max),
// then raiseOnly / lowerOnly against v, integer fields rounded. Validation is per edit and all or nothing: a malformed edit is
// refused with a reason, the rest of the batch stands. The schema descriptor below is what S1's generator emits
// (schema/registry/*.json); here it is filled by hand by the tests. The document's JSON Schema: schema/registry-ir.schema.json.
//
// The Advanced tier (owner decision D5): {"registry": "items", "select": {...}, "raw": {"damageSetup.dmgMod": 1.5}} names the
// game's own member paths of an entry, in a registry whose schema allows it ("raw": "allowed"). It works while the game's names
// and layout hold: each path is resolved per entry on this game build (S5's model) and an edit naming one that does not resolve
// there is refused, never written blind. The IR checks only the shape here (a member path, a plain value or an op); the type and
// range checks are the member's own, at composition (compose.h).
// Nothing here touches the game, a service table or a lock: plain data in, plain data out.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace kc::reg {

using json = nlohmann::json;

// ---- the schema descriptor (generated from schema/registry/*.json by S1) ----
enum class Applies : uint8_t { Now = 1, WorldLoad = 2, Restart = 3 };  // KILN_REGISTRY_APPLIES_*
enum class FieldType : uint8_t { Int, Float, Bool, Enum, Id, List };

struct FieldSpec {
    std::string name;  // the Kiln field path, as edits name it ("maxStack", "damage.fire", "inputs")
    FieldType type = FieldType::Int;
    FieldType element = FieldType::Int;  // a list's elements
    std::optional<double> min, max;      // the range of a number (of a list's numbers)
    size_t max_items = 0;                // a list: at most this many (0 = no limit)
    std::vector<std::string> enum_values;
    bool read_only = false;
    Applies applies = Applies::Restart;
};

struct RegistrySpec {
    std::string name;
    std::string version;  // the registry schema's "version" ("1.0.0"): registry.json "schema.registries"
    std::map<std::string, FieldSpec> fields;
    bool add = true, remove = true;
    bool raw = false;  // "raw": "allowed": edits may name the game's member paths (the Advanced tier, D5)
    const FieldSpec* field(const std::string& path) const {
        const auto it = fields.find(path);
        return it == fields.end() ? nullptr : &it->second;
    }
};

struct Schema {
    int format = 1;  // "registrySchema": the schema format (registry.json "schema.registrySchema")
    std::map<std::string, RegistrySpec> registries;
    const RegistrySpec* registry(const std::string& name) const {
        const auto it = registries.find(name);
        return it == registries.end() ? nullptr : &it->second;
    }
};

// ---- values ----
struct Value {
    enum Kind : uint8_t { None, Int, Num, Bool, Text, List } kind = None;
    int64_t i = 0;
    double d = 0;
    bool b = false;
    std::string t;  // an enum member's name, an id
    std::vector<Value> list;

    static Value integer(int64_t v) { Value x; x.kind = Int, x.i = v, x.d = static_cast<double>(v); return x; }
    static Value number(double v) { Value x; x.kind = Num, x.d = v; return x; }
    static Value boolean(bool v) { Value x; x.kind = Bool, x.b = v; return x; }
    static Value text(std::string v) { Value x; x.kind = Text, x.t = std::move(v); return x; }
    static Value of_list(std::vector<Value> v) { Value x; x.kind = List, x.list = std::move(v); return x; }
    bool numeric() const { return kind == Int || kind == Num; }
    double as_double() const { return kind == Int ? static_cast<double>(i) : kind == Num ? d : kind == Bool ? (b ? 1.0 : 0.0) : 0.0; }
    json to_json() const;
    std::string text_of() const { return to_json().dump(); }
    friend bool operator==(const Value& a, const Value& b);
};

// A value in JSON for a field (or a list's element): checked against its type and range. false + *err: why not.
bool value_of(const json& j, const FieldSpec& f, bool element, Value* out, std::string* err);

// ---- edits ----
enum class Stage : uint8_t { Add = 0, Alter = 1, Final = 2 };
const char* stage_name(Stage s);
bool stage_of(const std::string& s, Stage* out);

struct Op {
    bool set = false;  // an absolute value (a plain value, or {"set": v})
    Value value;
    std::optional<double> mul, add, min, max;
    bool raise_only = false, lower_only = false;
    bool relative() const { return !set; }
    json to_json() const;
};

enum class ListOp : uint8_t { None, Clear, Append, Remove, Replace };
enum class EntryOp : uint8_t { None, Add, Remove };

struct Selector {
    std::string id;  // an id or a case-insensitive glob over ids ("game:material_*"); "*" = every entry
    json where;      // {"<field>": "<cond>" | <value>}: every condition holds (on the view the stage gives)
    std::vector<std::string> has, has_not;  // traits
    bool exact() const { return id.find_first_of("*?") == std::string::npos; }
};

// One raw member path of an Advanced-tier edit (D5).
struct RawWrite {
    std::string path;  // as written ("damageSetup.dmgMod"), for messages and the report
    std::string key;   // raw_key(path): one spelling per place
    json op;           // a plain value or {set | mul | add | min | max | raiseOnly | lowerOnly}: typed per entry against the member
};

struct Edit {
    uint32_t n = 0;  // ordinal in its batch
    std::string registry;
    Selector select;
    std::string field;
    Op op;
    ListOp list = ListOp::None;
    std::vector<Value> list_values;
    EntryOp entry = EntryOp::None;
    std::string add_id, like;
    std::map<std::string, Value> add_fields;
    std::vector<RawWrite> raw;  // not empty: an Advanced-tier edit of raw member paths (then field, op and list are unset)
    bool exclusive = false, yield = false;
};

struct Batch {
    std::string mod, mod_version, source;
    Stage stage = Stage::Alter;
    std::vector<Edit> edits;
};

struct Refusal {
    std::string mod, source;
    Stage stage = Stage::Alter;
    uint32_t n = 0;
    std::string reason;
};

// One IR document. false: the document itself is unusable (*err: why; nothing of it is taken): not JSON, nested deeper than
// 32 levels, larger than 16 MiB, no usable ir / mod / stage / edits. true: *out holds every edit that passed validation; each
// refused edit is one row in *refused, named by its mod, source and ordinal ("n", else its position; an "n" that is not
// 0..4294967295 or is used twice refuses that edit). No exception leaves either form.
bool parse_batch(const std::string& text, const Schema& schema, Batch* out, std::vector<Refusal>* refused, std::string* err);
bool parse_batch(const json& doc, const Schema& schema, Batch* out, std::vector<Refusal>* refused, std::string* err);

// Today's numeric operation on a value (packs.cpp parse_value's order): the base for a relative op. Integer fields round.
// false + *why: the result is not finite, or past what a whole-number field holds (checked before any rounding).
bool op_result(const Op& op, const Value& base, const FieldSpec& f, Value* out, std::string* why);
// The same, total: a result op_result refuses leaves the value as it was (for back-ends re-evaluating a chain over another
// value; the stage itself refuses such an edit through op_result).
Value apply_op(const Op& op, const Value& base, const FieldSpec& f);

// A Kiln mod id ("^[a-z0-9][a-z0-9._-]{0,63}$", kiln.mod.schema.json) that is not reserved ("game", "kiln", ...): a batch of
// any other "mod" is refused whole, so no mod mints game: or kiln: ids. false + *err: why not.
bool mod_id_ok(const std::string& id, std::string* err);

// An edit's "op" for field f: a plain value (an absolute set), {"set": v}, or relative numbers {mul, add, min, max, raiseOnly,
// lowerOnly} (number fields only). false + *err: why not.
bool op_of(const json& j, const FieldSpec& f, Op* op, std::string* err);

// A raw member path ("damageSetup.dmgMod", "perkIds[2]", "a.b[03].c": member names [A-Za-z_][A-Za-z0-9_]*, indices [n]) into
// its key, one spelling per place: names lower-case (the game's reflection names are matched without case, as packs do),
// indices as plain numbers ("a.b[3].c"). No "*": a raw edit names exact places. false + *err: not such a path.
bool raw_key(const std::string& path, std::string* key, std::string* err);
// The field name a raw path's chain is kept under (compose.h FieldRef::field): "raw:" + its key. Schema fields have no ':'.
inline std::string raw_field(const std::string& key) { return "raw:" + key; }
inline bool is_raw_field(const std::string& field) { return field.rfind("raw:", 0) == 0; }

// Case-insensitive glob ('*' any run, '?' one character), as packs' selectors.
bool glob_ci(const std::string& pattern, const std::string& text);

}  // namespace kc::reg
