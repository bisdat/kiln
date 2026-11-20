// SPDX-License-Identifier: MIT
// kiln.content registry stage: composition and conflicts (registry P1 slice S4; P1-PLAN.md section 5).
//
// compose() runs one start's stage over Kiln fields: the base (the archive as the game will read it, decoded through the
// regmap by S5's model), the writes of Kiln's older paths and named archive-writer packages below the registry (D1), then
// every mod's add, every mod's alter, every mod's final, mods in the runtime load order. The rules (the owner's public
// commitments, BRIEF section 12):
//   - add, alter and final each run across all mods before the next stage starts;
//   - later in load order wins and is reported ("overridden"); relative ops stack ("composed": x4 then x2 = x8);
//   - "exclusive": an edit of another writer on the same field is a conflict and the later edit is refused whole;
//   - "yield": the edit applies only where no earlier writer set the field;
//   - an edit is all or nothing over its selection (a refusal names the mod, its source and the edit's ordinal);
//   - final: every mod's final reads the same frozen post-alter view (plus its own final edits); its edits are held back
//     and composed after every final ran, in load order, with alter's rules;
//   - old paths are never refused because of a registry edit.
// The result is deterministic: it depends on the inputs and their order only. Plain data in, plain data out: no game
// memory, no service table, no lock, no clock.
#pragma once
#include <functional>
#include <set>

#include "ir.h"

namespace kc::reg {

struct EntryRef {
    std::string registry, id;
    auto operator<=>(const EntryRef&) const = default;
};
struct FieldRef {
    std::string registry, id, field;
    auto operator<=>(const FieldRef&) const = default;
    std::string text() const { return registry + "[" + id + "]." + field; }
};

// The base as Kiln fields (S5's model fills it from the base view; tests by hand).
struct BaseModel {
    struct Entry {
        std::string id;
        std::map<std::string, Value> fields;
        std::set<std::string> traits;
        std::string removed_in;  // not empty: the alias ledger says this game build removed it
    };
    std::map<std::string, std::vector<Entry>> registries;  // entries in the registry's own order
    std::set<std::string> retired;                          // "<registry>|<id>" the ids ledger retired: never added again
};

enum class WriterKind : uint8_t { ArchiveWriter, OldPatch, OldPack, Code, Registry };
const char* writer_kind_name(WriterKind k);

// A write below the registry: an old Kiln path, or an archive-writer package the journal names (D1).
struct OldWrite {
    WriterKind kind = WriterKind::OldPatch;
    std::string mod;  // the Kiln mod behind it (empty for an archive-writer package)
    std::string who;  // "<mod>/<key>", "<mod>: content/<file>", "shroudforge:<package>"
    FieldRef field;
    Op op;
};

struct ChainRow {
    WriterKind kind = WriterKind::Registry;
    std::string mod;  // the Kiln mod (empty: an archive-writer package)
    std::string who;  // what the report names: the mod, or the old writer
    Stage stage = Stage::Alter;
    std::string source;
    uint32_t n = 0;
    Op op;
    ListOp list = ListOp::None;
    std::vector<Value> list_values;
    bool exclusive = false;
    Value after;  // the field's value after this row
};

struct FieldOutcome {
    Value base;
    std::vector<ChainRow> chain;
    Value result;
    std::string winner;  // the writer whose row decided the result last (empty: the base)
};

enum class LineKind : uint8_t { Overridden, Composed, ComposedOn, Exclusive, Dropped, Yielded };
const char* line_kind_name(LineKind k);
struct Line {
    LineKind kind = LineKind::Overridden;
    FieldRef field;
    std::string winner, loser, text;
};

class View {
public:
    virtual ~View() = default;
    virtual bool exists(const std::string& registry, const std::string& id) const = 0;  // present and not removed
    virtual std::optional<Value> get(const FieldRef& f) const = 0;
    virtual std::vector<std::string> ids(const std::string& registry) const = 0;  // present entries, in order
    virtual bool has_trait(const std::string& registry, const std::string& id, const std::string& trait) const = 0;
};

// One mod's part of the stage. Its batches (data files by name, then edits recorded from code) run in the order given, at
// their stage; then code(stage, view, &batch), if set, adds the edits its callbacks make (C# OnRegistry through Kiln.Host).
struct ModInput {
    std::string id;
    std::vector<Batch> batches;
    std::function<void(Stage, const View&, Batch*)> code;
};

struct Result {
    std::map<FieldRef, FieldOutcome> fields;  // every touched field
    std::vector<Line> lines;                  // in the order they happened
    std::vector<Refusal> refused;
    std::vector<EntryRef> added, removed;
};

Result compose(const Schema& schema, const BaseModel& base, const std::vector<OldWrite>& old, const std::vector<ModInput>& mods);

// The registry rows of a chain over another value below them (the live back-end: a pack's value under a relative layer).
Value evaluate(const std::vector<ChainRow>& rows, const Value& below, const FieldSpec& f);

}  // namespace kc::reg
