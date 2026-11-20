// SPDX-License-Identifier: MIT
// kiln.content registry stage: registry.json (report.h).
#include "report.h"

#include <set>

namespace kc::reg {

namespace {

const char* applies_name(Applies a) { return a == Applies::Now ? "now" : a == Applies::WorldLoad ? "worldLoad" : "restart"; }

json list_op_json(const ChainRow& r) {
    json values = json::array();
    for (const Value& v : r.list_values) values.push_back(v.to_json());
    switch (r.list) {
        case ListOp::Clear: return {{"clear", true}};
        case ListOp::Append: return {{"append", values}};
        case ListOp::Remove: return {{"remove", values}};
        default: return {{"replace", values}};
    }
}

json writer_json(const ChainRow& r) {
    json w = {{"kind", writer_kind_name(r.kind)}};
    if (!r.mod.empty()) w["mod"] = r.mod;
    if (r.who != r.mod) w["who"] = r.who;
    if (r.kind == WriterKind::Registry) {
        w["stage"] = stage_name(r.stage);
        if (!r.source.empty()) w["source"] = r.source;
        w["n"] = r.n;
        if (r.exclusive) w["exclusive"] = true;
    }
    if (!r.raw.empty()) w["raw"] = r.raw;
    if (r.element.on)
        w["element"] = r.element.list + "[" + (r.element.all ? std::string("*") : std::to_string(r.element.index)) + "]" +
                       (r.element.leaf.empty() ? "" : "." + r.element.leaf);
    if (r.list == ListOp::None) w["op"] = r.op.to_json();
    else w["list"] = list_op_json(r);
    w["value"] = r.after.to_json();
    w["certainty"] = r.kind == WriterKind::ArchiveWriter ? "named" : "certain";
    return w;
}

// What a field row and a conflict line call a place: the Kiln field, or the raw path as written.
std::string field_name(const FieldRef& ref, const FieldOutcome* fo) {
    if (!is_raw_field(ref.field)) return ref.field;
    return fo && !fo->raw.empty() ? fo->raw : ref.field.substr(4);
}

}  // namespace

json report_json(const Schema& schema, const Result& r, const ReportMeta& meta, const ReportCaps& caps) {
    json doc = {{"format", 1}, {"pid", meta.pid}, {"written", meta.written}};
    if (!meta.kiln.empty()) doc["kiln"] = meta.kiln;
    doc["generation"] = meta.generation;
    doc["phase"] = meta.world_load ? "worldLoad" : "start";
    json versions = json::object();
    for (const auto& [name, reg] : schema.registries) versions[name] = reg.version;
    doc["schema"] = {{"registrySchema", schema.format}, {"registries", versions}};
    doc["regmap"] = {{"side", meta.regmap.side}, {"exeSha256", meta.regmap.exe_sha256}, {"verified", meta.regmap.verified}};
    doc["regmap"]["record"] = meta.regmap.record.empty() ? json(nullptr) : json(meta.regmap.record);
    doc["base"] = {{"origin", meta.base.origin}, {"writer", meta.base.writer}};
    if (!meta.base.token.empty()) doc["base"]["token"] = meta.base.token;
    json order = json::array();
    for (const ReportMeta::Order& o : meta.order) {
        json row = {{"mod", o.mod}, {"version", o.version}};
        if (!o.edits_hash.empty()) row["editsHash"] = o.edits_hash;
        order.push_back(std::move(row));
    }
    doc["order"] = std::move(order);
    doc["orderSource"] = meta.order_source;
    if (meta.plan.hash.empty()) {
        doc["plan"] = nullptr;
    } else {
        doc["plan"] = {{"hash", meta.plan.hash}, {"file", meta.plan.file}};
        doc["plan"]["previous"] = meta.plan.previous.empty() ? json(nullptr) : json(meta.plan.previous);
    }

    std::set<EntryRef> added(r.added.begin(), r.added.end());
    json fields = json::array();
    for (const auto& [ref, fo] : r.fields) {
        if (fields.size() >= caps.fields) break;
        const RegistrySpec* reg = schema.registry(ref.registry);
        const FieldSpec* spec = is_raw_field(ref.field) ? (fo.raw_spec ? &*fo.raw_spec : nullptr) : (reg ? reg->field(ref.field) : nullptr);
        json row = {{"registry", ref.registry}, {"id", ref.id}, {"field", field_name(ref, &fo)}};
        if (is_raw_field(ref.field)) row["raw"] = true;
        row["applies"] = applies_name(spec ? spec->applies : Applies::Restart);
        const std::string origin = added.count({ref.registry, ref.id}) ? "added" : meta.origin ? meta.origin(ref) : "archive";
        row["base"] = {{"value", fo.base.to_json()}, {"origin", origin}};
        json writers = json::array();
        for (const ChainRow& c : fo.chain) writers.push_back(writer_json(c));
        row["writers"] = std::move(writers);
        row["result"] = fo.result.to_json();
        row["winner"] = fo.winner.empty() ? json(nullptr) : json(fo.winner);
        fields.push_back(std::move(row));
    }
    doc["fields"] = std::move(fields);

    json conflicts = json::array();
    for (const Line& l : r.lines) {
        if (conflicts.size() >= caps.conflicts) break;
        const auto fo = r.fields.find(l.field);
        json row = {{"field", l.field.registry + "[" + l.field.id + "]." + field_name(l.field, fo == r.fields.end() ? nullptr : &fo->second)},
                    {"registry", l.field.registry},
                    {"id", l.field.id},
                    {"kind", line_kind_name(l.kind)}};
        row["winner"] = l.winner.empty() ? json(nullptr) : json(l.winner);
        row["loser"] = l.loser.empty() ? json(nullptr) : json(l.loser);
        row["certainty"] = certainty_name(l.certainty);
        row["text"] = l.text;
        conflicts.push_back(std::move(row));
    }
    doc["conflicts"] = std::move(conflicts);

    json refused = json::array();
    for (const Refusal& x : r.refused) {
        if (refused.size() >= caps.refused) break;
        refused.push_back({{"mod", x.mod}, {"source", x.source}, {"stage", stage_name(x.stage)}, {"n", x.n}, {"reason", x.reason}});
    }
    doc["refused"] = std::move(refused);

    auto entries = [&](const std::vector<EntryRef>& list) {
        json a = json::array();
        for (const EntryRef& e : list) {
            if (a.size() >= caps.entries) break;
            a.push_back({{"registry", e.registry}, {"id", e.id}});
        }
        return a;
    };
    doc["added"] = entries(r.added);
    doc["removed"] = entries(r.removed);

    doc["counts"] = {{"fields", r.fields.size()}, {"conflicts", r.lines.size()}, {"refused", r.refused.size()}, {"added", r.added.size()}, {"removed", r.removed.size()}};
    doc["truncated"] = {{"fields", r.fields.size() - doc["fields"].size()},
                        {"conflicts", r.lines.size() - doc["conflicts"].size()},
                        {"refused", r.refused.size() - doc["refused"].size()},
                        {"added", r.added.size() - doc["added"].size()},
                        {"removed", r.removed.size() - doc["removed"].size()}};
    return doc;
}

std::vector<std::string> report_log(const Result& r, const ReportCaps& caps) {
    std::vector<std::string> out;
    const size_t all = r.lines.size() + r.refused.size();
    if (all == 0) return out;
    for (const Line& l : r.lines) {
        if (out.size() >= caps.log) break;
        out.push_back("registry: " + std::string(line_kind_name(l.kind)) + ": " + l.text);
    }
    for (const Refusal& x : r.refused) {
        if (out.size() >= caps.log) break;
        out.push_back("registry: refused " + x.mod + " " + (x.source.empty() ? std::string("(no source)") : x.source) + " #" + std::to_string(x.n) + " (" +
                      stage_name(x.stage) + "): " + x.reason);
    }
    const size_t left = all - out.size();
    std::string total = "registry: " + std::to_string(r.lines.size()) + (r.lines.size() == 1 ? " conflict, " : " conflicts, ") + std::to_string(r.refused.size()) +
                        (r.refused.size() == 1 ? " refused edit" : " refused edits");
    total += left ? "; " + std::to_string(left) + " not in kiln.log: state/registry.json has them all" : "; state/registry.json has them all";
    out.push_back(std::move(total));
    return out;
}

}  // namespace kc::reg
