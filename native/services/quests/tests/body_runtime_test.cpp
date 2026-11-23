// SPDX-License-Identifier: MIT
// 16f S3: authored roster bodies through the real compiler and the real per-mod publisher (synthetic resources, a
// synthetic Fisher chassis), and the admission of a mod's local body cache (bodyadmit.cpp) on in-memory mod files.
#define main memory_regression_main
#include "bootpack_memory_test.cpp"
#undef main

#include <cstdlib>
#include <map>
#include <set>

#include "../bodyadmit.h"
#include "../../../core/abi/services/kiln_capacity_util.hpp"
#include "body_fixture.h"

namespace {
constexpr const char* kFisherGuid = "e7666cd3-e530-4f9d-9586-519e4ab969dc";
constexpr const char* kModel = "3cfc8711-f659-5b14-873d-430c16e9c536";
constexpr const char* kGraph = "3c61aa14-09ef-590f-a662-c95fd33f0a36";
constexpr const char* kSkeleton = "e67f0760-d62a-54aa-ac36-84952772322d";
constexpr const char* kGraphType = "keen::anim_graph::runtime_graph::AnimationGraphResource2_0";

Guid guid(const char* text) { Guid g; parse_guid(text, &g); return g; }
std::string sha(const std::string& bytes) {
    const auto hash = kiln::capacity::sha256(bytes);
    std::string out;
    kiln::capacity::detail::hex(out, hash.data(), hash.size());
    return out;
}

// ---- the layout check -----------------------------------------------------------------------------------------
void layout_check() {
    std::string why;
    CHECK(check_body_layout(&body_fixture::layout_lookup, &why));
    CHECK(why.empty());
    CHECK(!check_body_layout(nullptr, &why));
    CHECK(why.find("body.chassis") != std::string::npos);
    struct Change { const char* type; std::function<void(BodyTypeFacts&)> change; const char* says; };
    const std::vector<Change> changes{
        {"keen::ecs::PathWalker", [](BodyTypeFacts& f) { f.size = 96; }, "keen::ecs::PathWalker is 96 bytes"},
        {"keen::ecs::PathWalker", [](BodyTypeFacts& f) {
             for (auto& field : f.fields) if (field.name == "horizontalRadius") field.offset += 4; }, "keen::ecs::PathWalker.horizontalRadius moved"},
        {"keen::actor::SetAnimationEvent", [](BodyTypeFacts& f) { f.fields[0].offset = 0x68; }, "keen::actor::SetAnimationEvent.animationName moved"},
        {"keen::ecs::NpcDialog", [](BodyTypeFacts& f) { f.size = 124; }, "keen::ecs::NpcDialog is 124 bytes"},
        {"keen::ecs::ModelResource", [](BodyTypeFacts& f) { f.fields[0].type = "keen::Guid"; }, "keen::ecs::ModelResource.model moved"},
        {"keen::ecs::CharacterSetup", [](BodyTypeFacts& f) { f.inner = "keen::ecs::Other"; }, "keen::ecs::CharacterSetup no longer derives"},
        {"keen::ecs::ColliderFlags", [](BodyTypeFacts& f) { f.values[0].second = 14; }, "keen::ecs::ColliderFlags::Interaction changed"},
        {"keen::ecs::UiOffsets", nullptr, "keen::ecs::UiOffsets is missing"},
    };
    for (const auto& c : changes) {
        const BodyTypeLookup lookup = [&](const std::string& name, BodyTypeFacts* out) {
            if (name == c.type && !c.change) return false;
            if (!body_fixture::layout_lookup(name, out)) return false;
            if (name == c.type) c.change(*out);
            return true;
        };
        CHECK(!check_body_layout(lookup, &why));
        if (why.find(c.says) == std::string::npos) std::printf("layout refusal said: %s\n", why.c_str());
        CHECK(why.find(c.says) != std::string::npos);
    }
    // Extra registry fields and values the body does not interpret are not a difference.
    const BodyTypeLookup extra = [](const std::string& name, BodyTypeFacts* out) {
        if (!body_fixture::layout_lookup(name, out)) return false;
        out->fields.push_back({"addedByAnUpdate", "keen::float32", 400});
        out->values.emplace_back("Added", 30);
        return true;
    };
    CHECK(check_body_layout(extra, &why));
}

// ---- the real compiler and publisher with an admitted authored body --------------------------------------------
const uint32_t kModelType = fnv1a("keen::RenderModel", std::strlen("keen::RenderModel"));
const uint32_t kSequenceTypeHash = fnv1a(motionclose::kSequenceType, std::strlen(motionclose::kSequenceType));
const uint32_t kGraphTypeHash = fnv1a(kGraphType, std::strlen(kGraphType));

ModInput knowledge_only(const char* id) {
    ModInput mod;
    mod.id = id;
    mod.documents = {ojson{{"knowledge", {{"seen", {{"scope", "both"}}}}}}};
    return mod;
}
ModInput body_mod(const char* id) {
    ModInput mod = knowledge_only(id);
    mod.authored_body = true;
    mod.documents.push_back({{"npcs", {{"tobin", {
        {"template", kBodyChassis}, {"clone", {{"guid", guid_text(derive_guid(id, "npc", "tobin"))}, {"name", std::string("Kiln_") + id + "_tobin"}}},
        {"names", {{"en", "Tobin"}}}, {"body", {{"block", "tobin"}, {"spec", {{"body", {{"model", "models/rig.gltf"}}}}}}}}}}}});
    return mod;
}
struct World {
    Store store;
    bootpack::Backend backend;
    Bytes fisher = body_fixture::fisher();
    const Bytes pristine = fisher;
    std::map<std::string, Bytes> sequences;  // the Fisher's 21 closure sequences, by source GUID text
    std::map<uint64_t, std::string> owner_mod;
    std::vector<std::string> admitted, published;
    bool layout_ok = true;
    std::string refuse_admission, fail_body_put, fail_finish;
    World() {
        backend = store.backend();
        for (const auto& s : motionclose::fisher_closure()) sequences[s.guid] = body_fixture::sequence(s);
        const auto read = backend.game.read;
        backend.game.read = [this, read](const Guid& g, uint32_t type, const uint8_t** p, size_t* n) {
            if (type == kTypeTemplate && g == guid(kFisherGuid)) { *p = fisher.data(); *n = fisher.size(); return true; }
            if (type == kSequenceTypeHash)
                if (const auto it = sequences.find(guid_text(g)); it != sequences.end()) { *p = it->second.data(); *n = it->second.size(); return true; }
            return read(g, type, p, n);
        };
        backend.game.template_by_name = [](const std::string& name, Guid* out) {
            if (name != kBodyChassis) return false;
            *out = guid(kFisherGuid);
            return true;
        };
        backend.game.body_layout = [this](std::string* why) {
            if (layout_ok) return true;
            *why = "body.chassis: keen::ecs::PathWalker is 96 bytes, not 92";
            return false;
        };
        const auto owner = backend.owner;
        backend.owner = [this, owner](const std::string& mod) { const auto o = owner(mod); owner_mod[o] = mod; return o; };
        const auto finish = backend.finish;
        backend.finish = [this, finish](uint64_t o, std::string* why) {
            if (owner_mod[o] == fail_finish) { *why = "injected content finish refusal"; return false; }
            return finish(o, why);
        };
        backend.admit_body = [this](ModInput* mod, ojson* assets, std::string* why) {
            admitted.push_back(mod->id);
            if (mod->id == refuse_admission) { *why = "npcs.tobin: models/rig.gltf changed after the last cook; cook the mod again"; return false; }
            for (auto& document : mod->documents)
                if (document.contains("npcs"))
                    for (auto& [name, npc] : document["npcs"].items())
                        if (npc.contains("body"))
                            npc["body"] = {{"model", kModel}, {"graph", kGraph}, {"size", {{"radius", .75}, {"height", 2}}},
                                           {"bindings", body_fixture::bindings()}, {"closure", "c"}};
            *assets = {{"entries", {{{"kind", "resource"}, {"guid", kModel}, {"type", "keen::RenderModel"}, {"mode", "new"}, {"file", "cooked/rig.bin"}},
                                    {{"kind", "resource"}, {"guid", kGraph}, {"type", kGraphType}, {"mode", "new"}, {"file", "cooked/rig.graph"}}}}};
            return true;
        };
        backend.put_body = [this](uint64_t o, const ModInput& mod, const ojson& assets, std::string* why) {
            published.push_back(mod.id);
            for (const auto& entry : assets["entries"]) {
                const std::string type = entry["type"].get<std::string>();
                store.layers.push_back({o, guid(entry["guid"].get<std::string>().c_str()), fnv1a(type.data(), type.size()), Bytes{1, 2, 3}});
                if (mod.id == fail_body_put) { *why = "kiln.assets refused the body's cooked resources: injected"; return false; }
            }
            return true;
        };
    }
    const Store::Layer* layer(const Guid& g, uint32_t type) const {
        for (auto it = store.layers.rbegin(); it != store.layers.rend(); ++it)
            if (it->guid == g && it->type == type) return &*it;
        return nullptr;
    }
    bool knows(const char* mod) const {
        const auto* k = layer(guid(kGuidKnowledge), kTypeKnowledge);
        return k && knowledge_find(k->bytes.data(), k->bytes.size(), false, derive_id(mod, "know", "seen")) != nullptr;
    }
    std::set<std::string> committed(const bootpack::Result& r) const {
        std::set<std::string> out;
        for (const auto& a : r.applied) out.insert(a.mod);
        return out;
    }
};

void lowered_clone_is_published_under_its_mod() {
    World w;
    const auto result = bootpack::build({knowledge_only("a-plain"), body_mod("b-body"), knowledge_only("c-after")}, w.backend);
    CHECK(!result.stopped && result.errors.empty());
    CHECK((w.committed(result) == std::set<std::string>{"a-plain", "b-body", "c-after"}));
    CHECK(w.admitted == std::vector<std::string>{"b-body"});
    CHECK(w.published == std::vector<std::string>{"b-body"});
    const Guid clone = derive_guid("b-body", "npc", "tobin");
    const auto* tpl = w.layer(clone, kTypeTemplate);
    CHECK(tpl != nullptr);
    CHECK(w.layer(guid(kFisherGuid), kTypeTemplate) == nullptr);  // the vanilla key is never written
    CHECK(w.fisher == w.pristine);
    if (tpl) {
        CHECK(w.owner_mod[tpl->owner] == "b-body");
        const Bytes& b = tpl->bytes;
        for (const char* removed : {"CharacterSetup", "NewCharacterSetup", "RenderEquipment", "EquipmentCheck", "SlotSelection"})
            CHECK(body_fixture::component(b, removed) == 0);
        const size_t model = body_fixture::component(b, "ModelResource"), graph = body_fixture::component(b, "AnimationGraphResourceComponent2");
        CHECK(model && std::memcmp(b.data() + model, guid(kModel).b, 16) == 0);
        CHECK(graph && std::memcmp(b.data() + graph, guid(kGraph).b, 16) == 0);
        CHECK(body_fixture::get32(b, body_fixture::component(b, "ActiveNpcState")) == fnv(clone));  // the quest edit ran after the body
        float radius = 0;
        std::memcpy(&radius, b.data() + body_fixture::component(b, "PathWalker") + 56, 4);
        CHECK(radius == .75f);
    }
    for (const auto& [g, type] : {std::pair{guid(kModel), kModelType}, std::pair{guid(kGraph), kGraphTypeHash}}) {
        const auto* l = w.layer(g, type);
        CHECK(l && w.owner_mod[l->owner] == "b-body");
    }
    CHECK(w.knows("a-plain") && w.knows("b-body") && w.knows("c-after"));
    // S4: the 21 closure clones are new keys of b-body; every template reference names a clone, none a source
    std::map<std::string, Guid> clones;
    for (const auto& l : w.store.layers)
        if (l.type == kSequenceTypeHash) {
            CHECK(w.owner_mod[l.owner] == "b-body");
            Guid id;
            std::memcpy(id.b, l.bytes.data(), 16);
            CHECK(id == l.guid);  // resourceId follows the key
            clones[guid_text(l.guid)] = l.guid;
        }
    CHECK(clones.size() == 21);
    if (tpl) {
        const Bytes& b = tpl->bytes;
        const auto at = [&](const char* c, size_t o) { Guid g; std::memcpy(g.b, b.data() + body_fixture::component(b, c) + o, 16); return g; };
        for (const auto& [c, o] : {std::pair{"ActiveNpcState", size_t{4}}, {"ActiveNpcState", 0x14}, {"NpcState", 0x50}, {"PathWalker", 0x10}})
            CHECK(clones.count(guid_text(at(c, o))) == 1);
        for (const auto& s : motionclose::fisher_closure()) {
            Guid g;
            parse_guid(s.guid, &g);
            CHECK(std::search(b.begin(), b.end(), std::begin(g.b), std::end(g.b)) == b.end());
        }
    }
    for (const auto& [text, bytes] : w.sequences) CHECK(bytes == body_fixture::sequence(*std::find_if(
        motionclose::fisher_closure().begin(), motionclose::fisher_closure().end(), [&](const auto& s) { return text == s.guid; })));  // sources untouched
}

void refused_or_withdrawn_body_mods_leave_their_neighbours() {
    for (const char* how : {"admission", "body-put", "finish", "layout", "chassis", "taken"}) {
        std::printf("-- 16f S3: a %s refusal of an authored body mod leaves earlier and independent mods intact\n", how);
        World w;
        const std::string mode = how;
        if (mode == "admission") w.refuse_admission = "b-body";
        if (mode == "body-put") w.fail_body_put = "b-body";
        if (mode == "finish") w.fail_finish = "b-body";
        if (mode == "layout") w.layout_ok = false;
        if (mode == "chassis") { w.fisher.resize(23); }  // a changed chassis the lowerer refuses
        if (mode == "taken") w.store.layers.push_back({999, guid(kModel), kModelType, Bytes{9}});  // another writer has the key
        const auto result = bootpack::build({knowledge_only("a-plain"), body_mod("b-body"), knowledge_only("c-after")}, w.backend);
        CHECK(!result.stopped);
        CHECK((w.committed(result) == std::set<std::string>{"a-plain", "c-after"}));
        CHECK(result.errors.size() == 1);
        if (!result.errors.empty()) {
            const std::string& e = result.errors.front();
            CHECK(e.rfind("bootpack:b-body: ", 0) == 0);
            const char* says = mode == "admission" ? "changed after the last cook" : mode == "body-put" ? "injected"
                             : mode == "finish" ? "injected content finish" : mode == "layout" ? "body.chassis"
                             : mode == "chassis" ? "body.chassis" : "new key already exists";
            if (e.find(says) == std::string::npos) std::printf("refusal said: %s\n", e.c_str());
            CHECK(e.find(says) != std::string::npos);
        }
        // nothing of b remains: no template, no body resource of its owner, no knowledge row; a and c are whole
        CHECK(w.layer(derive_guid("b-body", "npc", "tobin"), kTypeTemplate) == nullptr);
        for (const auto& l : w.store.layers) CHECK(w.owner_mod.count(l.owner) == 0 || w.owner_mod[l.owner] != "b-body");
        if (mode == "taken") CHECK(w.layer(guid(kModel), kModelType) && w.layer(guid(kModel), kModelType)->owner == 999);
        else CHECK(w.layer(guid(kModel), kModelType) == nullptr);
        CHECK(w.layer(guid(kGraph), kGraphTypeHash) == nullptr);
        CHECK(std::none_of(w.store.layers.begin(), w.store.layers.end(), [](const auto& l) { return l.type == kSequenceTypeHash; }));
        CHECK(w.knows("a-plain") && !w.knows("b-body") && w.knows("c-after"));
        CHECK(w.fisher.size() == 23 || w.fisher == w.pristine);
    }
}

void missing_lowerer_and_collection_refusals() {
    World w;
    w.backend.admit_body = nullptr;
    auto result = bootpack::build({body_mod("b-body"), knowledge_only("c-after")}, w.backend);
    CHECK((w.committed(result) == std::set<std::string>{"c-after"}));
    CHECK(!result.errors.empty() && result.errors.front().find("lowering is not installed") != std::string::npos);
    World v;
    auto refused = body_mod("b-body");
    refused.body_refusal = "quests/own.json: npcs.x.body: an authored roster body is an NPC block in blocks/*.json";
    result = bootpack::build({refused, knowledge_only("c-after")}, v.backend);
    CHECK((v.committed(result) == std::set<std::string>{"c-after"}));
    CHECK(v.admitted.empty());
    CHECK(!result.errors.empty() && result.errors.front().find("an NPC block in blocks/*.json") != std::string::npos);
    // a body is never lowered on a vanilla key or onto another chassis, even if admission wrote one
    World u;
    auto vanilla = body_mod("b-body");
    vanilla.documents[1]["npcs"]["tobin"]["clone"] = false;
    result = bootpack::build({vanilla}, u.backend);
    CHECK(u.committed(result).empty());
    CHECK(!result.errors.empty() && result.errors.front().find("new roster clone of the Fisher chassis") != std::string::npos);
}

// ---- admission of the local cook cache ------------------------------------------------------------------------
struct Mod {
    std::map<std::string, std::string> files;
    bool client = true;
    ojson cache, spec;
    std::string closure = std::string(64, 'c');
    int verdict = 0;  // kiln.assets' cooked-file verdict on the cache's bytes
    std::vector<std::string> warnings;
    Mod() {
        files["models/rig.gltf"] = "authored glTF";
        files["blocks/skin.json"] = R"({"materials":{"skin":{}}})";
        files["assets/cooked/models_rig.bin"] = "render model";
        files["assets/cooked/models_rig.blob"] = "render data";
        files["assets/cooked/tobin.graph"] = "graph";
        spec = {{"body", {{"model", "models/rig.gltf"}, {"size", {{"radius", .75}, {"height", 2}}}}},
                {"animation", {{"clips", "models/rig.gltf"}}}, {"motions", {{"bindings", ojson::array()}}}};
        rebuild();
    }
    void rebuild(bool server_cook = false) {
        ojson entries = ojson::array();
        const auto row = [&](const char* kind, const std::string& file, const char* producer, bool skipped) {
            ojson e = {{"kind", kind}, {"file", file}, {"producer", producer}, {"sourceHash", closure}};
            if (skipped) { e["skippedOnServer"] = true; e["sha256"] = nullptr; }
            else e["sha256"] = sha(files[file]);
            return e;
        };
        const bool skip = server_cook;
        entries.push_back(row("blob", "assets/cooked/models_rig.blob", "modelcook", skip));
        auto model = row("resource", "assets/cooked/models_rig.bin", "modelcook", skip);
        model.update({{"guid", kModel}, {"type", "keen::RenderModel"}, {"part", 0}, {"mode", "new"},
                      {"patch", {{{"offset", 64}, {"blob", "assets/cooked/models_rig.blob"}}}}});
        entries.push_back(model);
        auto graph = row("resource", "assets/cooked/tobin.graph", "animcook", false);
        graph.update({{"guid", kGraph}, {"type", kGraphType}, {"part", 0}, {"mode", "new"}});
        entries.push_back(graph);
        ojson cooked_spec = spec;
        cooked_spec["modelName"] = "models/rig";
        ojson bindings = ojson::array();
        for (int i = 0; i < 27; ++i) bindings.push_back({{"role", "sit"}, {"variant", i}});
        cache = {{"format", 1}, {"sourceHash", closure},
                 {"sources", {{{"file", "models/rig.gltf"}, {"sha256", sha(files["models/rig.gltf"])}}}},
                 {"entries", entries},
                 {"bodies", {{"tobin", {{"key", "tobin"}, {"sourceHash", closure}, {"spec", cooked_spec},
                                        {"model", server_cook ? ojson(nullptr) : ojson(kModel)}, {"graph", kGraph}, {"skeleton", kSkeleton},
                                        {"bindings", bindings}, {"materialBlocks", ojson::array()}, {"files", ojson::array()},
                                        {"complete", !server_cook}}}}}};
        files[bodyadmit::kCache] = cache.dump();
    }
    bodyadmit::Env env() {
        bodyadmit::Env e;
        e.client = client;
        e.read = [this](const std::string& file, size_t max, Bytes* out, std::string* why) {
            if (!bodyadmit::contained(file)) { *why = "not contained"; return false; }
            const auto it = files.find(file);
            if (it == files.end()) { *why = "missing"; return false; }
            if (it->second.size() > max) { *why = "too large"; return false; }
            out->assign(it->second.begin(), it->second.end());
            return true;
        };
        e.layout = [](std::string*) { return true; };
        e.materials.server = !client;
        e.cooked = [this](const Bytes& bytes) { return std::string(bytes.begin(), bytes.end()) == files[bodyadmit::kCache] ? verdict : -1; };
        e.warn = [this](const std::string& line) { warnings.push_back(line); };
        return e;
    }
    ModInput input() const {
        ModInput m;
        m.id = "body-mod";
        m.authored_body = true;
        const ojson npc = {{"template", kBodyChassis}, {"clone", {{"guid", kModel}}}, {"body", {{"block", "tobin"}, {"spec", spec}}}};
        m.documents = {ojson{{"npcs", {{"tobin", npc}}}}};
        return m;
    }
};
bool admit(Mod& m, ModInput* input, ojson* assets, std::string* why) {
    *input = m.input();
    return bodyadmit::admit(input, m.env(), assets, why);
}

void admission_of_the_local_cache() {
    {
        std::printf("-- 16f S3 admission: a client admits its complete cook and publishes every cooked output\n");
        Mod m;
        ModInput in; ojson assets; std::string why;
        CHECK(admit(m, &in, &assets, &why));
        const ojson& body = in.documents[0]["npcs"]["tobin"]["body"];
        CHECK(body.value("model", "") == kModel && body.value("graph", "") == kGraph);
        CHECK(body["size"] == m.spec["body"]["size"]);
        CHECK(body["bindings"].size() == 27);
        CHECK(body.value("closure", "") == m.closure);
        CHECK(assets["entries"].size() == 3);
        for (const auto& e : assets["entries"]) CHECK(e["file"].get<std::string>().rfind("cooked/", 0) == 0);
        CHECK(assets["entries"][1]["patch"][0]["blob"] == "cooked/models_rig.blob");
        CHECK(!assets["entries"][1].contains("sha256") && !assets["entries"][1].contains("producer"));
    }
    {
        std::printf("-- 16f S3 admission: a dedicated server takes the model id from the cook's RenderModel row and publishes no renderer output\n");
        Mod m;
        m.client = false;
        m.rebuild(true);
        ModInput in; ojson assets; std::string why;
        CHECK(admit(m, &in, &assets, &why));
        if (!why.empty()) std::printf("server admission said: %s\n", why.c_str());
        CHECK(in.documents[0]["npcs"]["tobin"]["body"].value("model", "") == kModel);
        CHECK(assets["entries"].size() == 1);
        CHECK(assets["entries"][0].value("guid", "") == kGraph);
    }
    struct Case { const char* name; std::function<void(Mod&)> change; const char* says; };
    const std::vector<Case> cases{
        {"no cache", [](Mod& m) { m.files.erase(bodyadmit::kCache); }, "cook the mod"},
        {"a source changed after the cook", [](Mod& m) { m.files["models/rig.gltf"] = "edited"; }, "models/rig.gltf changed after the last cook"},
        {"a cooked output changed after the cook", [](Mod& m) { m.files["assets/cooked/tobin.graph"] = "other"; }, "assets/cooked/tobin.graph changed after the last cook"},
        {"the authored size changed after the cook", [](Mod& m) { m.spec["body"]["size"]["radius"] = .5; }, "body changed after the last cook"},
        {"the motions changed after the cook", [](Mod& m) { m.spec["motions"]["optionalFallback"] = "Idle"; }, "motions changed after the last cook"},
        {"a server's cook on a client", [](Mod& m) { m.rebuild(true); }, "skipped by a dedicated server's cook"},
        {"an incomplete client cook", [](Mod& m) { m.cache["bodies"]["tobin"]["complete"] = false; m.files[bodyadmit::kCache] = m.cache.dump(); }, "not complete"},
        {"a graph that is not the cook's", [](Mod& m) { m.cache["bodies"]["tobin"]["graph"] = kSkeleton; m.files[bodyadmit::kCache] = m.cache.dump(); }, "graph is not one of the cook's"},
        {"a patch blob that is not a cooked blob", [](Mod& m) { m.cache["entries"][1]["patch"][0]["blob"] = "assets/cooked/tobin.graph"; m.files[bodyadmit::kCache] = m.cache.dump(); }, "patch blob is not a cooked body blob"},
        {"an entry outside the cooked folder", [](Mod& m) { m.cache["entries"][2]["file"] = "blocks/skin.json"; m.files[bodyadmit::kCache] = m.cache.dump(); }, "not a cooked resource or blob"},
        {"a traversing source", [](Mod& m) { m.cache["sources"][0]["file"] = "../other/x"; m.files[bodyadmit::kCache] = m.cache.dump(); }, "source row is malformed"},
        {"an empty entry", [](Mod& m) { m.cache["entries"].push_back(ojson::object()); m.files[bodyadmit::kCache] = m.cache.dump(); }, "not a cooked resource or blob"},
        {"an older cook's body", [](Mod& m) { m.cache["bodies"]["tobin"]["sourceHash"] = std::string(64, 'd'); m.files[bodyadmit::kCache] = m.cache.dump(); }, "older cook"},
        {"an overriding body resource", [](Mod& m) { m.cache["entries"][2]["mode"] = "override"; m.files[bodyadmit::kCache] = m.cache.dump(); }, "new keyed resource"},
        {"no body for the NPC", [](Mod& m) { m.cache["bodies"].erase("tobin"); m.files[bodyadmit::kCache] = m.cache.dump(); }, "has no body for this NPC"},
        {"an incomplete motion vector", [](Mod& m) { m.cache["bodies"]["tobin"]["bindings"].erase(0); m.files[bodyadmit::kCache] = m.cache.dump(); }, "27 rows"},
        {"two RenderModel rows for one body", [](Mod& m) { m.cache["entries"].push_back(m.cache["entries"][1]); m.files[bodyadmit::kCache] = m.cache.dump(); }, "2 RenderModel entries"},
        {"not JSON", [](Mod& m) { m.files[bodyadmit::kCache] = "{"; }, "not a format-1 body cache"},
    };
    for (const auto& c : cases) {
        std::printf("-- 16f S3 admission refuses: %s\n", c.name);
        Mod m;
        c.change(m);
        ModInput in; ojson assets; std::string why;
        const bool ok = admit(m, &in, &assets, &why);
        CHECK(!ok);
        if (why.find(c.says) == std::string::npos) std::printf("admission said: %s\n", why.c_str());
        CHECK(why.find(c.says) != std::string::npos);
        CHECK(why.rfind("npcs.tobin: ", 0) == 0);
    }
    {
        // review3 M2: a body defined in code (Kiln.Blocks' NPC files) has no cooked source identity and skips G2's material check: v1
        // refuses it, on a client and on a server, with the way to define it (blocks/*.json), and never asks for its resources
        std::printf("-- 16f admission: a body defined in code is refused (no source identity, no material check): blocks/*.json\n");
        Mod m;
        bool asked = false;
        auto env = m.env();
        env.materials.build_status = [&](ojson*, std::string* why) { asked = true; *why = "not asked"; return false; };
        ModInput in;
        in.id = "code-mod";
        in.authored_body = true;
        const ojson code_body = {{"model", kModel}, {"graph", kGraph}, {"skeleton", kSkeleton}, {"size", {{"radius", .5}, {"height", 1.5}}}};
        const ojson npc = {{"template", kBodyChassis}, {"body", code_body}};
        in.documents = {ojson{{"npcs", {{"bram", npc}}}}};
        for (const bool client : {true, false}) {
            env.client = client;
            ModInput copy = in;
            ojson assets; std::string why;
            CHECK(!bodyadmit::admit(&copy, env, &assets, &why));
            if (why.find("blocks/*.json") == std::string::npos) std::printf("admission said: %s\n", why.c_str());
            CHECK(why.rfind("npcs.bram: ", 0) == 0);
            CHECK(why.find("defined in C#") != std::string::npos && why.find("blocks/*.json") != std::string::npos);
        }
        CHECK(!asked);
    }
    {
        // review3 S4: the body cache is a cooked file like any other: kiln.assets' verdict (this game's cook receipts) applies to it
        std::printf("-- 16f review3 S4: the body cache goes through kiln.assets' cooked-file verdict\n");
        Mod m;
        m.verdict = 2;
        ModInput in = m.input(); ojson assets; std::string why;
        CHECK(!admit(m, &in, &assets, &why));
        if (why.find("not made by this game's cook") == std::string::npos) std::printf("admission said: %s\n", why.c_str());
        CHECK(why.find("npcs.tobin: assets/cooked/npc-bodies.v1.json") == 0 && why.find("not made by this game's cook") != std::string::npos);
        m.verdict = 1;
        in = m.input();
        CHECK(admit(m, &in, &assets, &why));
        CHECK(m.warnings.size() == 1 && m.warnings[0].find("not made by this game's cook") != std::string::npos);
        m.verdict = 0;
        m.warnings.clear();
        in = m.input();
        CHECK(admit(m, &in, &assets, &why) && m.warnings.empty());
        auto env = m.env();
        env.cooked = nullptr;  // fail closed when kiln.assets cannot be asked
        in = m.input();
        CHECK(!bodyadmit::admit(&in, env, &assets, &why));
        CHECK(why.find("cannot check") != std::string::npos);
    }
    {
        std::printf("-- 16f S3 admission: the type registry refusal names the chassis\n");
        Mod m;
        auto env = m.env();
        env.layout = [](std::string* why) { *why = "body.chassis: keen::ecs::PathWalker is 96 bytes, not 92"; return false; };
        ModInput in = m.input(); ojson assets; std::string why;
        CHECK(!bodyadmit::admit(&in, env, &assets, &why));
        CHECK(why.find("body.chassis") != std::string::npos);
    }
    CHECK(bodyadmit::contained("assets/cooked/a.bin") && bodyadmit::contained("blocks/x.json"));
    for (const char* bad : {"", "/abs", "../x", "a/../b", "a//b", "a\\b", "c:x", "./a", "a/"}) CHECK(!bodyadmit::contained(bad));
    // the cooker's part resolution: inline, by name, this mod's qualified name, never another mod's or a model-less body
    const ojson bodies = {{"golem", {{"model", "models/golem.gltf"}}}, {"plain", {{"like", "fisher"}}}};
    const ojson animations = {{"moves", {{"clips", "models/golem.gltf"}}}};
    const ojson motions = {{"poses", {{"bindings", ojson::array()}}}};
    ojson spec = bodyadmit::resolve_spec("m", {{"body", "golem"}, {"animation", "m:moves"}, {"motions", "poses"}}, bodies, animations, motions);
    CHECK(spec["body"] == bodies["golem"] && spec["animation"] == animations["moves"] && spec["motions"] == motions["poses"]);
    CHECK(bodyadmit::resolve_spec("m", {{"body", {{"model", "x.glb"}}}}, bodies, animations, motions)["body"]["model"] == "x.glb");
    CHECK(bodyadmit::resolve_spec("m", {{"body", "other:golem"}}, bodies, animations, motions).is_null());
    CHECK(bodyadmit::resolve_spec("m", {{"body", "plain"}}, bodies, animations, motions).is_null());
    CHECK(bodyadmit::resolve_spec("m", {{"body", "golem"}, {"animation", "other:moves"}}, bodies, animations, motions)["animation"].is_null());
}
// ---- S4: the motion closure itself ----------------------------------------------------------------------------
const motionclose::Source& source(const char* role) {
    for (const auto& s : motionclose::fisher_closure()) if (std::string(s.role) == role) return s;
    std::abort();
}
int64_t at64(const Bytes& b, size_t at) { int64_t v; std::memcpy(&v, b.data() + at, 8); return v; }
struct Parsed { size_t sub; std::vector<std::pair<uint32_t, size_t>> events; };  // type hash, payload offset
Parsed parse(const Bytes& b) {
    Parsed p;
    p.sub = 16 + static_cast<int32_t>(body_fixture::get32(b, 16));
    const size_t headers = p.sub + static_cast<int32_t>(body_fixture::get32(b, p.sub));
    for (uint32_t i = 0; i < body_fixture::get32(b, p.sub + 4); ++i) {
        const size_t h = headers + i * 12;
        p.events.emplace_back(body_fixture::get32(b, h), h + 4 + static_cast<int32_t>(body_fixture::get32(b, h + 4)));
    }
    return p;
}
void motion_closure_units() {
    std::printf("-- 16f S4: bindings, sequence clones, retiming and template remapping\n");
    motionclose::Bindings bound;
    std::string why;
    CHECK(motionclose::parse_bindings(body_fixture::bindings(), &bound, &why));
    CHECK(bound.size() == 21 && bound["sit"].size() == 3 && bound["door"].size() == 3 && bound["jump"].size() == 1);
    struct Bad { const char* name; std::function<void(ojson&)> change; const char* says; };
    for (const auto& c : std::vector<Bad>{
             {"26 rows", [](ojson& r) { r.erase(r.size() - 1); }, "27 rows"},
             {"a duplicate", [](ojson& r) { r[1] = r[0]; }, "duplicate sit[0]"},
             {"an unknown role", [](ojson& r) { r[0]["role"] = "dance"; }, "dance[0] is not in the Fisher manifest"},
             {"a fourth sit variant", [](ojson& r) { r[0]["variant"] = 3; }, "sit[3] is not in the Fisher manifest"},
             {"a zero state", [](ojson& r) { r[0]["state"] = "00000000-0000-0000-0000-000000000000"; }, ".state"},
             {"no duration", [](ojson& r) { r[0]["durationNs"] = 0; }, ".durationNs"}}) {
        ojson rows = body_fixture::bindings();
        c.change(rows);
        motionclose::Bindings out;
        CHECK(!motionclose::parse_bindings(rows, &out, &why));
        if (why.find(c.says) == std::string::npos) std::printf("bindings (%s) said: %s\n", c.name, why.c_str());
        CHECK(why.find(c.says) != std::string::npos);
    }

    // The sit clone: new ids, bound states, times exactly as the verified time map (motionpack) maps them.
    const auto& sit = source("sit");
    const Bytes raw = body_fixture::sequence(sit);
    const Guid clone = derive_guid("m", "npcmotion", "sit"), sub = derive_guid("m", "npcmotion", "sub");
    Bytes out{0x5a};
    CHECK(motionclose::clone_sequence(raw.data(), raw.size(), clone, sit, bound["sit"], [&](const Guid&) { return sub; }, &out, &why));
    CHECK(out.size() == raw.size());
    const Parsed in = parse(raw), cl = parse(out);
    CHECK(std::memcmp(out.data(), clone.b, 16) == 0);
    CHECK(std::memcmp(out.data() + cl.sub + 0x30, sub.b, 16) == 0);
    std::vector<motionpack::Event> events;
    std::vector<motionpack::Binding> bindings;
    size_t ordinal = 0;
    for (size_t j = 0; j < in.events.size(); ++j) {
        const auto [type, p] = in.events[j];
        motionpack::Event e;
        e.span = {at64(raw, p), at64(raw, p + 8)};
        e.type = type == body_fixture::hash("keen::actor::SetAnimationEvent") ? motionpack::EventType::SetAnimation
               : type == body_fixture::hash("keen::actor::LerpMovementFactorEvent") ? motionpack::EventType::LerpMovementFactor
               : type == body_fixture::hash("keen::actor::LoopEvent") ? motionpack::EventType::Loop
               : type == body_fixture::hash("keen::SfxNotifierEvent") ? motionpack::EventType::SfxNotifier : motionpack::EventType::SetState;
        if (e.type == motionpack::EventType::LerpMovementFactor) e.lerp_duration = at64(raw, p + 0x70);
        if (e.type == motionpack::EventType::SetAnimation) {
            const auto& v = bound["sit"][ordinal];
            bindings.push_back({j, v.duration, v.loop, ordinal == 0 ? motionpack::Phase::Enter : ordinal == 1 ? motionpack::Phase::Loop : motionpack::Phase::Exit});
            CHECK(std::memcmp(out.data() + cl.events[j].second + 0x60, v.state.b, 16) == 0);  // the bound state, in serialized order
            ++ordinal;
        }
        events.push_back(e);
    }
    motionpack::Retimed expected;
    CHECK(motionpack::retime(at64(raw, in.sub + 8), events, bindings, &expected, &why));
    CHECK(at64(out, cl.sub + 8) == expected.length && expected.length != at64(raw, in.sub + 8));
    for (size_t j = 0; j < cl.events.size(); ++j) {
        CHECK(at64(out, cl.events[j].second) == expected.events[j].span.time);
        CHECK(at64(out, cl.events[j].second + 8) == expected.events[j].span.duration);
        if (expected.events[j].type == motionpack::EventType::LerpMovementFactor) CHECK(at64(out, cl.events[j].second + 0x70) == expected.events[j].lerp_duration);
        // everything else of the event is copied: opaque payload bytes, flags, guards
        const size_t p = cl.events[j].second, q = in.events[j].second;
        CHECK(std::memcmp(out.data() + p + 16, raw.data() + q + 16, 0x50) == 0);
    }
    CHECK(at64(out, cl.events[0].second) == 0);  // zero stays zero
    CHECK(out[cl.sub + 0x10] == raw[in.sub + 0x10]);  // the loop flag is kept

    struct Refuse { const char* name; std::function<void(Bytes&)> change; const motionclose::Source* as; const char* says; };
    const auto& door = source("door");
    for (const auto& c : std::vector<Refuse>{
             {"an event outside the whitelist", [&](Bytes& b) { body_fixture::put32(b, 16 + static_cast<int32_t>(body_fixture::get32(b, 16)) +
                  static_cast<int32_t>(body_fixture::get32(b, parse(b).sub)), body_fixture::hash("keen::actor::SpawnEvent")); }, &sit, "outside the verified whitelist"},
             {"a changed event size", [&](Bytes& b) { const size_t s0 = parse(b).sub; body_fixture::put32(b, s0 + static_cast<int32_t>(body_fixture::get32(b, s0)) + 8, 112); }, &sit, "changed size"},
             {"fewer animation events than the manifest", [](Bytes&) {}, &door, "the manifest 3"},
             {"an event past the sub-sequence", [&](Bytes& b) { const auto q = parse(b); const int64_t t = at64(b, q.sub + 8) + 1; std::memcpy(b.data() + q.events[2].second, &t, 8); }, &sit, "outside the SubSequence"},
             {"a truncated sequence", [](Bytes& b) { b.resize(40); }, &sit, "sub-sequence array is invalid"}}) {
        Bytes bytes = c.as == &door ? body_fixture::sequence(source("jump")) : raw;
        c.change(bytes);
        Bytes kept{0x5a};
        CHECK(!motionclose::clone_sequence(bytes.data(), bytes.size(), clone, *c.as, bound[c.as->role], [&](const Guid&) { return sub; }, &kept, &why));
        if (why.find(c.says) == std::string::npos) std::printf("clone (%s) said: %s\n", c.name, why.c_str());
        CHECK(why.find(c.says) != std::string::npos);
        CHECK(kept == Bytes{0x5a});
    }

    // The template: every verified reference remapped; zero stays; unknown or leftover sources refuse, unchanged.
    std::map<std::string, Guid> map;
    for (const auto& s : motionclose::fisher_closure()) map[s.guid] = derive_guid("m", "npcmotion", s.guid);
    Bytes tpl = body_fixture::fisher();
    size_t replaced = 0;
    CHECK(motionclose::remap_template(&tpl, map, &replaced, &why));
    CHECK(replaced == 22);
    CHECK(body_fixture::get32(tpl, body_fixture::component(tpl, "PathWalker") + 0x24) == 0);  // teleport stays zero
    Guid sit_ref;
    std::memcpy(sit_ref.b, tpl.data() + body_fixture::component(tpl, "ActiveNpcState") + 4, 16);
    CHECK(sit_ref == map[sit.guid]);
    for (const auto& c : std::vector<std::pair<const char*, std::function<void(Bytes&)>>>{
             {"is not in the verified Fisher motion closure", [](Bytes& b) { std::memset(b.data() + body_fixture::component(b, "NpcState") + 0x10, 0x77, 16); }},
             {"remains at byte", [&](Bytes& b) { Guid g; parse_guid(sit.guid, &g); std::memcpy(b.data() + body_fixture::component(b, "LocaTagComponent"), g.b, 16); }},
             {"NpcActionSequences: missing or changed", [](Bytes& b) {
                  const size_t h = 16 + static_cast<int32_t>(body_fixture::get32(b, 16));
                  for (uint32_t i = 0; i < body_fixture::get32(b, 20); ++i)
                      if (body_fixture::get32(b, h + i * 12) == body_fixture::hash("keen::ecs::NpcActionSequences")) body_fixture::put32(b, h + i * 12 + 8, 40); }}}) {
        Bytes bad = body_fixture::fisher();
        c.second(bad);
        const Bytes before = bad;
        CHECK(!motionclose::remap_template(&bad, map, &replaced, &why));
        if (why.find(c.first) == std::string::npos) std::printf("remap said: %s\n", why.c_str());
        CHECK(why.find(c.first) != std::string::npos);
        CHECK(bad == before);
    }
}
}  // namespace

int main() {
    layout_check();
    motion_closure_units();
    lowered_clone_is_published_under_its_mod();
    refused_or_withdrawn_body_mods_leave_their_neighbours();
    missing_lowerer_and_collection_refusals();
    admission_of_the_local_cache();
    std::printf("body runtime: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
