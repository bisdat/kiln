// SPDX-License-Identifier: MIT
// A command line around native/services/quests/questpack.cpp for test_questbuild.py: runs the same builder code as
// kiln-quests.dll, on resource files tools/kfc dumped.
//   questbuild_cli --res DIR --names names.json --out DIR [--mod DIR]...
//   questbuild_cli --hash TEXT   (uuid5 / fnv checks)
//   questbuild_cli --motion DIR  (16f S4: DIR/template.bin, DIR/<source guid>.bin, DIR/bindings.json -> clones + remap)
//   questbuild_cli --motion-closure  (the 21 closure sources as JSON)
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include "questpack.h"
#include "bodypack.h"
#include "motionclose.h"

namespace fs = std::filesystem;
using namespace kiln::qb;

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
static void spit(const fs::path& p, const void* d, size_t n) {
    std::ofstream f(p, std::ios::binary);
    f.write(static_cast<const char*>(d), static_cast<std::streamsize>(n));
}

int main(int argc, char** argv) {
    fs::path res, names, out, body, body_spec;
    std::vector<fs::path> mod_dirs;
    uint32_t roster_max = 0;  // --roster-max 128: as a side where the roster's second page installs (default 64)
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--res") res = argv[++i];
        else if (a == "--names") names = argv[++i];
        else if (a == "--out") out = argv[++i];
        else if (a == "--mod") mod_dirs.push_back(argv[++i]);
        else if (a == "--body") body = argv[++i];
        else if (a == "--body-spec") body_spec = argv[++i];
        else if (a == "--body-layout") {
            ojson types = ojson::array();
            for (const auto& type : body_layout()) {
                ojson fields = ojson::array();
                for (const auto& field : type.fields) fields.push_back({{"name", field.name}, {"type", field.type}, {"offset", field.offset}});
                ojson values = ojson::array();
                for (const auto& value : type.values) values.push_back({{"name", value.name}, {"value", value.value}});
                types.push_back({{"name", type.name}, {"size", type.size}, {"fields", fields},
                    {"inner", type.inner ? ojson(type.inner) : ojson(nullptr)}, {"values", values}});
            }
            std::cout << types.dump() << '\n';
            return 0;
        }
        else if (a == "--motion-closure") {
            ojson rows = ojson::array();
            for (const auto& s : motionclose::fisher_closure()) rows.push_back({{"guid", s.guid}, {"role", s.role}, {"variants", s.variants}});
            std::cout << rows.dump() << '\n';
            return 0;
        }
        else if (a == "--motion") {
            // The motion closure as kiln-quests.dll builds it, on files a test dumped: clone ids derived like the boot build's.
            const fs::path dir = argv[++i];
            const ojson rows = ojson::parse(slurp(dir / "bindings.json"));
            motionclose::Bindings bound;
            std::string why;
            ojson report = {{"ok", false}, {"reason", ""}, {"clones", ojson::object()}, {"replaced", 0}};
            const auto done = [&](bool ok) { report["ok"] = ok; report["reason"] = why; std::cout << report.dump() << '\n'; return ok ? 0 : 1; };
            if (!motionclose::parse_bindings(rows, &bound, &why)) return done(false);
            std::map<std::string, Guid> map;
            for (const auto& s : motionclose::fisher_closure()) {
                const std::string raw = slurp(dir / (std::string(s.guid) + ".bin"));
                const Guid clone = derive_guid("motion-cli", "npcmotion", std::string("tobin:") + s.guid);
                Bytes out;
                if (!motionclose::clone_sequence(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), clone, s, bound[s.role],
                        [](const Guid& old) { return derive_guid("motion-cli", "npcmotion", "tobin:" + guid_text(old)); }, &out, &why))
                    return done(false);
                spit(dir / (std::string(s.guid) + ".clone.bin"), out.data(), out.size());
                map[s.guid] = clone;
                report["clones"][s.guid] = guid_text(clone);
            }
            const std::string raw = slurp(dir / "template.bin");
            Bytes tpl(raw.begin(), raw.end());
            size_t replaced = 0;
            if (!motionclose::remap_template(&tpl, map, &replaced, &why)) return done(false);
            spit(dir / "template.clone.bin", tpl.data(), tpl.size());
            report["replaced"] = replaced;
            return done(true);
        }
        else if (a == "--roster-max") roster_max = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (a == "--parse-blocks") {
            const std::string text((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
            const ojson parsed = parse_blocks_json(text);
            std::printf("%s\n", ojson{{"valid", !parsed.is_discarded()}, {"value", parsed.is_discarded() ? ojson(nullptr) : parsed}}.dump().c_str());
            return 0;
        }
        else if (a == "--hash") {
            Guid ns;
            parse_guid("928632ef-3270-4bc2-8657-0b6bcdeb2976", &ns);
            const Guid g = uuid5(ns, argv[++i]);
            std::printf("%s %08x %08x\n", guid_text(g).c_str(), fnv(g), derive_id("mod", "know", "x"));
            return 0;
        }
    }
    if (!body.empty()) {
        const std::string raw = slurp(body);
        const ojson spec = ojson::parse(slurp(body_spec));
        BodySpec b;
        std::string why;
        Bytes lowered{0x5a};  // a refusal must not replace its caller's output
        const bool ids = parse_guid(spec.value("model", ""), &b.model) && parse_guid(spec.value("graph", ""), &b.graph);
        b.radius = spec["size"]["radius"].get<double>();
        b.height = spec["size"]["height"].get<double>();
        bool ok = ids && lower_body(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), b, &lowered, &why);
        if (ok && spec.contains("edit")) {
            const auto& edit = spec["edit"];
            TemplateEdit e;
            e.new_name = edit.value("name", "");
            e.has_npc_id = edit.contains("npcId"); e.npc_id = edit.value("npcId", 0u);
            e.has_loca = edit.contains("locaTag");
            if (e.has_loca) parse_guid(edit["locaTag"].get<std::string>(), &e.loca_tag);
            e.dialog.any = edit.contains("headline"); e.dialog.headline = edit.value("headline", 0u);
            Bytes final;
            ok = edit_template(lowered.data(), lowered.size(), e, &final, &why);
            if (ok) lowered = std::move(final);
        }
        fs::create_directories(out);
        spit(out / "body.bin", lowered.data(), lowered.size());
        std::cout << ojson{{"ok", ok}, {"reason", why}, {"inputUnchanged", raw == slurp(body)}}.dump() << '\n';
        return ok ? 0 : 1;
    }
    std::map<std::string, std::string> cache;
    std::map<std::string, Guid> tnames;
    if (!names.empty()) {
        const nlohmann::json j = nlohmann::json::parse(slurp(names));
        for (auto it = j.begin(); it != j.end(); ++it) {
            Guid g;
            if (parse_guid(it.value().get<std::string>(), &g)) tnames[it.key()] = g;
        }
    }
    GameData game;
    game.read = [&](const Guid& g, uint32_t type, const uint8_t** p, size_t* n) {
        char t[16];
        std::snprintf(t, sizeof t, "%08x", type);
        const std::string key = guid_text(g) + "." + t + ".bin";
        auto it = cache.find(key);
        if (it == cache.end()) {
            const fs::path f = res / key;
            if (!fs::exists(f)) return false;
            it = cache.emplace(key, slurp(f)).first;
        }
        *p = reinterpret_cast<const uint8_t*>(it->second.data());
        *n = it->second.size();
        return true;
    };
    game.template_by_name = [&](const std::string& s, Guid* o) {
        auto it = tnames.find(s);
        if (it == tnames.end()) return false;
        *o = it->second;
        return true;
    };
    if (roster_max) game.roster_max = [roster_max](uint32_t, std::string*) { return roster_max; };
    std::vector<ModInput> mods;
    for (const fs::path& d : mod_dirs) {
        ModInput m;
        const ojson mf = ojson::parse(slurp(d / "kiln.mod.json"), nullptr, false, true);
        m.id = mf.value("id", "");
        std::vector<fs::path> fl, gen;
        for (const auto& e : fs::directory_iterator(d / "quests"))
            if (e.path().extension() == ".json") fl.push_back(e.path());
        std::sort(fl.begin(), fl.end());
        if (fs::is_directory(d / "quests/generated"))
            for (const auto& e : fs::directory_iterator(d / "quests/generated"))
                if (e.path().extension() == ".json") gen.push_back(e.path());
        std::sort(gen.begin(), gen.end());
        fl.insert(fl.end(), gen.begin(), gen.end());
        for (const fs::path& f : fl) {
            m.documents.push_back(ojson::parse(slurp(f), nullptr, true, true));
            m.files.push_back(f.filename().string());
        }
        if (fs::exists(d / "assets/cooked/audio.json")) m.audio = ojson::parse(slurp(d / "assets/cooked/audio.json"));
        if (fs::exists(d / "assets/cook.json")) {
            const ojson c = ojson::parse(slurp(d / "assets/cook.json"), nullptr, false, true);
            if (c.is_object()) m.guid_namespace = c.value("guidNamespace", "");
        }
        mods.push_back(std::move(m));
    }
    PackOut p = compile_pack(mods, game);
    fs::create_directories(out);
    spit(out / "knowledge.bin", p.res_knowledge.data(), p.res_knowledge.size());
    spit(out / "journal.bin", p.res_journal.data(), p.res_journal.size());
    spit(out / "npcs.bin", p.res_npcs.data(), p.res_npcs.size());
    ojson summary = {{"ok", p.ok}, {"any", p.any}, {"errors", p.errors}, {"warnings", p.warnings}, {"manifest", p.manifest},
                     {"contentItems", p.content_items}, {"contentRecipes", p.content_recipes}, {"rosterMax", p.roster_max},
                     {"rosterRefused", p.roster_refused}, {"rosterRefusal", p.roster_refusal}, {"rosterMods", p.roster_mods}};
    ojson tpl = ojson::array();
    for (const TemplateOut& t : p.templates) {
        spit(out / ("template." + guid_text(t.guid) + ".bin"), t.data.data(), t.data.size());
        tpl.push_back({{"guid", guid_text(t.guid)}, {"name", t.name}, {"new", t.is_new}});
    }
    summary["templates"] = tpl;
    ojson texts = ojson::object();
    for (const TextOut& t : p.texts) {
        char k[16];
        std::snprintf(k, sizeof k, "%u", t.tag);
        texts[k] = t.json;
    }
    summary["texts"] = texts;
    ojson know = ojson::array();
    for (const KnowledgeDef& k : p.knowledge) know.push_back({{"id", k.id}, {"name", k.name}, {"world", k.world}, {"player", k.player}, {"type", k.type}});
    summary["knowledge"] = know;
    const std::string s = summary.dump(1);
    spit(out / "summary.json", s.data(), s.size());
    std::printf("%s\n", p.ok ? "ok" : "errors");
    return p.ok ? 0 : 1;
}
