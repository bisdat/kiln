// SPDX-License-Identifier: MIT
// kiln.content registry stage: the stage runner (registry P1 slice S11; P1-PLAN.md sections 1, 5.1, 6 and 8).
//
// Once per start, on the stage thread (the game's main thread at kiln.assets' resources-ready, before the game reads any
// resource Kiln changes): the mods' inputs (their data files' "registry" sections, the batches they submitted before the
// stage, their stage callbacks) in Kiln.Host's runtime load order; every edit of a field this game build has not mapped and
// proven refused per edit (regmap.h mapping); the base read through the model (model.h) and let go as soon as it is decoded;
// compose (compose.h); the boot back-end's writes handed to the resource patches (gamedata_bootpatch.cpp) and the live part
// to the live back-end; registry.json (report.h) written atomically, kiln.log's lines; phase START and the ready callbacks.
// The model's id table is written into kiln.content's data folder (never the repository), and the decoded base is cached
// there under a key of the base view's digest, the decoder's version, Kiln's version and the regmap record: a later start
// whose base reads the same bytes takes it from the cache and gives the same registry.json.
//
// The Runner holds no game state of its own: the environment (Env) gives it the base source, its files and its log, so the
// host tests run it as kiln-content.dll does (stage_service.cpp binds it to the tables, kiln.assets and the files).
// Threads: every member takes the runner's lock; no lock is held while a mod's callback runs.
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "compose.h"
#include "model.h"
#include "regmap.h"
#include "report.h"

namespace kc::registry::stage {

enum class Phase : uint32_t { None = 0, Add = 1, Alter = 2, Final = 3, Start = 4, WorldLoad = 5 };  // KILN_REGISTRY_PHASE_*
enum class Mode : uint32_t { ResourcesReady = 0, OnBoot = 1 };                                       // KILN_REGISTRY_MODE_*

struct Env {
    // The base of this start (the live bytes at resources-ready; in mode OnBoot a tree view). null + *why: none.
    std::function<std::unique_ptr<model::Source>(std::string* why)> source;
    // The regmap of the running build (regmap.h load_dir over <game>/kiln/regmap); *errs: rows loaded as unproven.
    std::function<bool(map::RegMap* out, std::vector<std::string>* errs)> regmap;
    // A mod's data sections: one row per pack file in load order (the files of mods the order does not name come after).
    struct DataFile {
        std::string mod, version, rel;  // rel: "content/<file>.json"
        std::vector<Batch> batches;
        std::vector<Refusal> refused;
        std::string error;  // the section is unusable (nothing of it is taken)
    };
    std::function<std::vector<DataFile>(const Schema&)> data;
    // The mod folders' order (without .NET: orderSource "folders"): ids with versions.
    std::function<std::vector<std::pair<std::string, std::string>>()> folders;
    // Files: registry.json under state; the id table, the plans and the cache under kiln.content's data folder (rel paths).
    std::function<bool(const std::string& text)> write_report;
    std::function<bool(const std::string& rel, const std::string& text)> write_data;
    std::function<std::optional<std::string>(const std::string& rel)> read_data;
    std::function<void(int level, const std::string& line)> log;  // 0 info, 1 warn, 2 error
    std::function<std::string()> now;                              // RFC 3339 UTC
    uint32_t pid = 0;
    std::string kiln_version, side, exe_sha256;
    std::string writer = "none";  // the archive writer of this start (kiln-eml, shroudforge-hosted, external, none)
};

// The boot back-end's part of one resource: the registry rows of each boot-routed field, evaluated over the value the old
// documents leave there and written after them, in the same copy (gamedata_bootpatch.cpp build).
struct BootField {
    FieldRef field;
    const map::RegistryMap* reg = nullptr;
    const map::Row* row = nullptr;
    FieldSpec spec;
    std::vector<ChainRow> rows;  // the registry's rows of the chain, in order
};
struct BootResource {
    KilnAssetKey key{};
    std::string type;
    std::vector<BootField> fields;
};
struct BootOutcome {
    FieldRef field;
    bool ok = false;
    Value written;     // the value now in the resource
    std::string why;   // not written: why (each of its registry rows is refused with it)
};

// What the live back-end takes (lane D: store layers at world load, registry_live.cpp).
struct LiveHandoff {
    const Schema* schema = nullptr;
    const map::RegMap* regmap = nullptr;
    const Result* result = nullptr;
    const std::map<std::string, std::string>* ids = nullptr;  // "<registry>|<kiln id>" -> GUID text
    const std::map<std::string, uint32_t>* nums = nullptr;    // the same key -> the game's 32-bit id
    const BaseModel* base = nullptr;
    const std::vector<OldWrite>* old = nullptr;
    const std::vector<ModInput>* inputs = nullptr;  // code batches recorded, code unset
    const std::set<std::string>* decoded = nullptr; // "<registry>.<field>" the base holds values for
    std::function<void(const Result&)> report;      // registry.json again (generation + 1), after a recompose
};

class Runner {
   public:
    explicit Runner(Env env);
    ~Runner();

    // ---- before the stage (kiln.internal.registry, kiln.registry) ----
    KilnResult set_order(const std::string& order_json);
    KilnResult set_mode(uint32_t mode);
    Mode mode() const { return mode_.load(); }
    // A stage callback of mod's (one per mod): it submits its edits from inside; false: it faulted (the edits it submitted in
    // that turn are dropped, the subscription removed, reported).
    using StageFn = std::function<bool(Stage, const View&)>;
    KilnResult subscribe_stage(uint64_t owner, const std::string& mod, StageFn fn, uint64_t* handle);
    KilnResult subscribe_ready(uint64_t owner, std::function<void(Phase, uint64_t)> fn, uint64_t* handle);
    KilnResult unsubscribe(uint64_t handle);
    // One IR document. Before the stage: queued for its mod; inside that mod's stage callback (stage thread, the running
    // stage): taken into its batch. report: {"accepted", "refused": [{"n", "reason"}]}.
    KilnResult submit(uint64_t owner, const std::string& owner_mod, const std::string& ir, uint32_t* accepted, uint32_t* refused, std::string* report);
    void owner_gone(uint64_t owner);

    // ---- the stage ----
    using OldWritesFn = std::function<std::vector<OldWrite>(model::Model&)>;
    // Reads the base, composes, and returns the boot back-end's resources. The caller writes them (with the old documents)
    // and then calls finish() with what each field became. old(): the old paths' writes below the registry (boot patches).
    std::vector<BootResource> run(const OldWritesFn& old);
    void finish(const std::vector<BootOutcome>& boot);
    void set_live(std::function<void(const LiveHandoff&)> install) { live_ = std::move(install); }

    // ---- reads (any thread) ----
    Phase phase() const { return phase_.load(); }
    uint64_t generation() const;
    struct Counts {
        uint32_t edits = 0, refused = 0, conflicts = 0;
        bool failed = false;
    };
    Counts counts() const;
    std::string schema_json();
    // A field's mapping on this build: applies, mapping (KILN_REGISTRY_MAPPED..REMOVED), type, read-only, reason.
    struct FieldInfo {
        uint32_t applies = 0, mapping = 0, type = 0;
        bool read_only = false;
        std::string reason;
    };
    bool field_info(const std::string& qualified, FieldInfo* out);
    // view 0 base, 1 stage (inside the caller's callback only), 2 resolved. Results: KILN_OK, NOT_FOUND, UNAVAILABLE, WRONG_THREAD.
    KilnResult read(uint32_t view, const std::string& registry, const std::string& id, const std::string& field, std::string* out);
    KilnResult select(uint32_t view, const std::string& registry, const std::string& selector, std::string* out);
    KilnResult report_json(std::string* out);
    bool on_stage_thread() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> d_;
    std::atomic<Phase> phase_{Phase::None};
    std::atomic<Mode> mode_{Mode::ResourcesReady};
    std::function<void(const LiveHandoff&)> live_;
};

// The mapping check before the stage: every edit naming a field this build does not map (or has not proven) is taken out
// of its batch and refused with mapping()'s reason (an entry add naming one is refused whole, so a reference to it names
// that refusal).
void refuse_unmapped(const map::RegMap& rm, const Schema& schema, Batch* b, std::vector<Refusal>* refused);

// One mod's data files' "registry" sections (files by name, as kiln.content reads packs: comments allowed), lowered (ir.h
// lower_section). files: (rel "content/<file>.json", its text).
std::vector<Env::DataFile> read_sections(const std::string& mod, const std::string& version,
                                         const std::vector<std::pair<std::string, std::string>>& files, const Schema& schema);

// kiln-content.dll's runner (stage_service.cpp): its tables, kiln.assets' live bytes and its files.
Runner& runner();
// Binds the tables (kiln.registry v1, kiln.internal.registry v1) from kiln_plugin_main.
void bind_service();

}  // namespace kc::registry::stage
