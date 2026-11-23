// SPDX-License-Identifier: MIT
// 16f S3: authored roster bodies through the real compiler and the real per-mod publisher (synthetic resources, a
// synthetic Fisher chassis), and the admission of a mod's local body cache (bodyadmit.cpp) on in-memory mod files.
#define main memory_regression_main
#include "bootpack_memory_test.cpp"
#undef main

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
        {"keen::ecs::PathWalker", [](BodyTypeFacts& f) { f.fields[0].offset += 4; }, "keen::ecs::PathWalker.horizontalRadius moved"},
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
    std::map<uint64_t, std::string> owner_mod;
    std::vector<std::string> admitted, published;
    bool layout_ok = true;
    std::string refuse_admission, fail_body_put, fail_finish;
    World() {
        backend = store.backend();
        const auto read = backend.game.read;
        backend.game.read = [this, read](const Guid& g, uint32_t type, const uint8_t** p, size_t* n) {
            if (type == kTypeTemplate && g == guid(kFisherGuid)) { *p = fisher.data(); *n = fisher.size(); return true; }
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
                                           {"bindings", ojson::array()}, {"closure", "c"}};
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
const uint32_t kModelType = fnv1a("keen::RenderModel", std::strlen("keen::RenderModel"));
const uint32_t kGraphTypeHash = fnv1a(kGraphType, std::strlen(kGraphType));

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
    std::set<std::pair<std::string, uint32_t>> served;
    ojson cache, spec;
    std::string closure = std::string(64, 'c');
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
        e.served = [this](const Guid& g, uint32_t type) { return served.count({guid_text(g), type}) != 0; };
        e.materials.server = !client;
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
        std::printf("-- 16f S3 admission: a body defined in code needs its cooked resources served (the model only on clients)\n");
        Mod m;
        ModInput in;
        in.id = "code-mod";
        in.authored_body = true;
        const ojson code_body = {{"model", kModel}, {"graph", kGraph}, {"skeleton", kSkeleton}, {"size", {{"radius", .5}, {"height", 1.5}}}};
        const ojson npc = {{"template", kBodyChassis}, {"body", code_body}};
        in.documents = {ojson{{"npcs", {{"bram", npc}}}}};
        ojson assets; std::string why;
        ModInput first = in;
        CHECK(!bodyadmit::admit(&first, m.env(), &assets, &why));
        CHECK(why.find("graph is not served") != std::string::npos);
        m.served.insert({kGraph, kGraphTypeHash});
        ModInput second = in;
        CHECK(!bodyadmit::admit(&second, m.env(), &assets, &why));
        CHECK(why.find("model is not served on this game client") != std::string::npos);
        m.client = false;
        ModInput server = in;
        CHECK(bodyadmit::admit(&server, m.env(), &assets, &why));
        CHECK(assets["entries"].empty());  // its resources are the mod's ordinary assets.json entries
        CHECK(server.documents[0]["npcs"]["bram"]["body"].value("model", "") == kModel);
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
}  // namespace

int main() {
    layout_check();
    lowered_clone_is_published_under_its_mod();
    refused_or_withdrawn_body_mods_leave_their_neighbours();
    missing_lowerer_and_collection_refusals();
    admission_of_the_local_cache();
    std::printf("body runtime: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
