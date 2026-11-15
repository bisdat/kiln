// SPDX-License-Identifier: MIT
// Boot-time resources for mods' quests and NPCs in the game's UI (docs/kiln/services/npcs.md): reads every enabled mod's
// quests/*.json, compiles what goes to the game (questpack.cpp) and registers the rebuilt GameKnowledgeResource,
// JournalRegistryResource, NpcCollection and templates with kiln.assets, before the game reads them.
//
// When: right after the game's resource service is up (a post-hook on ResourceInit: nothing can have read a resource
// yet; on a client whose service was up before Kiln loaded, in on_ready). Every reader in the game goes through the
// resource service's slots (key -> handle -> data), which kiln.assets answers: the registries keep the handle and ask
// for the data when their world loads (docs/re/quests.md, "R4").
//
// When the WHOLE build fails, so that nothing of the mods' native content is served (reading their files fails, the
// build throws, the shared resources do not build, kiln.assets refuses the pack or is not there, the hook on
// ResourceInit is not there), the start stops before any world loads (refuse_start, StartStop::Content), as long as an
// enabled mod ships such content: a world loaded without it drops the mods' progress at its next save. One mod's own
// errors (its manifest entry ok: false) leave that mod out, and the start goes on (docs/kiln/services/npcs.md).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <set>
#include <tuple>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "knowledge_internal.h"
#include "questpack.h"
#include "start_stop.h"
#include "kiln_boot_collector.hpp"
#include "kiln_hook.hpp"
#include "services/kiln_assets.h"
#include "services/kiln_content.h"
#include "services/kiln_profile.h"

namespace fs = std::filesystem;

namespace kiln::knowledge {

namespace {

std::mutex g_mu;
bool g_built = false;
std::string g_manifest = "{\"mods\":[],\"errors\":[]}";
std::unordered_map<std::string, uint32_t> g_names;  // "<mod>:<name>" (lower case) -> knowledge id
kiln::RawHook g_init_hook;
const KilnAssetsApi* A = nullptr;

// kiln/control/knowledge.fail-build (labs and tests only, never needed in play; read once at the start, loud while
// present): one kind of whole-build failure without a broken game. Its first word: read (reading the mods' files
// throws), compile (the build throws), shared (the build cannot read the game's GameKnowledgeResource, so the shared
// resources do not build), assets (the built pack is not given to kiln.assets, as when it refuses), hook (the hook on
// ResourceInit is not installed), late (the build serves as usual; 30 steps after the server world's first one the
// stop is asked for, which must not end the process). Each stops the start only when a mod ships such content, as the
// real failure does.
enum class Fail : uint8_t { None, Read, Compile, Shared, Assets, Hook, Late };
Fail g_fail = Fail::None;
const std::string kFailSwitch = "lab switch kiln/control/knowledge.fail-build";
std::string g_no_hook;              // why the hook on ResourceInit is not there: nothing builds the pack at the resource start
std::vector<std::string> g_served;  // the mods whose content the build served (under g_mu)
std::atomic<uint32_t> g_late_posts{0};

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
std::string path_of(int32_t which) {
    char buf[1024];
    size_t n = 0;
    if (g_api->path(which, buf, sizeof buf, &n) != KILN_OK) return {};
    return std::string(buf, n);
}

bool res_read(const qb::Guid& g, uint32_t type, const uint8_t** p, size_t* n) {
    KilnAssetKey k{};
    std::memcpy(k.guid, g.b, 16);
    k.type = type;
    const void* data = nullptr;
    size_t len = 0;
    if (A->resource_data(&k, &data, &len) != KILN_OK || !data) return false;
    *p = static_cast<const uint8_t*>(data);
    *n = len;
    return true;
}

// Template names -> GUIDs, built once when a name is first asked for (every TemplateResource's name field).
std::unordered_map<std::string, qb::Guid> g_template_names;
bool template_by_name(const std::string& name, qb::Guid* out) {
    if (g_template_names.empty()) {
        size_t count = 0;
        A->resource_list(qb::kTypeTemplate, nullptr, 0, &count);
        std::vector<KilnAssetKey> keys(count);
        if (count && A->resource_list(qb::kTypeTemplate, keys.data(), keys.size(), &count) == KILN_OK) {
            for (const KilnAssetKey& k : keys) {
                const void* d = nullptr;
                size_t l = 0;
                if (A->resource_data(&k, &d, &l) != KILN_OK || !d) continue;
                qb::Guid g;
                std::memcpy(g.b, k.guid, 16);
                g_template_names.emplace(qb::template_name(static_cast<const uint8_t*>(d), l), g);
            }
        }
    }
    auto it = g_template_names.find(name);
    if (it == g_template_names.end()) return false;
    *out = it->second;
    return true;
}

// Bound the read itself too: a block file can grow after file_size, and must not exhaust the game's memory at startup.
std::string read_block_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::string text;
    char buffer[8192];
    while (file) {
        file.read(buffer, sizeof buffer);
        const size_t n = static_cast<size_t>(file.gcount());
        if (text.size() + n > qb::kMaxBlockFile) return {};
        text.append(buffer, n);
    }
    return file.bad() ? std::string{} : text;
}

// The NPC blocks of a mod's blocks/*.json ("npcs"), in the quests' file format, so the game builds them at the first start: what Kiln.Blocks'
// NpcCompiler.NpcFile writes for an NPC block, the same ids (UUIDv5 of <mod>:npc:<name>, <mod>:item:<name>.tool) and the same body premades.
// An NPC block written in code is not visible here: Kiln.Blocks writes it to the data folder, and the game builds it from the next start.
qb::ojson npcs_from_blocks(const std::string& mod, const std::vector<qb::ojson>& docs) {
    struct Premade {
        const char *key, *tmpl, *workshop, *tool;
    };
    static const Premade kBodies[] = {
        {"fisher", "NPC_Workshop_Fisher_Trader", "0xc4acc104", "NPC_Townsfolk_Male_Villager_08_fisher_Trader"},
        {"villager", "NPC_Workshop_Fisher_Trader", "0xc4acc104", "NPC_Townsfolk_Male_Villager_08_fisher_Trader"},
        {"vukah", "Enemy_Vukah_Melee", "", ""},
        {"scavenger", "Enemy_Scavenger_Melee01", "", ""},
    };
    std::map<std::string, qb::ojson> bodies;
    for (const qb::ojson& d : docs)
        if (d.contains("bodies") && d["bodies"].is_object())
            for (const auto& [k, v] : d["bodies"].items()) bodies[k] = v;
    // a body: a premade's key, a "bodies" block by name, or one inline; "like" starts from a premade
    struct Body {
        std::string tmpl, workshop, tool;
        bool own = false;
    };
    const auto premade = [&](const std::string& key, Body* b) {
        for (const Premade& p : kBodies)
            if (key == p.key) {
                b->tmpl = p.tmpl, b->workshop = p.workshop, b->tool = p.tool;
                return true;
            }
        return false;
    };
    const auto body_of = [&](const qb::ojson& v, Body* b) {
        const qb::ojson* o = &v;
        if (v.is_string()) {
            if (premade(v.get<std::string>(), b)) return true;
            auto it = bodies.find(v.get<std::string>());
            if (it == bodies.end()) return false;
            o = &it->second;
        }
        if (!o->is_object()) return false;
        if (o->contains("like") && (*o)["like"].is_string() && !premade((*o)["like"].get<std::string>(), b)) return false;
        for (auto [key, field] : {std::pair{"template", &b->tmpl}, std::pair{"workshop", &b->workshop}, std::pair{"tool", &b->tool}})
            if (o->contains(key) && (*o)[key].is_string()) *field = (*o)[key].get<std::string>();
        b->own = o->contains("model");
        return !b->tmpl.empty();
    };
    qb::ojson out = qb::ojson::object();
    for (const qb::ojson& d : docs) {
        if (!d.contains("npcs") || !d["npcs"].is_object()) continue;
        for (const auto& [name, n] : d["npcs"].items()) {
            Body b;
            if (!n.is_object() || !n.contains("name") || !n["name"].is_string() || !n.contains("body") || !body_of(n["body"], &b) || b.own) continue;  // Kiln.Blocks says what is wrong
            const std::string key = n.contains("was") && n["was"].is_string() ? n["was"].get<std::string>() : name;
            const std::string clone = qb::guid_text(qb::derive_guid(mod, "npc", key));
            qb::ojson o = {{"template", b.tmpl}, {"clone", {{"guid", clone}, {"name", "Kiln_" + mod + "_" + key}}}};
            qb::ojson names = {{"en", n["name"]}};
            if (n.contains("names") && n["names"].is_object())
                for (const auto& [lang, text] : n["names"].items()) names[lang] = text;
            o["names"] = names;
            if (n.contains("talk") && n["talk"].is_string()) o["dialogue"] = n["talk"];
            const qb::ojson roster = n.value("roster", qb::ojson());
            if (roster.is_object() && roster.contains("description")) o["description"] = {{"en", roster["description"]}};
            if (n.contains("place") && n["place"].is_object()) {
                const qb::ojson& p = n["place"];
                const auto flag = [&p](const char* k, bool otherwise) { return p.contains(k) && p[k].is_boolean() ? p[k].get<bool>() : otherwise; };
                qb::ojson po = {{"once", flag("once", true)}, {"unlock", false}};
                if (p.contains("at")) po["at"] = p["at"];
                else po["near"] = "player";
                if (p.contains("distance")) po["distance"] = p["distance"];
                po["inBase"] = flag("inBase", true);  // an NPC block is friendly: a base is fine unless it says no
                if (flag("exact", false)) po["exact"] = true;
                o["place"] = po;
            }
            if (roster.is_object() && !b.tool.empty()) {
                qb::ojson ro = {{"summonItem",
                                 {{"guid", qb::guid_text(qb::derive_guid(mod, "item", key + ".tool"))},
                                  {"name", mod + "." + key + ".tool"},
                                  {"base", b.tool},
                                  {"texts", {{"en", {{"caption", roster.value("summonTool", n["name"])}}}}},
                                  {"refs", {{"equipment.placedEntity", clone}}}}}};
                if (roster.contains("unlockedBy")) ro["unlock"] = roster["unlockedBy"];
                if (roster.contains("roamingRange")) ro["roamingRange"] = roster["roamingRange"];
                if (!b.workshop.empty()) ro["workshop"] = b.workshop;
                o["roster"] = ro;
            }
            out[key] = o;
        }
    }
    return out;
}

// Every enabled mod of the load order with a quests/ folder, as kiln.assets finds its assets.json. `boot`: a boot generation's prepare
// (chapter 08): a mod it excludes is left out, and each mod's OnBoot documents (the quests file's format) follow its own files and blocks,
// before what its code defined at an earlier start.
// A mod's input holds native quests, knowledge or NPCs: a document with a "knowledge", "quests", "dialogues" or "npcs"
// entry that is not empty. An input without (no files, empty files) never stops the start.
bool ships(const qb::ModInput& m) {
    for (const qb::ojson& d : m.documents)
        if (d.is_object())
            for (const char* k : {"knowledge", "quests", "dialogues", "npcs"})
                if (d.contains(k) && (d[k].is_object() || d[k].is_array()) && !d[k].empty()) return true;
    return false;
}

// `reading`: the id of the mod being read once it has files of such content (a throw names it).
void collect_mods(std::vector<qb::ModInput>* mods, bool client, const kiln::boot::Input* boot = nullptr, std::string* reading = nullptr) {
    auto one = [&](const std::string& dir) {
        if (reading) reading->clear();
        std::error_code ec;
        const fs::path base(widen(dir));
        const fs::path qdir = base / L"quests";
        const qb::ojson mf = qb::ojson::parse(read_file(base / L"kiln.mod.json"), nullptr, false, true);
        if (mf.is_discarded() || !mf.is_object() || !mf.contains("id") || !mf["id"].is_string()) {
            if (fs::is_directory(qdir, ec)) KW("native: %s has quests/ but no readable kiln.mod.json: skipped", dir.c_str());
            return;
        }
        // a required participant prepares strictly (kiln_boot_collector.hpp, entry_error): a file of its own that cannot be read or is not
        // JSON refuses it; another mod's is logged and skipped as it always was
        const std::string mod_id = mf["id"].get<std::string>();
        const auto strict = [&](const std::string& key, const std::string& why) {
            kiln::boot::entry_error(boot, mod_id, "knowledge", key, "kiln.knowledge: " + key + ": " + why);
        };
        if (boot && boot->out(mf["id"].get<std::string>())) {
            KI("native: %s is out of the boot generation: its quests and NPCs are not built", mf["id"].get_ref<const std::string&>().c_str());
            return;
        }
        std::vector<std::pair<std::string, qb::ojson>> boot_docs;
        if (boot)
            for (const auto& b : boot->docs("knowledge", mf["id"].get<std::string>())) {
                if (!b.document.is_object())  // a document this collector cannot read refuses its mod (kiln.boot drops it, or stops the start)
                    throw kiln::boot::Refusal(mf["id"].get<std::string>(), "knowledge", b.key, "kiln.knowledge: a boot document is not a quests file (a JSON object)");
                boot_docs.emplace_back("boot:" + b.key, qb::ojson::parse(b.document.dump(), nullptr, false));
            }
        // what the mod defined in code at an earlier start (Kiln.Quests/Native/NativeExport.cs)
        const fs::path code = fs::path(widen(path_of(KILN_PATH_DATA))) / L"kiln-quests" / L"native" / widen(mf["id"].get<std::string>() + ".json");
        // the NPC blocks it defined (Kiln.Blocks, Npc/NpcCompiler.cs), kept beside it so that a redeploy of the mod keeps them; the mod's own files win
        const fs::path npc_blocks = fs::path(code).replace_filename(widen(mf["id"].get<std::string>() + ".blocks.json"));
        // the NPC blocks of its data files: built at the first start, from the mod's folder (read only)
        std::vector<qb::ojson> block_docs;
        // the quest and dialogue blocks ("quests", "dialogues"): the quests' file format, key for key as far as the build reads it (a block's rewards are a bundle that
        // no key here reads, and its actions and conditions are the file's), so they are built at the first start from the mod's folder like the NPC blocks. Kiln.Blocks
        // leaves what it read from a data file out of the file it writes for code (Kiln.Quests Native/NativeExport.cs), so nothing is built twice. One document per
        // file, named for messages ("blocks/<file>.json"), in name order.
        std::vector<std::pair<std::string, qb::ojson>> block_quests;
        {
            std::vector<fs::path> paths;
            for (auto it = fs::directory_iterator(base / L"blocks", ec); !ec && it != fs::directory_iterator(); it.increment(ec))
                if (it->path().extension() == L".json") paths.push_back(it->path());
            std::sort(paths.begin(), paths.end());
            std::vector<qb::ojson> bodies;
            for (const fs::path& path : paths) {
                const std::u8string u = path.filename().u8string();
                const std::string label = "blocks/" + std::string(u.begin(), u.end());
                const uintmax_t size = fs::file_size(path, ec);
                qb::ojson d = ec || size > qb::kMaxBlockFile ? qb::ojson(qb::ojson::value_t::discarded) : qb::parse_blocks_json(read_block_file(path));
                ec.clear();
                if (d.is_discarded() || !d.is_object()) {
                    KW("native: %s: %s: not read (not a JSON object, over 16 MiB, or nested deeper than %d): its quests, dialogues and NPCs are not built at start",
                       mf["id"].get_ref<const std::string&>().c_str(), label.c_str(), qb::kMaxBlockDepth);
                    strict(label, "not read (it cannot be read, is not a JSON object, is over 16 MiB, or is nested deeper than " +
                                      std::to_string(qb::kMaxBlockDepth) + ")");
                    continue;
                }
                qb::ojson q = qb::ojson::object();
                for (const char* key : {"quests", "dialogues"})
                    if (d.contains(key) && d[key].is_object() && !d[key].empty()) q[key] = d[key];
                if (!q.empty()) {
                    q["format"] = 1;
                    block_quests.emplace_back(label, std::move(q));
                }
                if (d.contains("npcs")) block_docs.push_back(std::move(d));
                else if (d.contains("bodies")) bodies.push_back(std::move(d));
            }
            // bodies a block names may be in any of the files
            if (!block_docs.empty())
                for (qb::ojson& b : bodies) block_docs.push_back(std::move(b));
            // an NPC block with "was" is that NPC in the build (npcs_from_blocks) and in the engine (QuestCompiler.Npc): a quest or dialogue block names it by its block name
            std::map<std::string, std::string> renamed;
            for (const qb::ojson& d : block_docs)
                if (d.contains("npcs") && d["npcs"].is_object())
                    for (const auto& [name, n] : d["npcs"].items())
                        if (n.is_object() && n.contains("was") && n["was"].is_string() && n["was"].get_ref<const std::string&>() != name) renamed[name] = n["was"].get<std::string>();
            if (!renamed.empty()) {
                // depth is bounded by parse_blocks_json
                const std::function<void(qb::ojson&)> rename = [&](qb::ojson& j) {
                    if (j.is_array())
                        for (qb::ojson& x : j) rename(x);
                    if (!j.is_object()) return;
                    for (auto& [k, v] : j.items()) {
                        if (v.is_string() && (k == "giver" || k == "near" || k == "to" || k == "npc" || k == "bindNpc")) {
                            auto r = renamed.find(v.get<std::string>());
                            if (r != renamed.end()) v = r->second;
                        } else {
                            rename(v);
                        }
                    }
                };
                for (auto& [label, d] : block_quests) rename(d);
            }
        }
        ec.clear();
        const qb::ojson block_npcs = block_docs.empty() ? qb::ojson::object() : npcs_from_blocks(mf["id"].get<std::string>(), block_docs);
        if (!fs::is_directory(qdir, ec) && !fs::exists(code, ec) && !fs::exists(npc_blocks, ec) && block_npcs.empty() && block_quests.empty() &&
            boot_docs.empty())
            return;
        if (reading) *reading = mf["id"].get<std::string>();
        const std::string side = mf.value("side", "both");
        if ((side == "server" && client) || (side == "client" && !client)) return;
        qb::ModInput m;
        m.id = mf["id"].get<std::string>();
        for (const auto& existing : *mods)
            if (existing.id == m.id) return;
        std::vector<fs::path> files, generated;
        for (auto it = fs::directory_iterator(qdir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->path().extension() == L".json") files.push_back(it->path());
        for (auto it = fs::directory_iterator(qdir / L"generated", ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->path().extension() == L".json") generated.push_back(it->path());
        std::sort(files.begin(), files.end());
        std::sort(generated.begin(), generated.end());
        files.insert(files.end(), generated.begin(), generated.end());
        ec.clear();
        const size_t own_files = files.size();
        if (fs::exists(npc_blocks, ec)) files.push_back(npc_blocks);
        if (fs::exists(code, ec)) files.push_back(code);
        const bool has_blocks = !block_npcs.empty() || !block_quests.empty();
        const auto push_blocks = [&]() {
            if (!block_npcs.empty()) {
                m.documents.push_back(qb::ojson{{"format", 1}, {"npcs", block_npcs}});
                m.files.push_back("blocks/*.json");
            }
            for (auto& [label, d] : block_quests) {
                m.documents.push_back(std::move(d));
                m.files.push_back(label);
            }
            for (auto& [label, d] : boot_docs) {
                if (d.is_discarded() || !d.is_object()) {
                    KW("native: %s: %s is not a JSON object: skipped", m.id.c_str(), label.c_str());
                    continue;
                }
                m.documents.push_back(std::move(d));
                m.files.push_back(label);
            }
        };
        const bool has_blocks_or_boot = has_blocks || !boot_docs.empty();
        for (size_t i = 0; i < files.size(); ++i) {
            // the mod's own quests files first (they win), then its NPC, quest and dialogue blocks, then (a boot generation) what its OnBoot
            // staged, then what its code defined at an earlier start
            if (i == own_files && has_blocks_or_boot) push_blocks();
            const fs::path& f = files[i];
            const std::string name = reinterpret_cast<const char*>(f.filename().u8string().c_str());
            std::ifstream probe(f, std::ios::binary);
            const bool readable = static_cast<bool>(probe);
            qb::ojson doc = readable ? qb::ojson::parse(read_file(f), nullptr, false, true) : qb::ojson(qb::ojson::value_t::discarded);
            if (doc.is_discarded() || !doc.is_object()) {
                KW("native: %s/%s %s: skipped", m.id.c_str(), name.c_str(), readable ? "is not valid JSON" : "cannot be read");
                // the mod's own quests files (quests/ and quests/generated/); what Kiln kept from its code is Kiln's data
                if (i < own_files) strict((i < own_files - generated.size() ? "quests/" : "quests/generated/") + name, readable ? "not valid JSON" : "cannot be read");
                continue;
            }
            m.documents.push_back(std::move(doc));
            m.files.push_back(reinterpret_cast<const char*>(f.filename().u8string().c_str()));
        }
        if (files.size() == own_files && has_blocks_or_boot) push_blocks();
        const qb::ojson audio = qb::ojson::parse(read_file(base / L"assets" / L"cooked" / L"audio.json"), nullptr, false, true);
        if (!audio.is_discarded()) m.audio = audio;
        const qb::ojson cook = qb::ojson::parse(read_file(base / L"assets" / L"cook.json"), nullptr, false, true);
        if (!cook.is_discarded() && cook.is_object()) m.guid_namespace = cook.value("guidNamespace", "");
        else if (fs::exists(base / L"assets" / L"cook.json", ec)) strict("assets/cook.json", "not valid JSON (its \"guidNamespace\" names the mod's GUIDs)");
        if (g_fail == Fail::Read && ships(m)) throw std::runtime_error(kFailSwitch + ": read");
        mods->push_back(std::move(m));
    };
    const auto* prof = static_cast<const KilnProfileApi*>(g_api->service(KilnStr{"kiln.profile", 12}, 1));
    bool scan_dropins = true;
    if (prof && prof->header.size >= sizeof(KilnProfileApi)) {
        const uint32_t n = prof->count(KILN_PROFILE_KIND_KILN);
        for (uint32_t i = 0; i < n; ++i) {
            KilnProfileMod m{sizeof m};
            if (prof->get(KILN_PROFILE_KIND_KILN, i, &m) == KILN_OK && m.enabled) one(std::string(m.path.ptr, m.path.len));
        }
        if (prof->mode() == KILN_PROFILE_ACTIVE || prof->mode() == KILN_PROFILE_ONCE)
            scan_dropins = (prof->dropins() & (1u << KILN_PROFILE_KIND_KILN)) != 0;
    }
    if (scan_dropins) {
        const std::string root = path_of(KILN_PATH_MODS);
        std::error_code ec;
        std::vector<fs::path> dirs;
        for (auto it = fs::directory_iterator(widen(root), ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->is_directory(ec) && !fs::exists(it->path() / L".disabled", ec)) dirs.push_back(it->path());
        std::sort(dirs.begin(), dirs.end());
        for (const fs::path& d : dirs) {
            const std::u8string u = d.u8string();
            one(std::string(u.begin(), u.end()));
        }
    }
}

bool put_resource(const char* what, const qb::Guid& g, uint32_t type, const qb::Bytes& data, uint32_t flags) {
    if (data.empty()) return true;
    if (g_fail == Fail::Assets) {
        KW("native: %s (%s, %zu bytes): not given to kiln.assets (%s: assets), as when it refuses it", what, qb::guid_text(g).c_str(),
           data.size(), kFailSwitch.c_str());
        return false;
    }
    KilnAssetKey k{};
    std::memcpy(k.guid, g.b, 16);
    k.type = type;
    const KilnResult r = A->resource_put(g_owner, &k, data.data(), data.size(), flags);
    if (r != KILN_OK) KW("native: %s (%s, %zu bytes): kiln.assets refused it (%d)", what, qb::guid_text(g).c_str(), data.size(), r);
    return r == KILN_OK;
}

// A boot generation's part of kiln.knowledge (chapter 08): the mods' inputs, read in prepare; the build (it needs the game's own
// resources) runs when the resource service is up, from the published plan.
struct BootPlan {
    std::vector<qb::ModInput> mods;
    std::set<std::string> required;  // participants whose boot content is required: a build that leaves theirs out stops the start
    bool built_early = false;  // the resource service was up at prepare (a client that loaded Kiln late): built then
};
bool g_boot = false;  // this process has a boot generation: the build takes the published plan's mods

// A required boot mod whose native quests, knowledge or NPCs the build left out, and why (the first error).
struct LeftOut {
    std::string mod, why;
};

// The start must not go on: the caller stops it once g_mu is released (refuse_start).
struct Stop {
    bool set = false;
    StartStop kind = StartStop::Roster;
    std::string why;
    std::vector<std::string> mods;
};

// The mods that ship native quests, knowledge or NPCs (ships). An empty input never stops anything.
std::vector<std::string> shipping(const std::vector<qb::ModInput>& mods) {
    std::vector<std::string> ids;
    for (const qb::ModInput& m : mods)
        if (ships(m)) ids.push_back(m.id);
    return ids;
}
// The mods whose content the build was to serve: the manifest's entries that came out whole (ok).
std::vector<std::string> built(const qb::ojson& manifest_mods) {
    std::vector<std::string> ids;
    for (const auto& e : manifest_mods)
        if (e.is_object() && e.value("ok", false) && e.contains("id") && e["id"].is_string()) ids.push_back(e["id"].get<std::string>());
    return ids;
}
// The whole build failed and nothing of the mods' native content is served: the start stops when `mods` (those whose
// content is involved) is not empty. A stop set first (the roster's) stays.
void whole_stop(Stop* stop, const std::string& why, const std::vector<std::string>& mods) {
    if (!stop || stop->set || mods.empty()) return;
    *stop = Stop{true, StartStop::Content, why, mods};
}
bool client_side() {
    KilnBuildInfo bi{sizeof bi};
    g_api->build_info(&bi);
    return bi.side == KILN_SIDE_CLIENT;
}
// Without a boot generation, for a stop that comes instead of the build: the mods that ship such content (a read that
// throws keeps the mods read before it and names the one it was reading).
std::vector<std::string> shipping_now() {
    std::vector<qb::ModInput> mods;
    std::string reading;
    try {
        collect_mods(&mods, client_side(), nullptr, &reading);
    } catch (const std::exception& e) {
        KW("native: reading the mods' quest files failed: %s", e.what());
        std::vector<std::string> ids = shipping(mods);
        if (!reading.empty() && std::find(ids.begin(), ids.end(), reading) == ids.end()) ids.push_back(reading);
        return ids;
    }
    return shipping(mods);
}

// `left_out` (a boot generation): the required mods of `required` the build could not build whole. `stop`: set when
// the roster was refused, or when the whole build failed while a mod ships such content (whole_stop): then nothing is
// served and the start must not go on. One mod's own errors set neither.
void build_locked(const std::vector<qb::ModInput>* prepared = nullptr, const std::set<std::string>* required = nullptr,
                  std::vector<LeftOut>* left_out = nullptr, Stop* stop = nullptr) {
    if (g_built) return;
    g_built = true;
    KilnBuildInfo bi{sizeof bi};
    g_api->build_info(&bi);
    const bool client = bi.side == KILN_SIDE_CLIENT;
    std::vector<qb::ModInput> mods;
    if (prepared) {
        mods = *prepared;
    } else if (g_boot) {
        const auto* plan = static_cast<const BootPlan*>(kiln::boot::published(g_api, "knowledge"));
        if (!plan) {
            KW("native: the boot generation is not published: mods' native quests and NPCs are off");
            return;
        }
        mods = plan->mods;
        required = &plan->required;
    } else {
        std::string reading;
        try {
            collect_mods(&mods, client, nullptr, &reading);  // the mods' files: nothing they hold may throw out of the game's resource init
        } catch (const std::exception& e) {
            KW("native: reading the mods' quest files failed: %s", e.what());
            // nothing is built: the whole build failed (the mods read before the throw and the one it was reading)
            std::vector<std::string> who = shipping(mods);
            if (!reading.empty() && std::find(who.begin(), who.end(), reading) == who.end()) who.push_back(reading);
            whole_stop(stop, std::string("reading the mods' quest files failed: ") + e.what(), who);
            return;
        }
    }
    if (mods.empty()) return;
    // every required mod in the build that did not come out whole (its entry's errors, or the whole build's)
    std::string whole;  // why the whole build is not served
    auto required_left_out = [&](const qb::ojson& manifest_mods) {
        if (!required || !left_out) return;
        for (const qb::ModInput& m : mods) {
            if (!required->count(m.id)) continue;
            std::string why = whole;
            for (const auto& e : manifest_mods)
                if (e.value("id", std::string()) == m.id && !e.value("ok", true)) {
                    const auto& errs = e.contains("errors") && e["errors"].is_array() ? e["errors"] : qb::ojson::array();
                    why = errs.empty() ? "its native quests, knowledge or NPCs did not build" : errs[0].get<std::string>();
                    if (errs.size() > 1) why += " (and " + std::to_string(errs.size() - 1) + " more, kiln.log)";
                }
            if (!why.empty()) left_out->push_back({m.id, why});
        }
    };
    qb::GameData game;
    if (g_fail == Fail::Shared)  // the lab switch: the game's knowledge resource cannot be read, so the shared resources do not build
        game.read = [](const qb::Guid& g, uint32_t type, const uint8_t** d, size_t* n) { return type != qb::kTypeKnowledge && res_read(g, type, d, n); };
    else
        game.read = &res_read;
    game.template_by_name = &template_by_name;
    game.roster_max = &roster_max;  // 64, or 128 with the second unlock page (installed only when a roster needs it)
    qb::PackOut p;
    try {
        if (g_fail == Fail::Compile) throw std::runtime_error(kFailSwitch + ": compile");
        p = qb::compile_pack(mods, game);
    } catch (const std::exception& e) {
        KW("native: the build failed: %s", e.what());
        whole = std::string("the build of the mods' native quests failed: ") + e.what();
        required_left_out(qb::ojson::array());
        whole_stop(stop, whole, shipping(mods));
        return;
    }
    for (const std::string& e : p.errors) KW("native: %s", e.c_str());
    if (p.roster_refused && stop) {
        stop->set = true;
        stop->why = p.roster_refusal;
        stop->mods = p.roster_mods;
    }
    const qb::ojson manifest_mods = p.manifest.contains("mods") ? p.manifest["mods"] : qb::ojson::array();
    // compile_pack's fail(): the shared resources did not build, nothing is served (a mod's own errors say ok: false):
    // the start stops, naming the mods it was to serve (a roster that cannot be held stops as the roster, set above)
    if (p.manifest.contains("failed") && p.manifest["failed"].is_string()) {
        whole = "the mods' native quests are not served: " + p.manifest["failed"].get<std::string>();
        whole_stop(stop, whole, built(manifest_mods));
    }
    struct Said {
        decltype(required_left_out)& f;
        const qb::ojson& m;
        ~Said() { f(m); }  // on every way out below, after what is served is known
    } said{required_left_out, manifest_mods};
    for (const std::string& w : p.warnings) KW("native: %s", w.c_str());
    g_manifest = p.manifest.dump();
    g_template_names.clear();
    if (!p.any) return;
    if (!p.roster_boot_ids.empty() || !p.roster_kiln_ids.empty()) roster_built(p.roster_boot_ids, p.roster_kiln_ids);
    qb::Guid gk, gj, gn;
    qb::parse_guid(qb::kGuidKnowledge, &gk);
    qb::parse_guid(qb::kGuidJournal, &gj);
    qb::parse_guid(qb::kGuidNpcs, &gn);
    for (const auto& [name, g, type, data] : {std::tuple{"GameKnowledgeResource", gk, qb::kTypeKnowledge, &p.res_knowledge},
                                              std::tuple{"JournalRegistryResource", gj, qb::kTypeJournal, &p.res_journal},
                                              std::tuple{"NpcCollection", gn, qb::kTypeNpcs, &p.res_npcs}})
        if (!put_resource(name, g, type, *data, KILN_ASSET_PUT_OVERRIDE) && whole.empty())
            whole = std::string("kiln.assets refused the built ") + name;
    for (const qb::TemplateOut& t : p.templates)
        if (!put_resource(t.name.c_str(), t.guid, qb::kTypeTemplate, t.data, t.is_new ? KILN_ASSET_PUT_NEW : KILN_ASSET_PUT_OVERRIDE) && whole.empty())
            whole = "kiln.assets refused the built template " + t.name;
    // a refused part: the shared pack is not served as it was built (every mod's knowledge, journal and NPCs are in it)
    if (!whole.empty()) whole_stop(stop, whole, built(manifest_mods));
    else g_served = built(manifest_mods);
    // names for Knowledge.Resolve: "<mod>:<name>"
    for (const auto& m : p.manifest["mods"]) {
        if (!m.value("ok", false)) continue;
        const std::string mod = m.value("id", "");
        for (const auto& [name, idv] : m["knowledge"].items()) {
            std::string key = mod + ":" + name;
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            g_names[key] = static_cast<uint32_t>(std::stoul(idv.get<std::string>(), nullptr, 16));
        }
    }
    // texts: answered by kiln.content's loca hooks (client only: the server never shows text)
    int texts = 0;
    if (client) {
        using SetTexts = void (*)(uint32_t, const char*, size_t);
        HMODULE h = GetModuleHandleW(L"kiln-content.dll");
        auto set = h ? reinterpret_cast<SetTexts>(GetProcAddress(h, "kc_nc_set_texts")) : nullptr;
        if (set) {
            for (const qb::TextOut& t : p.texts) {
                set(t.tag, t.json.data(), t.json.size());
                ++texts;
            }
        } else {
            KW("native: kiln.content has no kc_nc_set_texts: native quests and dialogue show no texts");
        }
    }
    // items and recipes a mod defines inline (summon tools, vendor stock)
    if (!p.content_items.empty() || !p.content_recipes.empty()) {
        const auto* C = static_cast<const KilnContentApi*>(g_api->service(KilnStr{"kiln.content", 12}, 4));
        if (!C) {
            KW("native: kiln.content v4 is not available: summon items and vendor recipes are not defined");
        } else {
            auto define = [&](uint32_t kind, const std::string& json) {
                char report[512] = {};
                size_t n = 0;
                const KilnResult r = C->define(g_owner, kind, KilnStr{json.data(), json.size()}, report, sizeof report, &n);
                if (r != KILN_OK) KW("native: kiln.content refused a definition: %s", report);
            };
            for (const std::string& j : p.content_items) define(KILN_CONTENT_ITEMS, j);
            for (const std::string& j : p.content_recipes) define(KILN_CONTENT_RECIPES, j);
        }
    }
    KI("native: %zu mod(s): %zu new knowledge object(s), %zu quest(s), %zu NPC(s) in the roster, %zu template(s), %d text(s); "
       "GameKnowledgeResource %zu bytes served by kiln.assets",
       mods.size(), p.knowledge.size(), p.quests.size(), p.npcs.size(), p.templates.size(), texts, p.res_knowledge.size());
}

void build_once() {
    std::vector<LeftOut> left_out;
    Stop stop;
    {
        std::lock_guard lock(g_mu);
        if (!A) A = static_cast<const KilnAssetsApi*>(g_api->service(kstr(KILN_ASSETS_SERVICE), 1));
        if (!A || A->header.size < sizeof(KilnAssetsApi) || !A->resource_data || !A->resource_put) {
            if (!g_built) {
                KW("native: kiln.assets is not available: mods' native quests and NPCs are off");
                const auto* plan = g_boot ? static_cast<const BootPlan*>(kiln::boot::published(g_api, "knowledge")) : nullptr;
                if (plan)
                    for (const qb::ModInput& m : plan->mods)
                        if (plan->required.count(m.id)) left_out.push_back({m.id, "kiln.assets is not available: native quests and NPCs are off"});
                // nothing can be served: the whole build failed
                whole_stop(&stop, "kiln.assets is not available (the mods' native quests and NPCs are served through it)",
                           plan ? shipping(plan->mods) : g_boot ? std::vector<std::string>() : shipping_now());
            }
            g_built = true;
        } else {
            build_locked(nullptr, nullptr, &left_out, &stop);
        }
    }
    if (stop.set) refuse_start(stop.kind, stop.why, stop.mods);  // before any world loads; returns only after a world ran
    // a required boot mod whose native data the build left out: kiln.boot stops the start (the call does not return then)
    for (const LeftOut& l : left_out) kiln::boot::activation_fault(g_api, g_owner, l.mod, "knowledge", "", "kiln.knowledge: " + l.why);
}

char __fastcall hk_resource_init(uintptr_t S, void* cfg) {
    const char ok = g_init_hook.call<char>(S, cfg);
    if (ok) build_once();
    return ok;
}

// The lab switch's word (kiln/control/knowledge.fail-build), logged at every start while the file is there.
Fail read_fail_switch() {
    const std::string dir = path_of(KILN_PATH_CONTROL);
    if (dir.empty()) return Fail::None;
    std::ifstream f(fs::path(widen(dir)) / L"knowledge.fail-build", std::ios::binary);
    if (!f) return Fail::None;
    std::string word;
    f >> word;
    std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const struct {
        const char* word;
        Fail fail;
        const char* does;
    } kWords[] = {
        {"read", Fail::Read, "reading the mods' quest files throws, so the start stops when a mod ships native quests or NPCs"},
        {"compile", Fail::Compile, "the build of the mods' native quests throws, so the start stops when a mod ships them"},
        {"shared", Fail::Shared, "the build cannot read the game's GameKnowledgeResource, so the shared resources do not build "
                                 "and the start stops when a mod ships native quests or NPCs"},
        {"assets", Fail::Assets, "the built pack is not given to kiln.assets, as when it refuses it, so the start stops when a mod "
                                 "ships native quests or NPCs"},
        {"hook", Fail::Hook, "the hook on the game's resource start (assets/ResourceInit) is not installed, so the start stops "
                             "when a mod ships native quests or NPCs"},
        {"late", Fail::Late, "the build serves as usual; 30 steps after the server world's first one the stop is asked for, "
                             "which must not end the process"},
    };
    for (const auto& w : kWords)
        if (word == w.word) {
            KW("native: LAB SWITCH kiln/control/knowledge.fail-build is present (%s): %s. Delete the file to play", w.word, w.does);
            return w.fail;
        }
    KW("native: kiln/control/knowledge.fail-build is present but says \"%s\", not read, compile, shared, assets, hook or late: "
       "IGNORED; delete the file",
       word.c_str());
    return Fail::None;
}

// The lab switch "late": 30 steps after the server world's first one (a world runs), the stop a whole failure asks for.
// refuse_start must log that the start could not be stopped and return: ending the process now could lose play.
void KILN_CALL on_late(const KilnTickInfo*, void*) {
    if (g_late_posts.fetch_add(1) != 30) return;
    std::vector<std::string> who;
    {
        std::lock_guard lock(g_mu);
        who = g_served;
    }
    if (who.empty()) return;  // nothing was served: nothing to stop for
    KW("native: %s: late: the stop of a whole-build failure is asked for after a world ran", kFailSwitch.c_str());
    refuse_start(StartStop::Content, kFailSwitch + ": late: a whole-build failure after a world ran", who);
}

}  // namespace

void native_pack_install() {
    g_fail = read_fail_switch();
    g_no_hook.clear();
    g_served.clear();
    g_late_posts = 0;
    if (g_fail == Fail::Late && g_api->size >= offsetof(KilnApi, subscribe_world_tick) + sizeof(void*)) {
        KilnHandle h = 0;
        g_api->subscribe_world_tick(g_owner, KILN_WORLD_SERVER, KILN_TICK_POST, &on_late, nullptr, &h);
    }
    uintptr_t a = 0;
    if (g_api->symbol(kstr("assets/ResourceInit"), &a) != KILN_OK || !a) {
        KW("native: assets/ResourceInit is not resolved: mods' native quests and NPCs are off");
        g_no_hook = "assets/ResourceInit is not resolved on this game build, so nothing builds them at the game's resource start";
        return;
    }
    if (g_fail == Fail::Hook) {
        KW("native: hook on ResourceInit failed: mods' native quests and NPCs are off (%s: hook)", kFailSwitch.c_str());
        g_no_hook = "the hook on the game's resource start (assets/ResourceInit) was not installed (" + kFailSwitch + ": hook)";
        return;
    }
    // before kiln.assets' link (as the old layered hook ran): its post-init code (the resource service it serves our
    // data through) runs first, then build_once
    if (!g_init_hook.install(g_api, g_owner, a, reinterpret_cast<void*>(&hk_resource_init), "assets/ResourceInit", 0,
                             "kiln.assets")) {
        KW("native: hook on ResourceInit failed: mods' native quests and NPCs are off");
        g_no_hook = std::string("the hook on the game's resource start (assets/ResourceInit) could not be installed (") +
                    (g_init_hook.error() ? g_init_hook.error() : "refused") + ")";
        return;
    }
}

namespace {
void KILN_CALL destroy_boot(const void* p) {
    const auto* plan = static_cast<const BootPlan*>(p);
    if (plan->built_early) {  // what the early build served and defined goes with it
        std::lock_guard lock(g_mu);
        if (A) A->revert_owner(g_owner);
        if (const auto* C = static_cast<const KilnContentApi*>(g_api->service(KilnStr{"kiln.content", 12}, 4))) C->undefine_owner(g_owner);
        g_built = false;
        g_names.clear();
        g_manifest = "{\"mods\":[],\"errors\":[]}";
    }
    delete plan;
}

KilnResult KILN_CALL prepare_boot(void*, const KilnBootPrepareInputV1* in, KilnBootPreparedV1* out, KilnBootIssueV1* issue) {
    const kiln::boot::Input input = kiln::boot::parse(in);
    if (!input.ok) return kiln::boot::refuse(out, issue, "", "", "", "kiln.knowledge: the boot envelope is not JSON");
    KilnBuildInfo bi{sizeof bi};
    g_api->build_info(&bi);
    auto* plan = new BootPlan;
    plan->required = input.required;
    try {
        collect_mods(&plan->mods, bi.side == KILN_SIDE_CLIENT, &input);
    } catch (const kiln::boot::Refusal& r) {
        delete plan;
        return kiln::boot::refuse(out, issue, r.mod, r.kind, r.key, r.what());
    } catch (const std::exception& e) {
        delete plan;
        return kiln::boot::refuse(out, issue, "", "", "", std::string("kiln.knowledge: reading the mods' quest files failed: ") + e.what());
    }
    // A client whose resource service was up before Kiln loaded: the ResourceInit hook will not fire, so the pack is built now.
    std::lock_guard lock(g_mu);
    if (!A) A = static_cast<const KilnAssetsApi*>(g_api->service(kstr(KILN_ASSETS_SERVICE), 1));
    qb::Guid gk;
    qb::parse_guid(qb::kGuidKnowledge, &gk);
    const uint8_t* p = nullptr;
    size_t n = 0;
    const bool up = !g_built && A && A->header.size >= sizeof(KilnAssetsApi) && A->resource_data && res_read(gk, qb::kTypeKnowledge, &p, &n);
    if (!up && !g_built && !g_no_hook.empty()) {  // nothing will build these mods' pack at the game's resource start
        const std::vector<std::string> who = shipping(plan->mods);
        if (!who.empty()) {  // before any world loads; does not return (the lock goes first)
            g_mu.unlock();
            refuse_start(StartStop::Content, g_no_hook, who);
            g_mu.lock();  // only when a world ran already: the mods' native content stays off
        }
    }
    if (up) {
        std::vector<LeftOut> left_out;
        Stop stop;
        build_locked(&plan->mods, &plan->required, &left_out, &stop);
        plan->built_early = true;
        if (stop.set) {  // before any world loads; does not return (the lock goes first)
            g_mu.unlock();
            refuse_start(stop.kind, stop.why, stop.mods);
            g_mu.lock();  // only when a world ran already (not at a boot generation's prepare): the build stays refused
        }
        if (!left_out.empty()) {  // built in prepare: a required mod the build left out is refused here (destroy takes the build back)
            const LeftOut first = left_out.front();
            g_mu.unlock();
            destroy_boot(plan);
            g_mu.lock();
            return kiln::boot::refuse(out, issue, first.mod, "knowledge", "", "kiln.knowledge: " + first.why);
        }
    }
    KI("native: boot generation %llu prepared: %zu mod(s) with quests or NPCs%s", static_cast<unsigned long long>(input.generation),
       plan->mods.size(), plan->built_early ? " (built at once: the resource service was up)" : "");
    return kiln::boot::prepared(out, plan, &destroy_boot);
}
}  // namespace

bool native_pack_boot() {
    g_boot = kiln::boot::collect(g_api, g_owner, "knowledge", &prepare_boot, nullptr);
    if (g_boot) KI("native: boot profile: the mods' quest files are read with the boot generation");
    return g_boot;
}

void native_pack_ready() {
    // A client whose resource service was up before Kiln loaded: the hook will not fire.
    Stop stop;
    {
        std::lock_guard lock(g_mu);
        if (g_built || g_boot) return;  // a boot generation builds in its prepare or at the hook
        if (!A) A = static_cast<const KilnAssetsApi*>(g_api->service(kstr(KILN_ASSETS_SERVICE), 1));
        bool up = false;
        if (A && A->header.size >= sizeof(KilnAssetsApi) && A->resource_data) {
            qb::Guid gk;
            qb::parse_guid(qb::kGuidKnowledge, &gk);
            const uint8_t* p = nullptr;
            size_t n = 0;
            up = res_read(gk, qb::kTypeKnowledge, &p, &n);
            if (up) build_locked(nullptr, nullptr, nullptr, &stop);
        }
        if (!up && !g_no_hook.empty()) {  // nothing will build the mods' pack at the game's resource start: the whole build failed
            g_built = true;
            whole_stop(&stop, g_no_hook, shipping_now());
        }
    }
    if (stop.set) refuse_start(stop.kind, stop.why, stop.mods);  // before any world loads; returns only after a world ran
}

std::string native_pack_manifest() {
    std::lock_guard lock(g_mu);
    return g_manifest;
}

bool native_pack_resolve(const std::string& lower_name, uint32_t* id) {
    std::lock_guard lock(g_mu);
    auto it = g_names.find(lower_name);
    if (it == g_names.end()) return false;
    *id = it->second;
    return true;
}

}  // namespace kiln::knowledge
