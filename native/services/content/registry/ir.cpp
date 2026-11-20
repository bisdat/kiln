// SPDX-License-Identifier: MIT
// kiln.content registry stage: the edit IR's reader and checks (ir.h).
#include "ir.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace kc::reg {

json Value::to_json() const {
    switch (kind) {
        case Int: return i;
        case Num: return d;
        case Bool: return b;
        case Text: return t;
        case List: {
            json a = json::array();
            for (const Value& v : list) a.push_back(v.to_json());
            return a;
        }
        default: return nullptr;
    }
}

bool operator==(const Value& a, const Value& b) {
    if (a.numeric() && b.numeric()) return a.as_double() == b.as_double();
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case Value::Bool: return a.b == b.b;
        case Value::Text: return a.t == b.t;
        case Value::List: return a.list == b.list;
        default: return true;
    }
}

const char* stage_name(Stage s) { return s == Stage::Add ? "add" : s == Stage::Alter ? "alter" : "final"; }
bool stage_of(const std::string& s, Stage* out) {
    if (s == "add") return *out = Stage::Add, true;
    if (s == "alter") return *out = Stage::Alter, true;
    if (s == "final") return *out = Stage::Final, true;
    return false;
}

json Op::to_json() const {
    if (set) return value.to_json();
    json o = json::object();
    if (mul) o["mul"] = *mul;
    if (add) o["add"] = *add;
    if (min) o["min"] = *min;
    if (max) o["max"] = *max;
    if (raise_only) o["raiseOnly"] = true;
    if (lower_only) o["lowerOnly"] = true;
    return o;
}

bool glob_ci(const std::string& p, const std::string& t) {
    size_t i = 0, j = 0, star = std::string::npos, back = 0;
    auto eq = [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
    while (j < t.size()) {
        if (i < p.size() && (p[i] == '?' || (p[i] != '*' && eq(p[i], t[j])))) {
            ++i, ++j;
        } else if (i < p.size() && p[i] == '*') {
            star = i++, back = j;
        } else if (star != std::string::npos) {
            i = star + 1, j = ++back;
        } else {
            return false;
        }
    }
    while (i < p.size() && p[i] == '*') ++i;
    return i == p.size();
}

namespace {

bool in_range(double v, const FieldSpec& f, std::string* err) {
    if ((f.min && v < *f.min) || (f.max && v > *f.max)) {
        json lo = f.min ? json(*f.min) : json("-"), hi = f.max ? json(*f.max) : json("-");
        return *err = f.name + ": " + json(v).dump() + " is outside " + lo.dump() + ".." + hi.dump(), false;
    }
    return true;
}

bool scalar_of(const json& j, FieldType type, const FieldSpec& f, Value* out, std::string* err) {
    switch (type) {
        case FieldType::Int:
            if (j.is_number_integer() || (j.is_number_float() && std::nearbyint(j.get<double>()) == j.get<double>())) {
                const double v = j.get<double>();
                if (!in_range(v, f, err)) return false;
                *out = Value::integer(j.is_number_integer() ? (j.is_number_unsigned() ? static_cast<int64_t>(j.get<uint64_t>()) : j.get<int64_t>())
                                                            : static_cast<int64_t>(v));
                return true;
            }
            return *err = f.name + ": a whole number is expected, not " + j.dump(), false;
        case FieldType::Float:
            if (!j.is_number()) return *err = f.name + ": a number is expected, not " + j.dump(), false;
            if (!in_range(j.get<double>(), f, err)) return false;
            *out = Value::number(j.get<double>());
            return true;
        case FieldType::Bool:
            if (!j.is_boolean()) return *err = f.name + ": true or false is expected, not " + j.dump(), false;
            *out = Value::boolean(j.get<bool>());
            return true;
        case FieldType::Enum: {
            if (!j.is_string()) return *err = f.name + ": one of the names is expected, not " + j.dump(), false;
            const std::string s = j.get<std::string>();
            const auto it = std::find_if(f.enum_values.begin(), f.enum_values.end(), [&](const std::string& e) { return glob_ci(e, s) && e.size() == s.size(); });
            if (it == f.enum_values.end()) {
                std::string names;
                for (const std::string& e : f.enum_values) names += (names.empty() ? "" : ", ") + e;
                return *err = f.name + ": \"" + s + "\" is not one of " + names, false;
            }
            *out = Value::text(*it);
            return true;
        }
        case FieldType::Id:
            if (!j.is_string() || j.get<std::string>().find(':') == std::string::npos)
                return *err = f.name + ": an id (\"game:<name>\" or \"<mod>:<name>\") is expected, not " + j.dump(), false;
            *out = Value::text(j.get<std::string>());
            return true;
        default:
            return *err = f.name + ": a list holds plain values", false;
    }
}

constexpr size_t kMaxRawPaths = 32;

bool scalar(const json& j) { return j.is_number() || j.is_boolean() || j.is_string(); }

// A raw path's value: its shape only (the member's type and range are checked per entry at composition).
bool raw_op_shape(const json& v, const std::string& path, std::string* err) {
    if (scalar(v)) return true;
    const std::string what = "raw path \"" + path + "\"";
    if (!v.is_object()) return *err = what + ": a plain value (a number, true or false, a name) or an op is expected, not " + v.dump(), false;
    if (v.contains("set")) {
        if (v.size() != 1) return *err = what + ": \"set\" stands alone", false;
        if (!scalar(v["set"])) return *err = what + ": \"set\" takes a plain value (a number, true or false, a name), not " + v["set"].dump(), false;
        return true;
    }
    FieldSpec any;  // relative ops: numbers whatever the member's type; the member says at composition whether it is a number
    any.name = what, any.type = FieldType::Float;
    Op op;
    return op_of(v, any, &op, err);
}

bool selector_of(const json& j, Selector* s, std::string* err) {
    *s = Selector{};
    if (j.is_string()) return s->id = j.get<std::string>(), !s->id.empty() || (*err = "an empty selector", false);
    if (!j.is_object() || !j.contains("id") || !j["id"].is_string() || j["id"].get<std::string>().empty())
        return *err = "\"select\" is {\"id\": \"<id or glob>\", \"where\": {...}, \"has\": [...], \"hasNot\": [...]}", false;
    for (const auto& [k, v] : j.items())
        if (k != "id" && k != "where" && k != "has" && k != "hasNot") return *err = "\"select\": unknown member \"" + k + "\"", false;
    s->id = j["id"].get<std::string>();
    if (j.contains("where")) {
        if (!j["where"].is_object()) return *err = "\"where\" is {\"<field>\": <condition>}", false;
        s->where = j["where"];
    }
    for (const char* k : {"has", "hasNot"}) {
        if (!j.contains(k)) continue;
        if (!j[k].is_array()) return *err = std::string("\"") + k + "\" is a list of trait names", false;
        for (const json& t : j[k]) {
            if (!t.is_string()) return *err = std::string("\"") + k + "\" is a list of trait names", false;
            (k[3] == 'N' ? s->has_not : s->has).push_back(t.get<std::string>());
        }
    }
    return true;
}

bool edit_of(const json& e, const Batch& b, const Schema& schema, Edit* out, std::string* err) {
    if (!e.is_object()) return *err = "an edit is an object", false;
    static const char* const known[] = {"n", "registry", "select", "field", "op", "list", "entry", "id", "like", "fields", "exclusive", "yield", "raw"};
    for (const auto& [k, v] : e.items())
        if (std::none_of(std::begin(known), std::end(known), [&](const char* x) { return k == x; })) return *err = "unknown member \"" + k + "\"", false;
    if (!e.contains("registry") || !e["registry"].is_string()) return *err = "\"registry\" names the registry", false;
    out->registry = e["registry"].get<std::string>();
    const RegistrySpec* reg = schema.registry(out->registry);
    if (!reg) return *err = "no registry \"" + out->registry + "\"", false;
    for (const char* k : {"exclusive", "yield"})
        if (e.contains(k) && !e[k].is_boolean()) return *err = std::string("\"") + k + "\" is true or false", false;
    out->exclusive = e.value("exclusive", false);
    out->yield = e.value("yield", false);
    if (e.contains("raw")) {  // the Advanced tier (D5): raw game member paths
        for (const char* k : {"field", "op", "list", "entry", "id", "like", "fields"})
            if (e.contains(k)) return *err = std::string("\"raw\" (game member paths) and \"") + k + "\" in one edit: an edit has one or the other", false;
        if (!reg->raw)
            return *err = out->registry + ": raw game paths (the Advanced tier) are not allowed in this registry: its schema does not allow them; use its Kiln fields", false;
        if (b.stage == Stage::Add) return *err = "stage add adds entries; field edits, raw ones too, belong in alter or final", false;
        if (!e.contains("select")) return *err = "\"select\" names the entries a raw edit applies to", false;
        if (!selector_of(e["select"], &out->select, err)) return false;
        const json& r = e["raw"];
        if (!r.is_object()) return *err = "\"raw\" is {\"<game member path>\": <value or op>}", false;
        if (r.empty()) return *err = "\"raw\" names at least one game member path", false;
        if (r.size() > kMaxRawPaths) return *err = "\"raw\" names at most " + std::to_string(kMaxRawPaths) + " game member paths in one edit", false;
        for (const auto& [p, v] : r.items()) {
            RawWrite w;
            w.path = p;
            if (!raw_key(p, &w.key, err)) return false;
            for (const RawWrite& o : out->raw)
                if (o.key == w.key) return *err = "raw paths \"" + o.path + "\" and \"" + p + "\" name one place", false;
            if (!raw_op_shape(v, p, err)) return false;
            w.op = v;
            out->raw.push_back(std::move(w));
        }
        return true;
    }
    if (e.contains("entry")) {
        const std::string what = e["entry"].is_string() ? e["entry"].get<std::string>() : "";
        if (what == "add") {
            out->entry = EntryOp::Add;
            if (!reg->add) return *err = out->registry + ": entries are not added to this registry", false;
            if (b.stage != Stage::Add) return *err = "an entry is added in stage add", false;
            if (!e.contains("id") || !e["id"].is_string()) return *err = "an added entry has an \"id\" (\"" + b.mod + ":<name>\")", false;
            out->add_id = e["id"].get<std::string>();
            const std::string prefix = b.mod + ":";
            if (out->add_id.size() <= prefix.size() || out->add_id.compare(0, prefix.size(), prefix) != 0 ||
                out->add_id.find_first_of("*?", prefix.size()) != std::string::npos)
                return *err = "an added entry's id is \"" + prefix + "<name>\", not \"" + out->add_id + "\"", false;
            if (e.contains("like")) {
                if (!e["like"].is_string()) return *err = "\"like\" is an id", false;
                out->like = e["like"].get<std::string>();
            }
            if (e.contains("fields")) {
                if (!e["fields"].is_object()) return *err = "\"fields\" is {\"<field>\": <value>}", false;
                for (const auto& [k, v] : e["fields"].items()) {
                    const FieldSpec* f = reg->field(k);
                    if (!f) return *err = out->registry + " has no field \"" + k + "\"", false;
                    if (f->read_only) return *err = out->registry + "." + k + " is read-only", false;
                    Value x;
                    if (!value_of(v, *f, false, &x, err)) return false;
                    out->add_fields[k] = x;
                }
            }
            return true;
        }
        if (what != "remove") return *err = "\"entry\" is \"add\" or \"remove\"", false;
        out->entry = EntryOp::Remove;
        if (!reg->remove) return *err = out->registry + ": entries are not removed from this registry", false;
        if (b.stage == Stage::Add) return *err = "an entry is removed in stage alter or final", false;
        return e.contains("select") ? selector_of(e["select"], &out->select, err) : (*err = "\"select\" names what is removed", false);
    }
    if (b.stage == Stage::Add) return *err = "stage add adds entries (\"entry\": \"add\"); field edits belong in alter or final", false;
    if (!e.contains("select") || !selector_of(e["select"], &out->select, err)) return err->empty() ? (*err = "\"select\" names the entries", false) : false;
    if (!e.contains("field") || !e["field"].is_string()) return *err = "\"field\" names the field", false;
    out->field = e["field"].get<std::string>();
    const FieldSpec* f = reg->field(out->field);
    if (!f) return *err = out->registry + " has no field \"" + out->field + "\"", false;
    if (f->read_only) return *err = out->registry + "." + out->field + " is read-only", false;
    if (e.contains("op") == e.contains("list")) return *err = "an edit has \"op\" or \"list\"", false;
    if (e.contains("op")) {
        if (f->type == FieldType::List) return *err = out->registry + "." + out->field + " is a list: use \"list\" (clear, append, remove, replace)", false;
        return op_of(e["op"], *f, &out->op, err);
    }
    if (f->type != FieldType::List) return *err = out->registry + "." + out->field + " is not a list", false;
    const json& l = e["list"];
    if (!l.is_object() || l.size() != 1) return *err = "\"list\" is one of {\"clear\": true}, {\"append\": [...]}, {\"remove\": [...]}, {\"replace\": [...]}", false;
    const std::string k = l.begin().key();
    const json& v = l.begin().value();
    if (k == "clear") {
        if (v != true) return *err = "{\"clear\": true}", false;
        out->list = ListOp::Clear;
        return true;
    }
    out->list = k == "append" ? ListOp::Append : k == "remove" ? ListOp::Remove : k == "replace" ? ListOp::Replace : ListOp::None;
    if (out->list == ListOp::None) return *err = "unknown list operation \"" + k + "\" (clear, append, remove, replace)", false;
    if (!v.is_array() || (v.empty() && out->list != ListOp::Replace)) return *err = "\"" + k + "\" takes a list of values", false;
    for (const json& x : v) {
        Value y;
        if (!value_of(x, *f, true, &y, err)) return false;
        out->list_values.push_back(y);
    }
    if (out->list == ListOp::Replace && f->max_items && out->list_values.size() > f->max_items)
        return *err = out->registry + "." + out->field + " holds at most " + std::to_string(f->max_items) + " values", false;
    return true;
}

}  // namespace

bool value_of(const json& j, const FieldSpec& f, bool element, Value* out, std::string* err) {
    if (f.type == FieldType::List && !element) {
        if (!j.is_array()) return *err = f.name + ": a list is expected, not " + j.dump(), false;
        if (f.max_items && j.size() > f.max_items) return *err = f.name + ": at most " + std::to_string(f.max_items) + " values", false;
        std::vector<Value> v;
        for (const json& x : j) {
            Value y;
            if (!scalar_of(x, f.element, f, &y, err)) return false;
            v.push_back(std::move(y));
        }
        *out = Value::of_list(std::move(v));
        return true;
    }
    return scalar_of(j, element ? f.element : f.type, f, out, err);
}

bool op_of(const json& j, const FieldSpec& f, Op* op, std::string* err) {
    *op = Op{};
    if (!j.is_object()) {
        op->set = true;
        return value_of(j, f, false, &op->value, err);
    }
    if (j.contains("set")) {
        if (j.size() != 1) return *err = f.name + ": \"set\" stands alone", false;
        op->set = true;
        return value_of(j["set"], f, false, &op->value, err);
    }
    if (f.type != FieldType::Int && f.type != FieldType::Float)
        return *err = f.name + ": {mul, add, min, max} works on numbers; this field takes a plain value", false;
    if (j.empty()) return *err = f.name + ": an empty operation", false;
    for (const auto& [k, v] : j.items()) {
        if (k == "mul" || k == "add" || k == "min" || k == "max") {
            if (!v.is_number()) return *err = f.name + ": \"" + k + "\" takes a number", false;
            (k == "mul" ? op->mul : k == "add" ? op->add : k == "min" ? op->min : op->max) = v.get<double>();
        } else if (k == "raiseOnly" || k == "lowerOnly") {
            if (!v.is_boolean()) return *err = f.name + ": \"" + k + "\" takes true or false", false;
            (k == "raiseOnly" ? op->raise_only : op->lower_only) = v.get<bool>();
        } else {
            return *err = f.name + ": unknown operation \"" + k + "\" (set, mul, add, min, max, raiseOnly, lowerOnly)", false;
        }
    }
    if (op->min && op->max && *op->min > *op->max) return *err = f.name + ": min must not be greater than max", false;
    if (op->raise_only && op->lower_only) return *err = f.name + ": raiseOnly and lowerOnly together change nothing", false;
    return true;
}

bool raw_key(const std::string& path, std::string* key, std::string* err) {
    auto bad = [&](const std::string& why) { return *err = "raw path \"" + path + "\": " + why, false; };
    if (path.empty()) return bad("empty");
    if (path.size() > 256) return bad("longer than 256 characters");
    if (path.find('*') != std::string::npos) return bad("'*' is not allowed: a raw edit names exact places");
    auto letter = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; };
    auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    std::string k;
    size_t i = 0;
    for (;;) {
        if (i >= path.size() || !letter(path[i])) return bad("a member name starts with a letter or '_' (members are a.b, indices [n])");
        while (i < path.size() && word(path[i])) k += static_cast<char>(std::tolower(static_cast<unsigned char>(path[i++])));
        while (i < path.size() && path[i] == '[') {
            const size_t j = ++i;
            while (i < path.size() && std::isdigit(static_cast<unsigned char>(path[i]))) ++i;
            if (i == j || i >= path.size() || path[i] != ']') return bad("an index is [n], n a whole number");
            if (i - j > 9) return bad("an index has at most 9 digits");
            const size_t z = path.find_first_not_of('0', j);
            k += "[" + (z >= i ? std::string("0") : path.substr(z, i - z)) + "]";
            ++i;
        }
        if (i == path.size()) break;
        if (path[i] != '.') return bad("members are separated by '.'");
        k += '.';
        ++i;
    }
    *key = std::move(k);
    return true;
}

Value apply_op(const Op& op, const Value& base, const FieldSpec& f) {
    if (op.set) return op.value;
    const double v = base.as_double();
    double r = v * op.mul.value_or(1.0) + op.add.value_or(0.0);
    r = std::clamp(r, op.min.value_or(-INFINITY), op.max.value_or(INFINITY));
    if (op.raise_only) r = std::max(r, v);
    if (op.lower_only) r = std::min(r, v);
    return f.type == FieldType::Int ? Value::integer(static_cast<int64_t>(std::llround(r))) : Value::number(r);
}

bool parse_batch(const std::string& text, const Schema& schema, Batch* out, std::vector<Refusal>* refused, std::string* err) {
    const json doc = json::parse(text, nullptr, false, true);
    if (doc.is_discarded()) return *err = "the IR document is not JSON", false;
    return parse_batch(doc, schema, out, refused, err);
}

bool parse_batch(const json& doc, const Schema& schema, Batch* out, std::vector<Refusal>* refused, std::string* err) {
    *out = Batch{};
    if (!doc.is_object()) return *err = "the IR document is an object", false;
    if (!doc.contains("ir") || doc["ir"] != 1) return *err = "\"ir\": 1 is the only IR format this Kiln reads", false;
    if (!doc.contains("mod") || !doc["mod"].is_string() || doc["mod"].get<std::string>().empty()) return *err = "\"mod\" names the mod", false;
    if (!doc.contains("stage") || !doc["stage"].is_string() || !stage_of(doc["stage"].get<std::string>(), &out->stage))
        return *err = "\"stage\" is add, alter or final", false;
    if (!doc.contains("edits") || !doc["edits"].is_array()) return *err = "\"edits\" is a list", false;
    out->mod = doc["mod"].get<std::string>();
    out->mod_version = doc.value("modVersion", "");
    out->source = doc.value("source", "");
    uint32_t n = 0;
    for (const json& e : doc["edits"]) {
        Edit x;
        x.n = e.is_object() && e.contains("n") && e["n"].is_number_unsigned() ? e["n"].get<uint32_t>() : n;
        ++n;
        std::string why;
        if (edit_of(e, *out, schema, &x, &why)) out->edits.push_back(std::move(x));
        else refused->push_back({out->mod, out->source, out->stage, x.n, why});
    }
    return true;
}

}  // namespace kc::reg
