// SPDX-License-Identifier: MIT
// Host tests of the base view (src/services/boot_archive_view.h): the snapshot backing answers exactly as the snapshot
// reader it wraps, both backings' generation digests, and the live backing over a fake kiln.assets (bytes once the
// resources are up, the size-then-copy read, refusals, the read set its digest names, release and restart), with reads
// from several threads at once. Portable: test-boot-archive-view.sh builds it with ASan/UBSan and with TSan on the host;
// it builds with clang-cl for Windows too (the snapshot is read and hashed through std::filesystem's wide paths there).
// Prints one line per case and "BOOT ARCHIVE VIEW TESTS <n> PASS <m> FAIL"; exits 1 when anything failed.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "services/boot_archive_view.h"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace kiln;

namespace {
int t_pass = 0, t_fail = 0;
std::string t_case;
#define CHECK(x) check((x), #x, __LINE__)
void check(bool ok, const char* what, int line) {
    if (ok) {
        ++t_pass;
        return;
    }
    ++t_fail;
    std::printf("FAIL %s: %s (line %d)\n", t_case.c_str(), what, line);
}
void pass_line(const char* name, int fails_before) { std::printf("%s %s\n", t_fail == fails_before ? "PASS" : "FAIL", name); }

fs::path t_root;

// ---- a snapshot the test writes itself: invented names and values, nothing of the game's ----
const std::string kSha = [] {
    std::string s;
    for (int i = 0; i < 32; ++i) s += "cd";
    return s;
}();
const std::string kVersion = "4242|^/view/test|2026-01-01T00:00:00Z";
const std::string kGuidItem = "0f8fad5b-d9cb-469f-a165-70867728950e", kGuidRows = "99999999-8888-4777-8666-555555555555";

void write_text(const fs::path& f, const std::string& text) {
    fs::create_directories(f.parent_path());
    std::ofstream(f, std::ios::binary) << text;
}

std::string kfc_bytes(const std::string& version) {
    const uint32_t rel = 0x30, len = static_cast<uint32_t>(version.size());
    std::string b(0x10 + rel, '\0');
    std::memcpy(b.data(), "KFC4", 4);
    std::memcpy(b.data() + 0x10, &rel, 4);
    std::memcpy(b.data() + 0x14, &len, 4);
    return b + version;
}

struct Snap {
    fs::path game, root, dir, kfc;
    std::map<std::string, json> index;
    std::map<std::string, std::string> data;
};

void add(Snap& s, const std::string& kind, const std::string& name, const std::string& value, const std::string& guid,
         const std::string& type, int part) {
    s.index[kind][name] = json::array({s.data[kind].size(), value.size(), guid, type, part});
    s.data[kind] += value;
}

Snap make_snapshot(const std::string& name, const std::string& logs_stack = "50") {
    Snap s;
    s.game = t_root / name;
    std::error_code ec;
    fs::remove_all(s.game, ec);
    s.root = s.game / "kiln" / "data" / "boot-gamedata";
    s.kfc = s.game / "enshrouded.kfc";
    write_text(s.kfc, kfc_bytes(kVersion));
    s.dir = s.root / boot_gd::key_of(kSha, kVersion);
    for (const char* k : boot_gd::kKinds) s.index[k] = json::object(), s.data[k] = "";
    add(s, "items", "Test_Item_Logs", R"({"name":"Test_Item_Logs","stackSize":)" + logs_stack + R"(,"tags":["wood","fuel"]})", kGuidItem,
        "1a2b3c4d", 0);
    add(s, "items", "Test.Item-2", R"(  {"name":"second"})", "0f8fad5b-d9cb-469f-a165-70867728950f", "1a2b3c4d", 3);
    add(s, "templates", "Test_Template_Trader", R"({"template":"trader","health":120.5,"components":[{"$type":"Trader"}]})",
        "11111111-2222-4333-8444-555555555555", "0000beef", 0);
    add(s, "skills", "0x8efcc472", R"({"hash":2398930034,"cost":2})", kGuidRows, "00c0ffee", 0);
    add(s, "materials", "StoneRough", R"({"name":"StoneRough","hardness":3})", kGuidRows, "0000f00d", 1);
    return s;
}

// The files: index.json with the checksums unless sums is false (a snapshot from before them), the resource list
// when list is true.
void flush(const Snap& s, bool sums = true, bool list = true) {
    for (const char* k : boot_gd::kKinds) {
        write_text(s.dir / (std::string(k) + ".index.json"), json{{"format", 1}, {"kind", k}, {"entries", s.index.at(k)}}.dump());
        write_text(s.dir / (std::string(k) + ".data"), s.data.at(k));
    }
    if (list) {
        const std::string scene = R"({"scene":"x","cells":[1,2,3]})";
        write_text(s.dir / "resources.data", scene);
        char h[9];
        std::snprintf(h, sizeof h, "%08x", boot_gd::fnv1a32("keen::SceneResource"));
        write_text(s.dir / "resources.index.json",
                   json{{"format", 1},
                        {"kind", "resources"},
                        {"types", {{"keen::SceneResource", {{"hash", h}, {"entries", json::array({json::array({kGuidRows, 0, "Scene_A", 0, scene.size()})})}}}}}}
                       .dump());
    }
    json idx = {{"format", 1}, {"key", boot_gd::key_of(kSha, kVersion)}, {"side", "server"}};
    if (sums) {
        json c = json::object();
        for (const char* k : boot_gd::kKinds)
            for (const char* ext : {".index.json", ".data"}) c[std::string(k) + ext] = boot_gd::file_sha256(s.dir / (std::string(k) + ext));
        if (list)
            for (const char* n : {"resources.index.json", "resources.data"}) c[n] = boot_gd::file_sha256(s.dir / n);
        idx["checksums"] = c;
    }
    write_text(s.dir / "index.json", idx.dump());
}

struct Answer {
    KilnResult rc;
    std::string json, why;
    size_t required = 0, value = 0;
    bool operator==(const Answer& o) const { return rc == o.rc && json == o.json && why == o.why && required == o.required && value == o.value; }
};
template <class R>
Answer ask(R& r, const std::string& q, size_t capacity, size_t result_max = 4u << 20, uint64_t budget = 16u << 20) {
    Answer a{};
    a.required = 777;
    a.rc = r.read(q, capacity, result_max, budget, &a.json, &a.required, &a.value, &a.why);
    return a;
}
struct ViewAsReader {  // tree() under read()'s name, for ask()
    boot_view::BaseView& v;
    KilnResult read(std::string_view q, size_t c, size_t m, uint64_t b, std::string* j, size_t* r, size_t* n, std::string* w) {
        return v.tree(q, c, m, b, j, r, n, w);
    }
};

// ---- the snapshot backing ----
void snapshot_parity_case() {
    t_case = "snapshot-parity";
    const int f0 = t_fail;
    Snap s = make_snapshot("parity");
    flush(s);
    boot_gd::Reader direct, wrapped;
    std::string why;
    CHECK(direct.open(s.root, kSha, s.kfc, &why) && wrapped.open(s.root, kSha, s.kfc, &why));
    boot_view::SnapshotView view(wrapped);
    ViewAsReader vr{view};
    CHECK(std::string(view.source()) == "snapshot" && view.ready(&why));
    const std::vector<std::string> queries = {
        "item:Test_Item_Logs", "item:Test.Item-2", "item:Missing", "template:Test_Template_Trader", "skill:0x8EFCC472",
        "material:StoneRough", "names:items", "names:templates", "names:templates/Trader", "names:skills",
        "resource:" + kGuidItem + "/1a2b3c4d/0", "resource:" + kGuidRows + "/keen::SceneResource/0",
        "resource:" + kGuidRows + "/keen::OtherResource/0", "names:resources/keen::SceneResource", "names:resources/keen::None",
        "bogus", "names:everything"};
    for (const auto& q : queries)
        for (const size_t cap : {size_t{0}, size_t{8}, size_t{1} << 20}) {
            const Answer a = ask(direct, q, cap), b = ask(vr, q, cap);
            CHECK(a == b);
        }
    // the limits too: a value over result_max, a budget it would overspend
    CHECK(ask(direct, "item:Test_Item_Logs", 1 << 20, 40) == ask(vr, "item:Test_Item_Logs", 1 << 20, 40));
    CHECK(ask(direct, "item:Test_Item_Logs", 1 << 20, 4u << 20, 3) == ask(vr, "item:Test_Item_Logs", 1 << 20, 4u << 20, 3));
    const Answer ok = ask(vr, "item:Test_Item_Logs", 1 << 20);
    CHECK(ok.rc == KILN_OK && ok.json.find("\"stackSize\":50") != std::string::npos && ok.required == 777);
    // bytes: the snapshot holds none
    KilnAssetKey key{};
    std::string out = "keep";
    CHECK(view.bytes(key, 1 << 20, &out, &why) == KILN_ERR_UNAVAILABLE && out == "keep" && why.find("decoded values") != std::string::npos);
    // release: the parsed resource list goes, a later read loads it again (as the reader's own release)
    view.release();
    CHECK(ask(vr, "names:resources/keen::SceneResource", 1 << 20).rc == KILN_OK);
    // a closed reader: not ready, no digest
    boot_gd::Reader none;
    boot_view::SnapshotView closed(none);
    CHECK(!closed.ready(&why) && !why.empty() && closed.digest().empty());
    pass_line("BASE-VIEW-SNAPSHOT answers as the snapshot reader, byte for byte", f0);
}

void snapshot_digest_case() {
    t_case = "snapshot-digest";
    const int f0 = t_fail;
    std::string why;
    auto digest_of = [&](const Snap& s) {
        boot_gd::Reader r;
        if (!r.open(s.root, kSha, s.kfc, &why)) return std::string("not open");
        boot_view::SnapshotView v(r);
        const std::string d = v.digest();
        return d == v.digest() ? d : std::string("unstable");
    };
    Snap a = make_snapshot("digest-a");
    flush(a);
    const std::string da = digest_of(a);
    CHECK(boot_gd::lower_hex(da, 64));
    Snap b = make_snapshot("digest-b");  // the same data in another game folder: the same digest
    flush(b);
    CHECK(digest_of(b) == da);
    Snap c = make_snapshot("digest-c", "75");  // one value differs: another digest
    flush(c);
    const std::string dc = digest_of(c);
    CHECK(boot_gd::lower_hex(dc, 64) && dc != da);
    Snap d = make_snapshot("digest-d");  // without the resource list: two files fewer, another digest
    flush(d, true, false);
    CHECK(boot_gd::lower_hex(digest_of(d), 64) && digest_of(d) != da);
    // written before checksums: the files are hashed as they are, so a value still counts, and the same files give the
    // digest a snapshot with checksums of them gives
    Snap e = make_snapshot("digest-e"), f = make_snapshot("digest-f", "75");
    flush(e, false), flush(f, false);
    const std::string de = digest_of(e), df = digest_of(f);
    CHECK(boot_gd::lower_hex(de, 64) && boot_gd::lower_hex(df, 64) && de != df);
    CHECK(de == da && df == dc);
    // a damaged file: the digest checks the checksums first, finds the damage and the view stops serving
    Snap g = make_snapshot("digest-g");
    flush(g);
    {
        boot_gd::Reader r;
        CHECK(r.open(g.root, kSha, g.kfc, &why));
        boot_view::SnapshotView v(r);
        write_text(g.dir / "items.data", g.data["items"] + " ");
        CHECK(v.digest().empty() && !v.ready(&why));
    }
    // one view, the snapshot opened again after its files changed: the digest of the new opening
    Snap h = make_snapshot("digest-h");
    flush(h, false);
    {
        boot_gd::Reader r;
        CHECK(r.open(h.root, kSha, h.kfc, &why));
        boot_view::SnapshotView v(r);
        const std::string first = v.digest();
        Snap h2 = make_snapshot("digest-h", "99");
        flush(h2, false);
        CHECK(v.digest() == first);  // kept for the opening
        CHECK(r.open(h.root, kSha, h.kfc, &why));
        const std::string second = v.digest();
        CHECK(boot_gd::lower_hex(second, 64) && second != first);
    }
    pass_line("BASE-VIEW-SNAPSHOT-DIGEST names the data: same files same digest, one value or file more another", f0);
}

// ---- a fake kiln.assets: resources by key, when_resources_ready held until the test says the resources are up ----
struct FakeAssets {
    std::mutex m;
    std::map<std::string, std::string> res;  // key bytes -> data
    std::map<std::string, int> grow;         // key -> how many more copies find it larger than its size said
    std::function<void()> on_copy;           // runs at the start of a copy (a read with room), before it answers
    void(KILN_CALL* ready_fn)(void*) = nullptr;
    void* ready_user = nullptr;
    bool up = false;
    KilnResult ready_rc = KILN_OK;
    std::atomic<int> reads{0};
};
FakeAssets* t_fake = nullptr;
std::string key_bytes(const KilnAssetKey& k) { return std::string(reinterpret_cast<const char*>(&k), sizeof k); }

KilnResult KILN_CALL fake_read(const KilnAssetKey* key, void* buffer, size_t capacity, size_t* out_len) {
    FakeAssets& f = *t_fake;
    ++f.reads;
    if (capacity != 0 && f.on_copy) f.on_copy();
    std::lock_guard lk(f.m);
    const auto it = f.res.find(key_bytes(*key));
    if (it == f.res.end()) return KILN_ERR_NOT_FOUND;
    const size_t size = it->second.size();
    if (auto g = f.grow.find(it->first); g != f.grow.end() && g->second > 0 && capacity != 0) {
        --g->second;  // it grew between the size and the copy
        *out_len = size + 8;
        return KILN_ERR_BUFFER_TOO_SMALL;
    }
    *out_len = size;
    if (capacity < size) return KILN_ERR_BUFFER_TOO_SMALL;
    if (size) std::memcpy(buffer, it->second.data(), size);
    return KILN_OK;
}
KilnResult KILN_CALL fake_when_ready(void(KILN_CALL* fn)(void*), void* user) {
    FakeAssets& f = *t_fake;
    if (f.ready_rc != KILN_OK) return f.ready_rc;
    {
        std::lock_guard lk(f.m);
        if (!f.up) {
            f.ready_fn = fn, f.ready_user = user;
            return KILN_OK;
        }
    }
    fn(user);  // already up: at once, on this thread
    return KILN_OK;
}
void resources_up(FakeAssets& f) {
    void(KILN_CALL* fn)(void*) = nullptr;
    void* user = nullptr;
    {
        std::lock_guard lk(f.m);
        f.up = true;
        fn = f.ready_fn, user = f.ready_user;
    }
    if (fn) fn(user);
}

KilnAssetsApi t_table{};
size_t t_table_size = sizeof(KilnAssetsApi);
bool t_running = true;
const KilnAssetsApi* fake_get() {
    if (!t_running) return nullptr;
    t_table = KilnAssetsApi{};
    t_table.header = {static_cast<uint32_t>(t_table_size), KILN_ASSETS_VERSION};
    t_table.resource_read = &fake_read;
    t_table.when_resources_ready = &fake_when_ready;
    return &t_table;
}

KilnAssetKey key_of(uint8_t g, uint32_t type, uint32_t part = 0) {
    KilnAssetKey k{};
    for (int i = 0; i < 16; ++i) k.guid[i] = static_cast<uint8_t>(g + i);
    k.type = type;
    k.part = part;
    return k;
}

void live_case() {
    t_case = "live";
    const int f0 = t_fail;
    FakeAssets f;
    t_fake = &f;
    const KilnAssetKey item = key_of(1, KILN_ASSET_TYPE_ITEM_INFO), other = key_of(2, 0x1234u, 3), empty = key_of(3, 0x1234u),
                       missing = key_of(9, 0x1234u);
    f.res[key_bytes(item)] = std::string("\x01\x00\x02item bytes", 13);
    f.res[key_bytes(other)] = "other";
    f.res[key_bytes(empty)] = "";
    std::string why, out;
    // kiln.assets not running yet: not ready, asked again later
    t_running = false;
    boot_view::LiveView v(&fake_get);
    CHECK(std::string(v.source()) == "live");
    CHECK(!v.ready(&why) && why == "kiln.assets is not running" && v.digest().empty());
    CHECK(v.bytes(item, 1 << 20, &out, &why) == KILN_ERR_UNAVAILABLE && f.reads == 0);
    t_running = true;
    // bound, but before the game's resources are up: unavailable, nothing read
    CHECK(!v.ready(&why) && why.find("not up yet") != std::string::npos && v.digest().empty());
    CHECK(v.bytes(item, 1 << 20, &out, &why) == KILN_ERR_UNAVAILABLE && f.reads == 0 && out.empty());
    resources_up(f);
    CHECK(v.ready(&why));
    const std::string none = v.digest();  // nothing served yet: the digest of an empty read set
    CHECK(boot_gd::lower_hex(none, 64));
    // the bytes the game gets now, embedded zeros included
    CHECK(v.bytes(item, 1 << 20, &out, &why) == KILN_OK && out == f.res[key_bytes(item)]);
    CHECK(v.bytes(empty, 1 << 20, &out, &why) == KILN_OK && out.empty());
    out = "keep";
    CHECK(v.bytes(missing, 1 << 20, &out, &why) == KILN_ERR_NOT_FOUND && out == "keep");
    CHECK(v.bytes(other, 4, &out, &why) == KILN_ERR_UNAVAILABLE && out == "keep" && why.find("larger") != std::string::npos);
    CHECK(v.bytes(other, 5, &out, &why) == KILN_OK && out == "other");
    // grown between the size and the copy: read again; growing on every try: refused, nothing kept
    f.grow[key_bytes(other)] = 2;
    CHECK(v.bytes(other, 1 << 20, &out, &why) == KILN_OK && out == "other");
    f.grow[key_bytes(other)] = boot_view::LiveView::kReadTries;
    out = "keep";
    CHECK(v.bytes(other, 1 << 20, &out, &why) == KILN_ERR_UNAVAILABLE && out == "keep" && why.find("kept changing") != std::string::npos);
    f.grow.clear();
    // trees: none from live bytes
    std::string j;
    size_t req = 5, val = 0;
    CHECK(v.tree("item:Test_Item_Logs", 1 << 20, 4u << 20, 1u << 20, &j, &req, &val, &why) == KILN_ERR_UNAVAILABLE && req == 5 && j.empty());
    // the digest names what was served: the same reads in another order in another view, the same digest
    const std::string served = v.digest();
    CHECK(boot_gd::lower_hex(served, 64) && served != none && served == v.digest());
    {
        boot_view::LiveView w(&fake_get);  // the fake is up: when_resources_ready answers at once
        CHECK(w.bytes(other, 1 << 20, &out, &why) == KILN_OK && w.bytes(empty, 1 << 20, &out, &why) == KILN_OK &&
              w.bytes(item, 1 << 20, &out, &why) == KILN_OK && w.bytes(item, 1 << 20, &out, &why) == KILN_OK);
        CHECK(w.digest() == served);
        // a value served again with other bytes adds to it
        f.res[key_bytes(item)] = "changed";
        CHECK(w.bytes(item, 1 << 20, &out, &why) == KILN_OK && out == "changed" && w.digest() != served);
        // a missing key or a refused read changes nothing
        const std::string before = w.digest();
        CHECK(w.bytes(missing, 1 << 20, &out, &why) == KILN_ERR_NOT_FOUND && w.digest() == before);
    }
    // release: no read afterwards, nothing kept; restart: it serves again from an empty read set
    v.release();
    const int reads = f.reads;
    CHECK(!v.ready(&why) && why.find("released") != std::string::npos && v.digest().empty());
    CHECK(v.bytes(other, 1 << 20, &out, &why) == KILN_ERR_UNAVAILABLE && f.reads == reads);
    v.restart();
    CHECK(v.ready(&why) && v.digest() == none);
    // released while it read (between the copy and its record): refused, *out as it was, nothing kept
    f.on_copy = [&] { v.release(); };
    out = "keep";
    CHECK(v.bytes(other, 1 << 20, &out, &why) == KILN_ERR_UNAVAILABLE && out == "keep" && why.find("released") != std::string::npos);
    f.on_copy = nullptr;
    v.restart();
    CHECK(v.digest() == none);
    // kiln.assets too old (no when_resources_ready in its table), or without resources on this build: never serves
    {
        t_table_size = offsetof(KilnAssetsApi, when_resources_ready);
        boot_view::LiveView old(&fake_get);
        CHECK(!old.ready(&why) && why.find("too old") != std::string::npos);
        t_table_size = sizeof(KilnAssetsApi);
        CHECK(!old.ready(&why) && why.find("too old") != std::string::npos);  // the binding is decided once
        f.ready_rc = KILN_ERR_UNAVAILABLE;
        boot_view::LiveView off(&fake_get);
        CHECK(!off.ready(&why) && why.find("no resources") != std::string::npos && off.bytes(item, 1 << 20, &out, &why) == KILN_ERR_UNAVAILABLE);
        f.ready_rc = KILN_OK;
        boot_view::LiveView null_getter(nullptr);
        CHECK(!null_getter.ready(&why) && why == "kiln.assets is not running");
    }
    t_fake = nullptr;
    pass_line("BASE-VIEW-LIVE serves kiln.assets' bytes once the resources are up; its digest names the reads", f0);
}

std::string empty_live_digest() {
    boot_gd::Sha256 h;
    h.update(boot_view::kDigestHead).update("live\n");
    return boot_gd::hex_of(h.final());
}

// Reads from eight threads while the resources come up, the digest is taken and the view is released: no torn
// answer, nothing kept after the release (the TSan build: no race).
void live_threads_case() {
    t_case = "live-threads";
    const int f0 = t_fail;
    FakeAssets f;
    t_fake = &f;
    std::vector<KilnAssetKey> keys;
    for (uint8_t i = 0; i < 16; ++i) {
        keys.push_back(key_of(static_cast<uint8_t>(16 * i), 0x77u, i));
        f.res[key_bytes(keys.back())] = std::string(64 + i, static_cast<char>('a' + i));
    }
    boot_view::LiveView v(&fake_get);
    std::atomic<bool> go{false};
    std::atomic<int> bad{0};
    std::vector<std::thread> th;
    for (int t = 0; t < 8; ++t)
        th.emplace_back([&, t] {
            while (!go.load()) std::this_thread::yield();
            std::string out, why;
            for (int i = 0; i < 400; ++i) {
                const KilnAssetKey& k = keys[(t + i) % keys.size()];
                const KilnResult rc = v.bytes(k, 1 << 20, &out, &why);
                if (rc == KILN_OK) {
                    if (out != std::string(64 + k.part, static_cast<char>('a' + k.part))) ++bad;
                } else if (rc != KILN_ERR_UNAVAILABLE) {
                    ++bad;
                }
                if (i % 50 == 0) (void)v.digest();
            }
        });
    go = true;
    resources_up(f);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    v.release();
    for (auto& t : th) t.join();
    std::string why;
    CHECK(bad == 0);
    CHECK(!v.ready(&why) && v.digest().empty());
    v.restart();
    CHECK(v.digest() == empty_live_digest());  // the release let go of every read before it, none after it was kept
    t_fake = nullptr;
    pass_line("BASE-VIEW-LIVE-THREADS reads from 8 threads across the resources coming up and a release", f0);
}

void snapshot_threads_case() {
    t_case = "snapshot-threads";
    const int f0 = t_fail;
    Snap s = make_snapshot("threads");
    flush(s);
    boot_gd::Reader r;
    std::string why;
    CHECK(r.open(s.root, kSha, s.kfc, &why));
    boot_view::SnapshotView v(r);
    const std::string want = v.digest();
    std::atomic<int> bad{0};
    std::vector<std::thread> th;
    for (int t = 0; t < 6; ++t)
        th.emplace_back([&, t] {
            ViewAsReader vr{v};
            for (int i = 0; i < 60; ++i) {
                const Answer a = ask(vr, (t + i) % 2 ? "item:Test_Item_Logs" : "names:resources/keen::SceneResource", 1 << 20);
                if (a.rc != KILN_OK) ++bad;
                if (i % 20 == 0 && v.digest() != want) ++bad;
                if (t == 0 && i % 15 == 0) v.release();
            }
        });
    for (auto& t : th) t.join();
    CHECK(bad == 0);
    pass_line("BASE-VIEW-SNAPSHOT-THREADS tree, digest and release from 6 threads", f0);
}
}  // namespace

int main(int argc, char** argv) {
    t_root = fs::path(argc > 1 ? argv[1] : ".") / "boot-archive-view-test";
    std::error_code ec;
    fs::remove_all(t_root, ec);
    fs::create_directories(t_root);
    snapshot_parity_case();
    snapshot_digest_case();
    live_case();
    live_threads_case();
    snapshot_threads_case();
    fs::remove_all(t_root, ec);
    std::printf("BOOT ARCHIVE VIEW TESTS %d PASS %d FAIL\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
