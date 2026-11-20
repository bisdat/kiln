// SPDX-License-Identifier: MIT
// Linux host tests of kiln-dllmod's Windows-free logic (src/package.h): manifests, settings, ids, load order, SHA-256.
// tests/run.sh builds and runs it (no Wine, no game).
#include "../src/package.h"

#include <cstdio>

using namespace kdllmod;

static int g_failed = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_failed;                                                  \
        }                                                                \
    } while (0)

static bool bad(const char* text, const char* contains) {
    Manifest m;
    std::string error;
    if (parse_manifest(text, &m, &error)) {
        std::printf("  accepted: %s\n", text);
        return false;
    }
    if (error.find(contains) == std::string::npos) {
        std::printf("  reason \"%s\" lacks \"%s\"\n", error.c_str(), contains);
        return false;
    }
    return true;
}

int main() {
    // CarryOn 0.1.50's manifest as it ships (four bool settings)
    {
        Manifest m;
        std::string e;
        CHECK(parse_manifest("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=0.1.50-dev\nabi=1\n"
                             "setting=move_chests|bool|Chests|true\nsetting=move_objects|bool|Decor|true\n"
                             "setting=move_interactive|bool|Usable objects|true\nsetting=inspect_objects|bool|Debug (object IDs)|false\n",
                             &m, &e));
        CHECK(m.is_mod && m.version == "0.1.50-dev" && m.settings.size() == 4);
        // Configure gets every value sorted by key, each line ended by LF
        CHECK(settings_text(defaults(m)) == "inspect_objects=false\nmove_chests=true\nmove_interactive=true\nmove_objects=true\n");
    }
    // Belt Lantern 0.1.29's: no settings, CRLF and a BOM are fine
    {
        Manifest m;
        std::string e;
        CHECK(parse_manifest("\xEF\xBB\xBF" "ENSHROUDED_PACKAGE_V1\r\nkind=mod\r\nversion=0.1.29-dev\r\nabi=1\r\n", &m, &e));
        CHECK(m.is_mod && m.settings.empty() && settings_text(defaults(m)).empty());
    }
    // choice, percent and translated labels
    {
        Manifest m;
        std::string e;
        CHECK(parse_manifest("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1.0.0\nabi=1\n"
                             "setting=messages|bool|Messages|true\nsetting=mode|choice|Mode|Normal|Calm|Normal|Fast\n"
                             "setting=intensity|percent|Intensity|100|0|200|25\n"
                             "text=en|setting.mode|Mode\ntext=fr|choice.mode.2|Rapide\ntext=pt-br|setting.intensity|Intensidade\n",
                             &m, &e));
        CHECK(m.settings.size() == 3 && m.settings[1].choices.size() == 3 && m.settings[2].step == 25);
        CHECK(m.texts["fr"]["choice.mode.2"] == "Rapide");
        CHECK(valid_value(m.settings[2], "125") && !valid_value(m.settings[2], "110") && !valid_value(m.settings[2], "225"));
        CHECK(valid_value(m.settings[1], "Fast") && !valid_value(m.settings[1], "fast"));
        CHECK(!valid_value(m.settings[0], "1"));
        Values v;
        CHECK(parse_settings("ENSHROUDED_SETTINGS_V1\r\nmode=Fast\r\n\r\nintensity=0\r\n", m, &v, &e));
        CHECK(settings_text(v) == "intensity=0\nmessages=true\nmode=Fast\n");
        CHECK(!parse_settings("ENSHROUDED_SETTINGS_V1\nmode=Fast\nmode=Calm\n", m, &v, &e) && e.find("twice") != std::string::npos);
        CHECK(!parse_settings("ENSHROUDED_SETTINGS_V1\nspeed=1\n", m, &v, &e) && e.find("speed") != std::string::npos);
        CHECK(!parse_settings("ENSHROUDED_SETTINGS_V1\nintensity=10\n", m, &v, &e));
        CHECK(!parse_settings("mode=Fast\n", m, &v, &e));
    }
    // a library with a requirement
    {
        Manifest m;
        std::string e;
        CHECK(parse_manifest("ENSHROUDED_PACKAGE_V1\nkind=library\nversion=2.0\nabi=1\nrequire=Shared/Base.DLL@1.0\n", &m, &e));
        CHECK(!m.is_mod && m.requires_.size() == 1 && m.requires_[0].first == "shared/base.dll" && m.requires_[0].second == "1.0");
    }
    CHECK(bad("kind=mod\nversion=1\nabi=1\n", "first line"));
    CHECK(bad("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\n", "all needed"));
    CHECK(bad("ENSHROUDED_PACKAGE_V1\nkind=mod\nkind=mod\nversion=1\nabi=1\n", "kind"));
    CHECK(bad("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=2\n", "abi"));
    CHECK(bad("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1 0\nabi=1\n", "version"));
    CHECK(bad("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\nauthor=me\n", "unknown key"));
    CHECK(bad("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\njunk\n", "without"));
    CHECK(bad("ENSHROUDED_PACKAGE_V1\nkind=library\nversion=1\nabi=1\nsetting=a|bool|A|true\n", "library"));
    CHECK(bad("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\nsetting=a|choice|A|x|y|z\n", "default"));
    CHECK(bad("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\nsetting=a|choice|A|y|y|y\n", "repeat"));
    CHECK(bad("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\nsetting=a|percent|A|30|0|100|25\n", "percent"));
    CHECK(bad("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\nsetting=a|percent|A|0|0|10000|1\n", "percent"));
    CHECK(bad("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\nsetting=A|bool|A|true\n", "key|type"));
    CHECK(bad("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\nsetting=a|bool|A|true\nsetting=a|bool|B|true\n", "twice"));
    CHECK(bad("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\nsetting=a|bool|A|yes\n", "unknown type"));
    CHECK(bad("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\ntext=en|setting.b|B\n", "names no setting"));
    CHECK(bad("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\nrequire=../x.dll@1\n", "require"));
    CHECK(bad("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\nrequire=x.dll@1\nrequire=X.dll@2\n", "twice"));
    CHECK(bad("ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\nsetting=a|bool|\xC3|true\n", "UTF-8"));
    {
        std::string big = "ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\n";
        for (int i = 0; i < 17; ++i) big += "setting=s" + std::to_string(i) + "|bool|S|true\n";
        CHECK(bad(big.c_str(), "16 settings"));
        std::string huge = "ENSHROUDED_PACKAGE_V1\n" + std::string(16 * 1024, '\n');
        CHECK(bad(huge.c_str(), "16 KiB"));
    }

    // package ids: the DLL's path below the root, lower case with '/'
    CHECK(package_id("CarryOn\\CarryOn.dll") == "carryon/carryon.dll");
    CHECK(package_id("lantern/lantern.dll") == "lantern/lantern.dll");
    CHECK(package_id("../x.dll").empty() && package_id("/abs/x.dll").empty() && package_id("a/x.txt").empty());
    CHECK(package_id("a./x.dll").empty() && package_id("a/b:c.dll").empty() && package_id("a//x.dll").empty());

    // world ids
    {
        const uint8_t g[16] = {0xde, 0xad, 0xbe, 0xef, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xa, 0xb};
        CHECK(world_id_hex(g) == "deadbeef000102030405060708090a0b");
        CHECK(valid_world_id(world_id_hex(g)));
        const uint8_t z[16] = {};
        CHECK(world_id_hex(z).empty() && !valid_world_id(std::string(32, '0')) && !valid_world_id("DEADBEEF000102030405060708090A0B"));
    }

    // load order
    {
        std::map<std::string, std::string> files = {
            {"a/a.dll", "ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\nrequire=lib/l.dll@2\nrequire=b/b.dll@1\n"},
            {"b/b.dll", "ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\nrequire=lib/l.dll@2\n"},
            {"lib/l.dll", "ENSHROUDED_PACKAGE_V1\nkind=library\nversion=2\nabi=1\n"},
            {"c/c.dll", "ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\nrequire=lib/l.dll@3\n"},
            {"d/d.dll", "ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1\nabi=1\nrequire=e/e.dll@1\n"},
            {"e/e.dll", "ENSHROUDED_PACKAGE_V1\nkind=library\nversion=1\nabi=1\nrequire=d/d.dll@1\n"},
        };
        auto lookup = [&](const std::string& id, PlanItem* item) {
            item->id = id;
            auto it = files.find(id);
            if (it == files.end()) {
                item->error = "the package is missing";
                return true;
            }
            parse_manifest(it->second, &item->manifest, &item->error);
            return true;
        };
        std::vector<PlanItem> plan;
        std::string e;
        CHECK(make_plan({"a/a.dll", "b/b.dll"}, lookup, &plan, &e));
        CHECK(plan.size() == 3 && plan[0].id == "lib/l.dll" && plan[1].id == "b/b.dll" && plan[2].id == "a/a.dll");
        CHECK(!make_plan({"a/a.dll"}, lookup, &plan, &e) && e.find("switch it on") != std::string::npos);
        CHECK(!make_plan({"c/c.dll"}, lookup, &plan, &e) && e.find("version 3") != std::string::npos);
        CHECK(!make_plan({"d/d.dll"}, lookup, &plan, &e) && e.find("cycle") != std::string::npos);
        CHECK(!make_plan({"lib/l.dll"}, lookup, &plan, &e) && e.find("library") != std::string::npos);
        CHECK(!make_plan({"x/x.dll"}, lookup, &plan, &e) && e.find("missing") != std::string::npos);
    }

    // SHA-256 test vectors (FIPS 180-2)
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // the one game build
    CHECK(binding_for("5D9EF2F140B8ED49E30DBEE655841B89D08BAEC20F799A1817E171A18A850C77") == &kClient26430625);
    CHECK(binding_for("0000000000000000000000000000000000000000000000000000000000c0ffee") == nullptr);

    std::printf("%s (%d failed)\n", g_failed ? "FAILED" : "logic_test: all passed", g_failed);
    return g_failed ? 1 : 0;
}
