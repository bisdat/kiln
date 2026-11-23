// SPDX-License-Identifier: MIT
// 16f S3 end to end through native_pack.cpp: a blocks NPC with an authored body is collected, admitted against this
// machine's cook (real files under a temporary mods folder), its layout checked through a kiln.types fake that answers
// the verified Fisher layout, and its cooked resources registered through kiln.assets' private body export (a fake)
// under bootpack:<mod>, inside that mod's rollback. The compiler is native_pack_test.cpp's steered fake.
#define KILN_BOOTPACK_NATIVE_NO_MAIN
#include "bootpack_native_test.cpp"

#include <memory>
#include <tuple>

#include "../../../core/abi/services/kiln_capacity_util.hpp"

namespace {
constexpr const char* kModelGuid = "3cfc8711-f659-5b14-873d-430c16e9c536";
constexpr const char* kGraphGuid = "3c61aa14-09ef-590f-a662-c95fd33f0a36";
constexpr const char* kGraphTypeName = "keen::anim_graph::runtime_graph::AnimationGraphResource2_0";

// ---- kiln.types: the verified layout (PathWalker's size can be changed to stand for a game update) ------------------
struct FakeType {
    std::string name, inner;
    uint32_t size = 0;
    std::vector<std::tuple<std::string, std::string, uint64_t>> fields;
    std::vector<std::pair<std::string, int64_t>> values;
};
std::vector<std::unique_ptr<FakeType>> g_fake_types;
uint32_t t_pathwalker_size = 92;
FakeType* fake_type(const std::string& name) {
    for (auto& t : g_fake_types) if (t->name == name) return t.get();
    g_fake_types.push_back(std::make_unique<FakeType>());
    g_fake_types.back()->name = name;
    return g_fake_types.back().get();
}
void build_fake_types() {
    g_fake_types.clear();
    for (const auto& type : kiln::qb::body_layout()) {
        FakeType* t = fake_type(type.name);
        t->size = std::string(type.name) == "keen::ecs::PathWalker" ? t_pathwalker_size : type.size;
        if (type.inner) { t->inner = type.inner; fake_type(type.inner); }
        for (const auto& f : type.fields) { t->fields.emplace_back(f.name, f.type, f.offset); fake_type(f.type); }
        for (const auto& v : type.values) t->values.emplace_back(v.name, v.value);
    }
}
const KilnType* KILN_CALL ty_by_name(KilnStr name) {
    const std::string wanted(name.ptr, name.len);
    for (const auto& t : g_fake_types) if (t->name == wanted) return reinterpret_cast<const KilnType*>(t.get());
    return nullptr;
}
KilnResult KILN_CALL ty_info(const KilnType* handle, KilnTypeInfo* out) {
    const auto* t = reinterpret_cast<const FakeType*>(handle);
    const uint32_t size = out->size;
    *out = KilnTypeInfo{};
    out->size = size;
    out->name = out->qualified_name = KilnStr{t->name.data(), t->name.size()};
    out->type_size = t->size;
    out->kind = t->values.empty() ? KILN_TYPE_STRUCT : KILN_TYPE_ENUM;
    out->field_count = static_cast<uint32_t>(t->values.empty() ? t->fields.size() : t->values.size());
    out->inner = t->inner.empty() ? nullptr : ty_by_name(KilnStr{t->inner.data(), t->inner.size()});
    return KILN_OK;
}
KilnResult KILN_CALL ty_field(const KilnType* handle, uint32_t index, KilnTypeField* out) {
    const auto* t = reinterpret_cast<const FakeType*>(handle);
    if (index >= t->fields.size()) return KILN_ERR_NOT_FOUND;
    const auto& [name, type, offset] = t->fields[index];
    out->name = KilnStr{name.data(), name.size()};
    out->type = ty_by_name(KilnStr{type.data(), type.size()});
    out->offset = offset;
    return KILN_OK;
}
KilnResult KILN_CALL ty_enum(const KilnType* handle, uint32_t index, KilnStr* name, int64_t* value) {
    const auto* t = reinterpret_cast<const FakeType*>(handle);
    if (index >= t->values.size()) return KILN_ERR_NOT_FOUND;
    *name = KilnStr{t->values[index].first.data(), t->values[index].first.size()};
    *value = t->values[index].second;
    return KILN_OK;
}
KilnResult KILN_CALL ty_status(KilnTypesStatus* out) { out->types = 1; return KILN_OK; }
KilnTypesApi t_types = [] {
    KilnTypesApi api{};
    api.header = {sizeof(KilnTypesApi), KILN_TYPES_VERSION};
    api.status = &ty_status;
    api.type_by_name = &ty_by_name;
    api.type_info = &ty_info;
    api.type_field = &ty_field;
    api.type_enum = &ty_enum;
    return api;
}();

// ---- kiln.assets' private body export ------------------------------------------------------------------------------
bool t_body_bridge = true, t_body_fail = false;
std::vector<std::string> t_body_dirs;
std::vector<KilnOwner> t_body_owners;
std::vector<kiln::qb::ojson> t_body_docs;
KilnResult KILN_CALL body_assets(KilnOwner owner, const char* dir, size_t dir_length, const char* json, size_t length,
                                 char* report, size_t capacity, size_t* written) {
    t_body_dirs.emplace_back(dir, dir_length);
    t_body_owners.push_back(owner);
    t_body_docs.push_back(kiln::qb::ojson::parse(std::string(json, length)));
    for (const auto& entry : t_body_docs.back()["entries"]) {
        if (entry["kind"] != "resource") continue;
        KilnAssetKey key{};
        kiln::qb::Guid guid;
        kiln::qb::parse_guid(entry["guid"].get<std::string>(), &guid);
        std::memcpy(key.guid, guid.b, 16);
        const std::string type = entry["type"].get<std::string>();
        key.type = kiln::qb::fnv1a(type.data(), type.size());
        static const uint8_t bytes[] = {1};
        layered_put(owner, &key, bytes, sizeof bytes, KILN_ASSET_PUT_NEW);
        if (t_body_fail) return reply("entries[1] (resource cooked/tobin.graph): injected: not made here", report, capacity, written, KILN_ERR_INVALID_ARGUMENT);
    }
    return reply("resources 1", report, capacity, written);
}
int t_cooked_verdict = 0;  // kiln.assets' cooked-file verdict on the body cache (0 load, 1 warn, 2 refuse)
int KILN_CALL cooked_verdict(const void*, size_t) { return t_cooked_verdict; }
void* find_body_proc(const char* name) {
    if (std::string(name) == "ka_boot_body_v1") return t_body_bridge ? reinterpret_cast<void*>(&body_assets) : nullptr;
    if (std::string(name) == "ka_cooked_verdict_v1") return t_body_bridge ? reinterpret_cast<void*>(&cooked_verdict) : nullptr;
    return find_proc(name);
}
const void* KILN_CALL body_services(KilnStr name, uint32_t version) {
    if (std::string(name.ptr, name.len) == KILN_TYPES_SERVICE) return &t_types;
    return services(name, version);
}

std::string sha(const std::string& bytes) {
    const auto hash = kiln::capacity::sha256(bytes);
    std::string out;
    kiln::capacity::detail::hex(out, hash.data(), hash.size());
    return out;
}
// A mod with a blocks NPC whose body, animation and motions are named parts, and this machine's (server) cook of it.
void body_mod(const std::string& id) {
    mod(id, kGood);
    const fs::path root = t_root / "mods" / id;
    const std::string blocks = R"({"npcs":{"tobin":{"name":"Tobin","body":"golem","animation":"moves","motions":"poses","roster":{"summonTool":"Tobin"}}},)"
                               R"("bodies":{"golem":{"model":"models/rig.gltf","size":{"radius":0.75,"height":2}}}})";
    write(root / "blocks/npc.json", blocks);
    write(root / "blocks/parts.json", R"({"animations":{"moves":{"clips":"models/rig.gltf"}},"npcMotions":{"poses":{"bindings":[]}}})");
    const std::map<std::string, std::string> files{{"models/rig.gltf", "authored glTF"}, {"assets/cooked/tobin.graph", "graph"}};
    for (const auto& [file, bytes] : files) write(root / file, bytes);
    const std::string closure(64, 'c');
    using J = kiln::qb::ojson;
    J entries = J::array();
    entries.push_back({{"kind", "blob"}, {"file", "assets/cooked/models_rig.blob"}, {"producer", "modelcook"}, {"sourceHash", closure},
                       {"skippedOnServer", true}, {"sha256", nullptr}});
    entries.push_back({{"kind", "resource"}, {"guid", kModelGuid}, {"type", "keen::RenderModel"}, {"part", 0}, {"mode", "new"},
                       {"file", "assets/cooked/models_rig.bin"}, {"patch", {{{"offset", 64}, {"blob", "assets/cooked/models_rig.blob"}}}},
                       {"producer", "modelcook"}, {"sourceHash", closure}, {"skippedOnServer", true}, {"sha256", nullptr}});
    entries.push_back({{"kind", "resource"}, {"guid", kGraphGuid}, {"type", kGraphTypeName}, {"part", 0}, {"mode", "new"},
                       {"file", "assets/cooked/tobin.graph"}, {"producer", "animcook"}, {"sourceHash", closure}, {"sha256", sha("graph")}});
    J bindings = J::array();
    for (int i = 0; i < 27; ++i) bindings.push_back({{"role", "sit"}, {"variant", i}});
    J cooked_spec = {{"animation", {{"clips", "models/rig.gltf"}}}, {"body", {{"model", "models/rig.gltf"}, {"size", {{"height", 2}, {"radius", 0.75}}}}},
                     {"motions", {{"bindings", J::array()}}}, {"modelName", "models/rig"}, {"key", "tobin"}};
    const J cache = {{"format", 1}, {"sourceHash", closure}, {"sources", {{{"file", "models/rig.gltf"}, {"sha256", sha("authored glTF")}}}},
                     {"entries", entries},
                     {"bodies", {{"tobin", {{"key", "tobin"}, {"sourceHash", closure}, {"spec", cooked_spec}, {"model", nullptr}, {"graph", kGraphGuid},
                                            {"skeleton", nullptr}, {"bindings", bindings}, {"materialBlocks", J::array()}, {"files", J::array()},
                                            {"complete", false}}}}}};
    write(root / "assets/cooked/npc-bodies.v1.json", cache.dump(2));
}
void body_reset(const char* name) {
    reset(name);
    t_api.service = &body_services;
    kiln_test_proc = &find_body_proc;
    t_body_bridge = true, t_body_fail = false, t_cooked_verdict = 0;
    t_body_dirs.clear(), t_body_owners.clear(), t_body_docs.clear();
    t_pathwalker_size = 92;
    build_fake_types();
    g_layout_checked = false, g_layout_ok = false, g_layout_why.clear();
    g_body_network_clearance = true;  // the S3/S4 path as it runs once S5 installs the network clearance
}
const kiln::qb::ojson* admitted_body(const std::string& mod) {
    for (const auto& input : kiln::qb::t_compile_inputs)
        if (input.id == mod)
            for (const auto& document : input.documents)
                if (document.contains("npcs") && document["npcs"].contains("tobin") && document["npcs"]["tobin"].contains("body"))
                    return &document["npcs"]["tobin"]["body"];
    return nullptr;
}
bool owns_layers(const std::string& mod) {
    return std::any_of(layers.begin(), layers.end(), [&](const Layer& layer) { return owners[layer.owner] == "bootpack:" + mod; });
}

void test_server_admits_and_publishes() {
    body_reset("16f S3: a server admits a blocks NPC's authored body and registers the cook's graph under bootpack:<mod>");
    body_mod("a-body");
    mod("b-ordinary", kGood);
    boot_and_resource_init();
    CHECK(t_stops.empty());
    CHECK(g_names.count("a-body:k1") == 1 && g_names.count("b-ordinary:k1") == 1);
    CHECK(t_body_owners.size() == 1);
    if (t_body_owners.size() == 1) {
        CHECK(owners[t_body_owners[0]] == "bootpack:a-body");
        CHECK(t_body_dirs[0] == (t_root / "mods" / "a-body").string());
        const auto& entries = t_body_docs[0]["entries"];
        CHECK(entries.size() == 1);  // a dedicated server publishes no renderer output
        if (entries.size() == 1) {
            CHECK(entries[0]["guid"] == kGraphGuid);
            CHECK(entries[0]["file"] == "cooked/tobin.graph");
        }
    }
    const auto* body = admitted_body("a-body");
    CHECK(body != nullptr);
    if (body) {
        CHECK(body->value("model", "") == kModelGuid);  // the server's model id: the cook's RenderModel row
        CHECK(body->value("graph", "") == kGraphGuid);
        CHECK((*body)["size"]["radius"] == 0.75);
        CHECK((*body)["bindings"].size() == 27);
        CHECK(body->value("closure", "") == std::string(64, 'c'));
    }
    CHECK(g_layout_checked && g_layout_ok);
    // S5: the manifest's status row and the persisted ledger
    const auto bodies = kiln::qb::ojson::parse(g_manifest)["bodies"];
    CHECK(bodies.size() == 1);
    if (bodies.size() == 1) {
        const auto& row = bodies[0];
        CHECK(row["mod"] == "a-body" && row["npc"] == "tobin" && row["key"] == "tobin" && row["state"] == "ready" && row["reason"] == "");
        CHECK(row["template"] == kiln::qb::guid_text(kiln::qb::derive_guid("a-body", "npc", "tobin")));
        CHECK(row["model"] == kModelGuid && row["graph"] == kGraphGuid);
        CHECK(row["closure"].get<std::string>().size() == 64 && row["generation"].get<uint64_t>() > 0);
    }
    const auto ledger = kiln::qb::ojson::parse(read_file(t_root / "data/kiln-quests/native/npc-bodies.ledger.json"), nullptr, false);
    CHECK(ledger.is_object() && ledger["bodies"].size() == 1 && ledger["bodies"][0]["state"] == "ready");
}

void test_status_rows_and_removal() {
    body_reset("16f S5: a refused body is a failed row with its reason; a body taken out is reported removed once");
    body_mod("a-body");
    mod("b-ordinary", kGood);
    g_body_network_clearance = false;
    boot_and_resource_init();
    auto bodies = kiln::qb::ojson::parse(g_manifest)["bodies"];
    CHECK(bodies.size() == 1 && bodies[0]["state"] == "failed");
    if (bodies.size() == 1) CHECK(bodies[0]["reason"].get<std::string>().find("need the network clearance") != std::string::npos);

    body_reset("16f S5: first start with the body");
    body_mod("a-body");
    boot_and_resource_init();
    const std::string ledger = read_file(t_root / "data/kiln-quests/native/npc-bodies.ledger.json");
    CHECK(!ledger.empty());
    body_reset("16f S5: the next start without the body mod reports it removed");
    write(t_root / "data/kiln-quests/native/npc-bodies.ledger.json", ledger);
    mod("b-ordinary", kGood);
    boot_and_resource_init();
    bodies = kiln::qb::ojson::parse(g_manifest)["bodies"];
    CHECK(bodies.size() == 1);
    if (bodies.size() == 1) CHECK(bodies[0]["state"] == "removed" && bodies[0]["mod"] == "a-body" && bodies[0]["npc"] == "tobin");
    body_reset("16f S5: and the start after that no longer mentions it");
    write(t_root / "data/kiln-quests/native/npc-bodies.ledger.json", read_file(t_root / "data/kiln-quests/native/npc-bodies.ledger.json"));
    mod("b-ordinary", kGood);
    write(t_root / "data/kiln-quests/native/npc-bodies.ledger.json", R"({"format":1,"bodies":[]})");
    boot_and_resource_init();
    CHECK(kiln::qb::ojson::parse(g_manifest)["bodies"].empty());
}

// review3 S1: a hand-edited ledger with a field of the wrong type, or no JSON at all, must not throw out of the ResourceInit hook. The
// build goes on, says the ledger was damaged, and writes it anew.
void test_a_damaged_ledger_is_named_and_written_anew() {
    for (const char* damaged : {R"({"format":1,"bodies":[{"mod":"a-body","npc":"tobin","state":5}]})",
                                R"({"format":1,"bodies":[{"mod":null,"npc":"tobin","state":"ready"}]})",
                                R"({"format":1,"bodies":[{"mod":"gone","npc":"x","state":"ready","template":7,"closure":[]}]})",
                                R"({"format":1,"bodies":"no"})", "not json", R"([1,2])"}) {
        body_reset((std::string("16f review3 S1: a damaged ledger ") + damaged).c_str());
        body_mod("a-body");
        write(t_root / "data/kiln-quests/native/npc-bodies.ledger.json", damaged);
        bool threw = false;
        try {
            boot_and_resource_init();
        } catch (const std::exception& e) {
            threw = true;
            std::printf("the boot build threw: %s\n", e.what());
        }
        CHECK(!threw);
        CHECK(t_stops.empty());
        CHECK(g_names.count("a-body:k1") == 1);
        const auto bodies = kiln::qb::ojson::parse(g_manifest)["bodies"];
        CHECK(std::any_of(bodies.begin(), bodies.end(), [](const auto& row) { return row["mod"] == "a-body" && row["state"] == "ready"; }));
        const auto ledger = kiln::qb::ojson::parse(read_file(t_root / "data/kiln-quests/native/npc-bodies.ledger.json"), nullptr, false);
        CHECK(ledger.is_object() && ledger["bodies"].is_array() && ledger["bodies"].size() == 1 && ledger["bodies"][0]["state"] == "ready");
    }
    // a row of the wrong type is named once; the others still count
    CHECK(logged("NPC body ledger"));
}

// review3 S2: the body mod was the only mod with native content. The next start collects no mod at all and must still say that the body
// is gone, and write the ledger without it.
void test_removed_when_no_native_mod_is_left() {
    body_reset("16f review3 S2: first start: the body mod alone");
    body_mod("a-body");
    boot_and_resource_init();
    const std::string ledger = read_file(t_root / "data/kiln-quests/native/npc-bodies.ledger.json");
    CHECK(ledger.find("\"ready\"") != std::string::npos);
    body_reset("16f review3 S2: next start: no mod with native content");
    write(t_root / "data/kiln-quests/native/npc-bodies.ledger.json", ledger);
    boot_and_resource_init();
    const auto bodies = kiln::qb::ojson::parse(g_manifest)["bodies"];
    CHECK(bodies.is_array() && bodies.size() == 1);
    if (bodies.is_array() && bodies.size() == 1) CHECK(bodies[0]["state"] == "removed" && bodies[0]["mod"] == "a-body" && bodies[0]["npc"] == "tobin");
    CHECK(logged("NPC body a-body:tobin: removed"));
    const auto after = kiln::qb::ojson::parse(read_file(t_root / "data/kiln-quests/native/npc-bodies.ledger.json"), nullptr, false);
    CHECK(after.is_object() && after["bodies"].is_array() && after["bodies"].empty());
}

// Found by the 16f S6 lab (blocks-npc-test's refusal fixture): an NPC block whose body names a model but has no animation or no
// motions is no authored roster body (the cook keeps such a legacy model on its eager path; Kiln.Blocks refuses the block). The
// start leaves only that NPC out, as before 16f, and builds the rest of the mod's native pack.
void test_an_incomplete_model_body_leaves_only_its_npc_out() {
    body_reset("16f S6: an NPC whose body names a model but no animation or motions leaves only itself out");
    mod("a-legacy", kGood);
    write(t_root / "mods/a-legacy/blocks/npc.json",
          R"({"npcs":{"nobody":{"name":"Nobody","body":"stone"},"keeper":{"name":"Keeper","body":"villager"}},)"
          R"("bodies":{"stone":{"model":"models/stone.glb","scale":1.4}}})");
    boot_and_resource_init();
    CHECK(t_stops.empty());
    CHECK(g_names.count("a-legacy:k1") == 1);
    CHECK(owns_layers("a-legacy"));
    CHECK(kiln::qb::ojson::parse(g_manifest)["bodies"].empty());
    CHECK(logged("a-legacy: npcs.nobody"));
}

void test_refusals_leave_the_neighbour() {
    struct Case { const char* name; std::function<void()> arrange; const char* says; };
    const std::vector<Case> cases{
        {"a client refuses a dedicated server's cook", [] { t_client = true; }, "skipped by a dedicated server's cook"},
        {"a source changed after the cook", [] { write(t_root / "mods/a-body/models/rig.gltf", "edited"); }, "models/rig.gltf changed after the last cook"},
        {"the authored size changed after the cook", [] {
             write(t_root / "mods/a-body/blocks/npc.json", R"({"npcs":{"tobin":{"name":"Tobin","body":"golem","animation":"moves","motions":"poses"}},)"
                                                       R"("bodies":{"golem":{"model":"models/rig.gltf","size":{"radius":0.5,"height":2}}}})"); },
         "body changed after the last cook"},
        {"kiln.assets refuses the body after registering a resource", [] { t_body_fail = true; }, "injected: not made here"},
        {"a game update changed the Fisher layout", [] { t_pathwalker_size = 96; build_fake_types(); }, "body.chassis: keen::ecs::PathWalker is 96 bytes"},
        {"a quests file names a body", [] {
             write(t_root / "mods/a-body/quests/a-body.json", R"({"format":1,"knowledge":{"met":{}},"npcs":{"x":{"template":"NPC_Workshop_Fisher_Trader","clone":true,"body":{"model":"m"}}}})"); },
         "an authored roster body is an NPC block in blocks/*.json"},
        {"another chassis", [] {
             write(t_root / "mods/a-body/blocks/npc.json", R"({"npcs":{"tobin":{"name":"Tobin","body":"golem","animation":"moves","motions":"poses"}},)"
                                                       R"("bodies":{"golem":{"model":"models/rig.gltf","template":"Enemy_Vukah_Melee","size":{"radius":0.75,"height":2}}}})"); },
         "uses the Fisher/Villager chassis only"},
        {"no body registration in kiln.assets", [] { t_body_bridge = false; }, "authored roster-body lowering is not installed"},
        {"the body cache was not made by this game's cook (cooked: local)", [] { t_cooked_verdict = 2; }, "not made by this game's cook, so it is not loaded"},
        {"no network clearance in this build", [] { g_body_network_clearance = false; }, "need the network clearance"},
    };
    for (const auto& c : cases) {
        body_reset((std::string("16f S3: ") + c.name + ": the body mod is refused whole, its neighbour publishes").c_str());
        body_mod("a-body");
        mod("b-ordinary", kGood);
        c.arrange();
        boot_and_resource_init();
        CHECK(t_stops.empty());
        CHECK(g_names.count("a-body:k1") == 0);
        CHECK(g_names.count("b-ordinary:k1") == 1);
        CHECK(!owns_layers("a-body"));
        CHECK(owns_layers("b-ordinary"));
        if (!logged(c.says)) std::printf("expected a log line with: %s\n", c.says);
        CHECK(logged(c.says));
        // S5: every body the refused mod names is a failed row carrying the refusal (review3 N3: also one collection dropped, another
        // chassis), so NpcBodies.Status says why rather than NoDefinition
        const auto manifest = kiln::qb::ojson::parse(g_manifest);
        CHECK(!manifest["bodies"].empty());
        for (const auto& row : manifest["bodies"]) {
            CHECK(row["state"] == "failed" && row["mod"] == "a-body" && row["closure"] == "");
            CHECK(row["reason"].get<std::string>().find(c.says) != std::string::npos);
        }
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    t_root = fs::path(argv[1]) / "body-native-world";
    g_api = &t_api;
    test_server_admits_and_publishes();
    test_refusals_leave_the_neighbour();
    test_status_rows_and_removal();
    test_a_damaged_ledger_is_named_and_written_anew();
    test_removed_when_no_native_mod_is_left();
    test_an_incomplete_model_body_leaves_only_its_npc_out();
    std::error_code ec;
    fs::remove_all(t_root, ec);
    std::printf("body native: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
