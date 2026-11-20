// SPDX-License-Identifier: MIT
// Compile the real manifest reader and ownership decision on the host; discard unused game-memory paths at link time.
#include "../refs.cpp"
#include <cstdarg>
#include <chrono>
#include <stdexcept>

namespace kc {
const KilnApi* g_api = nullptr;
std::string test_mods_dir;
std::vector<std::string> test_warnings;
std::string mods_dir() { return test_mods_dir; }
void logf(int, const char* fmt, ...) {
    char text[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    test_warnings.emplace_back(text);
}
std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
// What resolve() reaches on the host: no game memory (rd refuses), no assets service, no schema fields.
uint32_t fnv1a(const void* data, size_t n) { uint32_t h=2166136261u; for(size_t i=0;i<n;++i)h=(h^static_cast<const uint8_t*>(data)[i])*16777619u; return h; }
bool rd(uintptr_t, void*, size_t) { return false; }
const KilnAssetsApi* test_assets = nullptr;  // null: no assets service (every test but served_impact_programs)
const KilnAssetsApi* assets_api() { return test_assets; }
const Field* Schema::find(const std::string&) const { return nullptr; }
// Host fixture GUID decoder (same Windows byte layout as game.cpp; no game memory involved).
bool guid_parse(const std::string& s, uint8_t out[16]) {
    unsigned a, b, c, d[8];
    if (s.size() != 36 || std::sscanf(s.c_str(), "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &b, &c,
        &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]) != 11) return false;
    const uint32_t aa = a;
    const uint16_t bb = b, cc = c;
    std::memcpy(out, &aa, 4);
    std::memcpy(out + 4, &bb, 2);
    std::memcpy(out + 6, &cc, 2);
    for (int i = 0; i < 8; ++i) out[8 + i] = static_cast<uint8_t>(d[i]);
    return true;
}
}

namespace {
int checks = 0;
void check(bool yes, const char* name) {
    if (!yes) throw std::runtime_error(name);
    ++checks;
}
KilnWorldTablesApi tables{};
std::string pool_status;
void table_references() {
    using namespace kc;
    KilnApi api{};api.service=[](KilnStr name,uint32_t)->const void* {
        return std::string(name.ptr,name.len)==KILN_WORLDTABLES_SERVICE?&tables:nullptr;
    };g_api=&api;
    tables.header={144,1};tables.state=[]()->int32_t{return KILN_WT_MERGED;};
    tables.material_id=[](const uint8_t*)->int32_t{return 213;};
    tables.status_json=[](char* out,size_t capacity,size_t* count)->KilnResult {
        *count=pool_status.size();if(!out || capacity<=*count)return KILN_ERR_BUFFER_TOO_SMALL;
        std::memcpy(out,pool_status.c_str(),pool_status.size()+1);return KILN_OK;
    };
    uint8_t guid[16];guid_parse("11111111-2222-4333-8444-555555555555",guid);
    pool_status=R"({"state":"merged","customPools":[{"guid":"11111111-2222-4333-8444-555555555555"}],"shapeBindings":[{"item":"22222222-2222-4222-8222-222222222222","pool":"11111111-2222-4333-8444-555555555555"}]})";
    uint8_t shape[16];guid_parse("22222222-2222-4222-8222-222222222222",shape);
    check(world_material(guid),"old worldtables prefix still admits material references");
    check(world_pool(guid,shape),"served custom pool admits its 16-byte GUID for the shape bound to it");
    guid[0]^=1;check(!world_pool(guid,shape),"unknown pool GUID is refused");guid[0]^=1;
    tables.header.size=offsetof(KilnWorldTablesApi,status_json);check(!world_pool(guid,shape),"short table never calls status_json");
    tables.header.size=144;pool_status=R"({"state":"off","customPools":[{"guid":"11111111-2222-4333-8444-555555555555"}]})";
    check(!world_pool(guid,shape),"off tables cannot authorize pool references");
    pool_status=R"({"state":"merged","customPools":[{"guid":42}]})";check(!world_pool(guid,shape),"malformed snapshot refuses without throwing");
    tables.status_json=nullptr;check(!world_pool(guid,shape),"missing member refuses without a null call");
    tables.header.size=offsetof(KilnWorldTablesApi,material_id);check(!world_material(guid),"short material table refuses before call");
    g_api=nullptr;
}
// Item 11: a mod's pool GUID on an item's equipment.voxelBlueprintMaterialPool only for the shape worldtables bound to it
// (its own mod's or a dependency's pool, roof agreeing; and that binding is in the handshake hash): not for a vanilla shape,
// another mod's item, or a roof shape on a wall pool.
void pool_references_need_their_binding() {
    using namespace kc;
    KilnApi api{};api.service=[](KilnStr name,uint32_t)->const void* {
        return std::string(name.ptr,name.len)==KILN_WORLDTABLES_SERVICE?&tables:nullptr;
    };g_api=&api;
    tables.header={sizeof(KilnWorldTablesApi),1};tables.state=[]()->int32_t{return KILN_WT_MERGED;};
    tables.status_json=[](char* out,size_t capacity,size_t* count)->KilnResult {
        *count=pool_status.size();if(!out || capacity<=*count)return KILN_ERR_BUFFER_TOO_SMALL;
        std::memcpy(out,pool_status.c_str(),pool_status.size()+1);return KILN_OK;
    };
    pool_status=R"({"state":"merged","customPools":[{"guid":"11111111-2222-4333-8444-555555555555"}],)"
                R"("shapeBindings":[{"item":"22222222-2222-4222-8222-222222222222","pool":"11111111-2222-4333-8444-555555555555"}]})";
    Schema s;Field f;f.path="equipment.voxelBlueprintMaterialPool";f.type_name="keen::ObjectReference<keen::VoxelBlueprintMaterialPool>";f.bytes=16;
    Pools p;uint8_t out[16];std::string why;
    const ItemFind none=[](const std::string&)->uintptr_t{return 0;};
    check(!resolve(s,f,"11111111-2222-4333-8444-555555555555",none,p,out,nullptr,nullptr,&why),"a served pool outside a shape bound to it is refused");
    check(why.find("material pool")!=std::string::npos && why.find("bound")!=std::string::npos,"and the refusal says it is a mod's pool the item is not bound to");
    uint8_t shape[16],other[16];guid_parse("22222222-2222-4222-8222-222222222222",shape);guid_parse("33333333-3333-4333-8333-333333333333",other);
    set_kitbash_item(shape);
    check(resolve(s,f,"11111111-2222-4333-8444-555555555555",none,p,out,nullptr,nullptr,&why),"the shape bound to the pool takes it");
    set_kitbash_item(other);
    check(!resolve(s,f,"11111111-2222-4333-8444-555555555555",none,p,out,nullptr,nullptr,&why),"another item (a vanilla shape, another mod's) does not");
    uint8_t pool[16];guid_parse("11111111-2222-4333-8444-555555555555",pool);
    p.by_type[f.type_name].insert(gof(pool)); // a previous bound shape now uses it in the loaded item registry
    check(!resolve(s,f,"11111111-2222-4333-8444-555555555555",none,p,out,nullptr,nullptr,&why),"a loaded shape's pool cannot bypass another item's binding check");
    set_kitbash_item(shape);
    check(resolve(s,f,"11111111-2222-4333-8444-555555555555",none,p,out,nullptr,nullptr,&why),"a cached pool still works on its bound shape");
    set_kitbash_item(nullptr);
    check(!resolve(s,f,"11111111-2222-4333-8444-555555555555",none,p,out,nullptr,nullptr,&why),"a cached pool cannot be assigned by a game-item rule");
    g_api=nullptr;
}
void malformed_entries() {
    using namespace kc;
    for (const char* key : {"kind", "type", "guid"}) {
        for (const json& bad : {json(nullptr), json(42), json(false), json::array(), json::object()}) {
            json entry = {{"name", "bad-model"}, {"kind", "resource"}, {"type", "keen::RenderModel"}, {"guid", "11111111-2222-4333-8444-555555555555"}};
            entry[key] = bad;
            std::unordered_set<G, GH> models;
            const size_t before = test_warnings.size();
            try { declared_models(json{{"entries", json::array({entry})}}, &models); }
            catch (const json::exception& e) { throw std::runtime_error(std::string("malformed ") + key + " threw: " + e.what()); }
            check(models.empty(), "malformed entry must not declare a model");
            check(test_warnings.size() == before + 1, "malformed entry must be logged once");
            check(test_warnings.back().find("bad-model") != std::string::npos, "warning must name the entry");
            check(test_warnings.back().find(key) != std::string::npos, "warning must name the invalid field");
        }
    }
}

void own_mod_models(const std::filesystem::path& root) {
    using namespace kc;
    check(!fs::exists(root), "fixture directory must be new");
    struct Fixture {
        fs::path path;
        ~Fixture() {
            set_kitbash_mod(nullptr);
            g_mod_folders.clear();
            g_decls.clear();
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } fixture{root};
    test_mods_dir = root.string();
    const std::string own = "11111111-2222-4333-8444-555555555555";
    const std::string other_type = "21111111-2222-4333-8444-555555555555";
    const std::string other_kind = "31111111-2222-4333-8444-555555555555";
    const std::string foreign = "41111111-2222-4333-8444-555555555555";
    const std::string unloaded = "51111111-2222-4333-8444-555555555555";
    const std::string unknown = "61111111-2222-4333-8444-555555555555";
    json manifest = {{"entries", json::array({
        json{{"kind", "resource"}, {"type", "keen::RenderModel"}, {"guid", own}},
        json{{"kind", "resource"}, {"type", "keen::JointAnimation"}, {"guid", other_type}},
        json{{"kind", "texture"}, {"type", "keen::RenderModel"}, {"guid", other_kind}},
        json{{"name", "bad-guid"}, {"kind", "resource"}, {"type", "keen::RenderModel"}, {"guid", nullptr}}
    })}};
    auto write_mod = [&](const std::string& id, const json& assets) {
        const auto folder = root / id;
        fs::create_directories(folder / "assets");
        std::ofstream(folder / "kiln.mod.json") << json{{"id", id}};
        std::ofstream(folder / "assets/assets.json") << assets;
    };
    write_mod("owner", manifest);
    write_mod("other", json{{"entries", json::array({json{{"kind", "resource"}, {"type", "keen::RenderModel"}, {"guid", foreign}}})}});
    write_mod("unloaded", json{{"entries", json::array({json{{"kind", "resource"}, {"type", "keen::RenderModel"}, {"guid", unloaded}}})}});
    std::ofstream(root / "unloaded/.disabled");
    KilnApi api{};
    api.service = [](KilnStr, uint32_t) -> const void* { return nullptr; };
    g_api = &api;

    std::unordered_set<G, GH> models;
    const size_t before = test_warnings.size();
    declared_models(read_jsonc(root / "owner/assets/assets.json"), &models);
    check(models.size() == 1, "only resource RenderModel entries declare models");
    check(test_warnings.size() == before + 1, "non-string GUID is skipped and logged once");
    auto accepted = [&](const std::string& text) {
        uint8_t g[16];
        check(guid_parse(text, g), "fixture GUID must parse");
        return declared_by_own_mod(g);
    };
    check(!accepted(own), "pack rules without a mod cannot use declared models");
    const std::string owner = "owner";
    set_kitbash_mod(&owner);
    check(accepted(own), "the item's own mod's declared model is accepted");
    check(!accepted(other_type), "another resource type is refused");
    check(!accepted(other_kind), "another entry kind is refused");
    check(!accepted(foreign), "a model only another loaded mod declares is refused");
    check(!accepted(unloaded), "a model only a disabled mod declares is refused");
    check(!accepted(unknown), "an unknown GUID is refused");
    const size_t cached_warnings = test_warnings.size();
    check(accepted(own), "the cached manifest still accepts its model");
    check(test_warnings.size() == cached_warnings, "cached malformed entry is not logged again");
    const std::string missing_mod = "unloaded";
    set_kitbash_mod(&missing_mod);
    check(!accepted(unloaded), "a disabled mod's folder cannot authorize its model");
    set_kitbash_mod(&owner);
    const auto mf = root / "owner/assets/assets.json";
    const auto stamp = fs::last_write_time(mf);
    std::ofstream(mf) << json{{"entries", json::array()}};
    fs::last_write_time(mf, stamp + std::chrono::seconds(1));
    check(!accepted(own), "hot reload removes a model declaration from the cache");
    g_api = nullptr;
}
}

// A gem's equipment.program (keen::ImpactProgramReference) names a program a mod serves through kiln.assets as a
// keen::ImpactProgram (gem wave G2: the Kiln gem trigger programs): asset_served asks kiln.assets for that type.
uint32_t asked_type = 0;
void served_impact_programs() {
    using namespace kc;
    static KilnAssetsApi a{};
    a.header.size = sizeof a;
    a.resource_read = [](const KilnAssetKey* k, void*, size_t, size_t* n) -> KilnResult {
        asked_type = k->type;
        *n = 64;
        return k->type == 0xd4922d88u && k->guid[0] == 0x6e ? KILN_ERR_BUFFER_TOO_SMALL : KILN_ERR_NOT_FOUND;
    };
    test_assets = &a;
    uint8_t g[16] = {0x6e, 0x6c, 0x69, 0x6b};
    check(asset_served("keen::ImpactProgramReference", g), "an ImpactProgramReference to a served keen::ImpactProgram is loaded");
    check(asked_type == 0xd4922d88u, "ImpactProgramReference asks kiln.assets for keen::ImpactProgram (0xd4922d88)");
    g[0] = 0x11;
    check(!asset_served("keen::ImpactProgramReference", g), "an ImpactProgramReference nobody serves is not loaded");
    check(asset_served("keen::ObjectReference<keen::ImpactProgram>", (const uint8_t[16]){0x6e}), "ObjectReference<ImpactProgram> as before");
    test_assets = nullptr;
}

// Gem wave integration: a removed mod's trigger gem. Its "[Missing: ...]" placeholder keeps the recorded references;
// its equipment.program names a Kiln trigger program (or a program its mod cooked) that nothing serves once the mod is
// gone. Kept as the base's value, the placeholder ran the base gem's program (a Frostbite acted as a Gem of Bleeding).
// A placeholder names no program instead (the game's gem spawner runs nothing for a gem without a program), while every
// other unresolved reference keeps the base's value as before, and a program that does resolve is kept.
void placeholder_programs() {
    using namespace kc;
    static KilnAssetsApi a{};
    a.header.size = sizeof a;
    a.resource_read = [](const KilnAssetKey* k, void*, size_t, size_t* n) -> KilnResult {
        *n = 64;
        return k->type == 0xd4922d88u && k->guid[0] == 0x6e && k->guid[15] == 0x02 ? KILN_ERR_BUFFER_TOO_SMALL : KILN_ERR_NOT_FOUND;
    };
    test_assets = &a;
    Schema s;
    Pools p;
    Field prog;
    prog.path = "equipment.program", prog.type_name = "keen::ImpactProgramReference", prog.bytes = 16, prog.ref = true;
    Field anim;
    anim.path = "equipment.equipmentEntityModelSetup.animation", anim.type_name = "keen::ObjectReference<keen::JointAnimation>",
    anim.bytes = 16, anim.ref = true;
    const ItemFind none = [](const std::string&) -> uintptr_t { return 0; };
    const std::string gone = "6b696c6e-6765-6d73-8000-000000000003";    // on_kill: no mod serves it now
    const std::string served = "6b696c6e-6765-6d73-8000-000000000002";  // on_crit: another mod's gem serves it
    const auto write_of = [](const KitbashOut& o, const Field* f) -> const RefWrite* {
        for (const RefWrite& w : o.writes)
            if (w.f == f) return &w;
        return nullptr;
    };
    const uint8_t zero[16] = {};

    KitbashOut ko;
    one_ref(s, prog, json(gone), none, p, &ko);
    one_ref(s, anim, json("61111111-2222-4333-8444-555555555555"), none, p, &ko);
    check(ko.problems.size() == 2 && !write_of(ko, &prog) && !write_of(ko, &anim), "unresolved references are problems, not writes");
    check(placeholder_clear_programs(&ko) == 1, "a placeholder clears its one unresolved program reference");
    const RefWrite* w = write_of(ko, &prog);
    check(w && std::memcmp(w->bytes, zero, 16) == 0, "the placeholder's program is none, not the base's");
    check(!write_of(ko, &anim), "another unresolved reference keeps the base's value");
    check(placeholder_clear_programs(&ko) == 1 && ko.writes.size() == 1, "clearing twice writes the field once");

    KitbashOut ok;
    one_ref(s, prog, json(served), none, p, &ok);
    check(ok.problems.empty() && placeholder_clear_programs(&ok) == 0, "a program another mod serves resolves: nothing cleared");
    w = write_of(ok, &prog);
    uint8_t want[16];
    guid_parse(served, want);
    check(w && std::memcmp(w->bytes, want, 16) == 0, "the placeholder runs that program (a trigger with no handler does nothing)");

    KitbashOut last;  // "program" then "refs" on one field: the later value wins, as for the writes
    one_ref(s, prog, json(gone), none, p, &last);
    one_ref(s, prog, json(served), none, p, &last);
    check(placeholder_clear_programs(&last) == 0, "a program reference resolved by a later key is not cleared");
    KitbashOut late;
    one_ref(s, prog, json(served), none, p, &late);
    one_ref(s, prog, json(gone), none, p, &late);
    check(placeholder_clear_programs(&late) == 1 && write_of(late, &prog) && std::memcmp(write_of(late, &prog)->bytes, zero, 16) == 0,
          "a later unresolved program reference is cleared over an earlier resolved one");

    Field oref = prog;
    oref.type_name = "keen::ObjectReference<keen::ImpactProgram>";
    KitbashOut obj;
    one_ref(s, oref, json(gone), none, p, &obj);
    check(placeholder_clear_programs(&obj) == 1, "an ObjectReference<keen::ImpactProgram> is a program reference too");
    test_assets = nullptr;
}

int main(int argc, char** argv) {
    try {
        table_references();
    pool_references_need_their_binding();
        malformed_entries();
        served_impact_programs();
        placeholder_programs();
        if (argc != 2) throw std::runtime_error("provide a new fixture directory");
        own_mod_models(argv[1]);
        std::printf("refs-test: %d checks passed, 0 failed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "refs-test: FAIL: %s\n", e.what());
        return 1;
    }
}
