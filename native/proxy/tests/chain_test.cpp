// SPDX-License-Identifier: MIT
// Exercise the production chain loader, including its on-disk export checks, without a game.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "../dinput8.cpp"

static unsigned checks = 0;
#define CHECK(test) do { ++checks; if (!(test)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); return 1; } } while (0)

int main(int argc, char** argv) {
    if (argc != 4) return 2; // scratch folder, addon DLL, ordinary proxy DLL
    namespace fs = std::filesystem;
    const auto root = fs::absolute(argv[1]);
    fs::create_directories(root);
    g_capacity = 32;
    g_chain = static_cast<KilnChainEntry*>(halloc(g_capacity * sizeof(KilnChainEntry)));
    g_canon = static_cast<const wchar_t**>(halloc(g_capacity * sizeof(wchar_t*)));
    g_game_canon = canonical(root.c_str());
    Late late{};
    g_late = &late;
    g_late_count = 1;
    // Identity is by exports, not file name or chain slot. Cover every chain source and the second install slot.
    for (uint32_t source : {KILN_CHAIN_SOURCE_CHAIN, KILN_CHAIN_SOURCE_PROFILE, KILN_CHAIN_SOURCE_EMF}) {
        const auto name = L"renamed-addon-" + std::to_wstring(source) + L"-2.dll";
        const auto file = root / name;
        fs::copy_file(fs::absolute(argv[2]), file, fs::copy_options::overwrite_existing);
        late = {nullptr, file.c_str(), canonical(file.c_str()), false};
        const uint32_t at = g_count;
        chain_one(source, root.c_str(), name.c_str(), kAccept, false);
        CHECK(g_chain[at].status == KILN_CHAIN_LOADED);
        CHECK(g_chain[at].module != nullptr && late.used);
        chain_one(source, root.c_str(), name.c_str(), kAccept, false);
        CHECK(g_chain[at + 1].status == KILN_CHAIN_DUPLICATE);
        CHECK(g_chain[at + 1].module == g_chain[at].module);
    }
    // A DirectInput proxy without shroudlink_shutdown must still obey [late].
    const auto plain = root / L"plain.dll";
    fs::copy_file(fs::absolute(argv[3]), plain, fs::copy_options::overwrite_existing);
    late = {nullptr, plain.c_str(), canonical(plain.c_str()), false};
    const uint32_t at = g_count;
    chain_one(KILN_CHAIN_SOURCE_CHAIN, root.c_str(), L"plain.dll", kAccept, false);
    CHECK(g_chain[at].status == KILN_CHAIN_LATE && !g_chain[at].module);
    CHECK(late.used);
    // Do not override play-once no-mods or a corrupt profile's skip decision.
    chain_one(KILN_CHAIN_SOURCE_PROFILE, root.c_str(), L"plain.dll", kAccept, true);
    CHECK(g_chain[g_count - 1].status == KILN_CHAIN_SKIPPED);
    // Issue 23: a chain.ini put in kiln\chain\ is not read. proxy.log says where it belongs, and only while
    // kiln\chain.ini does not exist.
    {
        const auto game = root / L"misplaced";
        fs::create_directories(game / L"kiln" / L"chain");
        std::ofstream(game / L"kiln" / L"chain" / L"chain.ini") << "[chain]\nloader.dll\n";
        lstrcpynW(g_game, game.c_str(), kPath);
        lstrcpynW(g_kiln, (game / L"kiln").c_str(), kPath);
        auto start = [&] {
            open_log();
            g_count = 0;  // load_chain sizes g_chain and g_canon for one start: earlier entries must not be counted in
            load_chain(false, false);
            CloseHandle(g_log);
            g_log = INVALID_HANDLE_VALUE;
            std::ifstream in(game / L"kiln" / L"logs" / L"proxy.log", std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        };
        const std::string hint = "\\kiln\\chain\\chain.ini is not read: the chain file belongs in ";
        std::string log = start();
        std::printf("misplaced chain.ini: %s", log.c_str());
        CHECK(log.find(hint) != std::string::npos);
        CHECK(log.find("\\kiln\\chain.ini (one folder up") != std::string::npos);
        std::ofstream(game / L"kiln" / L"chain.ini") << "[chain]\n";
        log = start();
        CHECK(log.find(hint) == std::string::npos);
    }
    // EMF's shared dinput8-chain.ini spec: a quoted name and a comment after a name count there; Kiln's own files
    // keep kini.py's rules.
    {
        const auto game = root / L"shared-spec";
        fs::create_directories(game / L"kiln");
        const auto file = game / L"dinput8-chain.ini";
        const char* text =
            "[Chain]\r\n\"quoted.dll\"\r\nnamed.dll ; a comment\r\ntabbed.dll\t# a comment\r\n"
            "key = \"keyed.dll\" ; a comment\r\nplain.dll\r\n\"mods\\with space\\x.dll\"\r\n; off.dll\r\nodd #1.dll\r\n";
        auto read = [&](bool shared, std::vector<std::wstring>* out) {
            std::ofstream(file, std::ios::binary) << text;
            List list{};
            Section sections[] = {{L"chain", &list}};
            read_dll_list(file.c_str(), &list, sections, 1, shared);
            out->clear();
            for (uint32_t i = 0; i < list.count; ++i) out->emplace_back(list.items[i]);
        };
        std::vector<std::wstring> got;
        read(true, &got);
        const std::vector<std::wstring> shared = {L"quoted.dll", L"named.dll", L"tabbed.dll", L"keyed.dll", L"plain.dll",
                                                  L"mods\\with space\\x.dll"};
        CHECK(got == shared);
        read(false, &got);
        CHECK((got == std::vector<std::wstring>{L"plain.dll", L"odd #1.dll"}));
        // The proxy's own read of the game's dinput8-chain.ini takes them: proxy.log names each (missing here).
        lstrcpynW(g_game, game.c_str(), kPath);
        lstrcpynW(g_kiln, (game / L"kiln").c_str(), kPath);
        std::ofstream(file, std::ios::binary) << "[Chain]\r\n\"ghost-a.dll\"\r\nghost-b.dll ; not here\r\n";
        open_log();
            g_count = 0;  // load_chain sizes g_chain and g_canon for one start: earlier entries must not be counted in
            load_chain(false, false);
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
        std::ifstream in(game / L"kiln" / L"logs" / L"proxy.log", std::ios::binary);
        const std::string log((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::printf("shared spec: %s", log.c_str());
        CHECK(log.find("ghost-a.dll") != std::string::npos);
        CHECK(log.find("ghost-b.dll") != std::string::npos);
        CHECK(log.find("not here") == std::string::npos);
    }
    // Invalid shared values are visible, without becoming chain entries or changing Kiln's own INI rules.
    {
        const auto game = root / L"shared-invalid";
        fs::create_directories(game / L"kiln");
        fs::copy_file(fs::absolute(argv[3]), game / L"valid.dll", fs::copy_options::overwrite_existing);
        lstrcpynW(g_game, game.c_str(), kPath);
        lstrcpynW(g_kiln, (game / L"kiln").c_str(), kPath);
        auto start = [&] {
            open_log();
            g_count = 0;  // load_chain sizes g_chain and g_canon for one start: earlier entries must not be counted in
            load_chain(false, false);
            CloseHandle(g_log);
            g_log = INVALID_HANDLE_VALUE;
            std::ifstream in(game / L"kiln" / L"logs" / L"proxy.log", std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        };
        auto count = [](const std::string& log, const std::string& needle) {
            size_t n = 0, pos = 0;
            while ((pos = log.find(needle, pos)) != std::string::npos) ++n, pos += needle.size();
            return n;
        };
        const std::string reason = "skipped: does not name a .dll";
        std::ofstream(game / L"dinput8-chain.ini") <<
            "valid.dll\nmymod\nplugin.asi\n; ignored-comment\n\n[Chain]\n";
        std::string log = start();
        CHECK(g_count == 1 && g_chain[0].status == KILN_CHAIN_LOADED && g_chain[0].module);
        CHECK(std::string(g_chain[0].path.ptr, g_chain[0].path.len).find("valid.dll") != std::string::npos);
        CHECK(count(log, "dinput8-chain.ini: mymod " + reason) == 1);
        CHECK(count(log, "dinput8-chain.ini: plugin.asi " + reason) == 1);
        CHECK(count(log, reason) == 2);
        CHECK(log.find("ignored-comment") == std::string::npos);
        CHECK(log.find("[Chain]") == std::string::npos);
        fs::remove(game / L"dinput8-chain.ini");
        std::ofstream(game / L"kiln" / L"chain.ini") << "mymod\nplugin.asi\n";
        log = start();
        CHECK(count(log, reason) == 0);
        // Preserve the value's spelling, but apply the shared quote/comment rules to its suffix check.
        // A "key = value" line whose value is no .dll is another tool's setting, and another tool's section is its own:
        // both stay silent (the shared spec: "other loaders ignore them").
        std::ofstream(game / L"dinput8-chain.ini") <<
            "\"quoted.asi\" ; explanation\nvalid.dll ; accepted\nsetting = 5\nkey = \"other.asi\"\n[Other]\nother-bad\n";
        log = start();
        CHECK(count(log, "dinput8-chain.ini: \"quoted.asi\" ; explanation " + reason) == 1);
        CHECK(count(log, reason) == 1);
        CHECK(log.find("other-bad") == std::string::npos);
        CHECK(log.find("setting") == std::string::npos);
        CHECK(log.find("other.asi") == std::string::npos);
        CHECK(g_count == 1 && g_chain[0].status == KILN_CHAIN_LOADED);
        // One list-wide budget across sections, then one exact summary; a huge value stays bounded.
        {
            std::ofstream out(game / L"dinput8-chain.ini");
            out << std::string(8000, 'x') << "TAIL\n";
            for (uint32_t i = 1; i < kMaxPerList + 3; ++i)
                out << (i == 128 ? "[EMF]\n" : "") << "bad-" << i << '\n';
        }
        log = start();
        CHECK(count(log, reason) == kMaxPerList);
        CHECK(count(log, "and 3 more") == 1);
        CHECK(log.find("bad-255") != std::string::npos);
        CHECK(log.find("bad-256") == std::string::npos);
        CHECK(log.find("TAIL") == std::string::npos);
        CHECK(count(log, std::string(256, 'x')) == 1);
        CHECK(log.find(std::string(257, 'x')) == std::string::npos);
    }
    // A DLL whose base name Wine also ships as a builtin (Ultimate ASI Loader installed as xinput9_1_0.dll): Wine can map
    // its builtin in place of the file, and the module still reports the asked path. Whether that happened is decided
    // here by the helper's own export (Wine's xinput9_1_0 has none), not by anything the proxy looks at: proxy.log must
    // then carry the warning, and otherwise the plain "loaded at" line. Wine 11.0 (GE-Proton 11) takes its builtin by
    // default; 11.20 loads a native DLL without version information as asked. test-chain.sh also runs with
    // xinput9_1_0=b (always the builtin: KILN_TEST_EXPECT_SWAP=1) and =n,b (never: =0; the override the warning gives).
    // The control's name is no Wine builtin: it always loads as asked. xinput9_1_0 is passive: no window, no device.
    {
        const auto game = root / L"module-swap";
        fs::create_directories(game / L"kiln");
        for (const wchar_t* name : {L"xinput9_1_0.dll", L"kiln-chain-swap-control.dll"})
            fs::copy_file(fs::absolute(argv[3]), game / name, fs::copy_options::overwrite_existing);
        lstrcpynW(g_game, game.c_str(), kPath);
        lstrcpynW(g_kiln, (game / L"kiln").c_str(), kPath);
        auto u8 = [](const wchar_t* text) {
            const auto v = utf8(text);
            return std::string(v.ptr, v.len);
        };
        auto count = [](const std::string& log, const std::string& needle) {
            size_t n = 0, pos = 0;
            while ((pos = log.find(needle, pos)) != std::string::npos) ++n, pos += needle.size();
            return n;
        };
        const char* expect = std::getenv("KILN_TEST_EXPECT_SWAP");
        for (uint32_t source : {KILN_CHAIN_SOURCE_CHAIN, KILN_CHAIN_SOURCE_PROFILE, KILN_CHAIN_SOURCE_EMF}) {
            fs::remove(game / L"kiln" / L"chain.ini");
            fs::remove(game / L"dinput8-chain.ini");
            const bool emf = source == KILN_CHAIN_SOURCE_EMF;
            const auto ini = emf ? game / L"dinput8-chain.ini" : game / L"kiln" / L"chain.ini";
            std::ofstream(ini) << (emf ? "[Chain]\n" : source == KILN_CHAIN_SOURCE_CHAIN ? "[chain]\n" : "[profile]\n")
                               << (emf ? "" : "..\\") << "xinput9_1_0.dll\n"
                               << (emf ? "" : "..\\") << "kiln-chain-swap-control.dll\n";
            open_log();
            g_count = 0;
            load_chain(false, false);
            CloseHandle(g_log);
            g_log = INVALID_HANDLE_VALUE;
            std::ifstream in(game / L"kiln" / L"logs" / L"proxy.log", std::ios::binary);
            const std::string log(std::istreambuf_iterator<char>(in), {});
            CHECK(g_count == 2);
            for (uint32_t j = 0; j < 2; ++j) {
                CHECK(g_chain[j].status == KILN_CHAIN_LOADED && g_chain[j].module);
                CHECK(g_chain[j].source == source);
                const auto h = static_cast<HMODULE>(g_chain[j].module);
                const bool swapped = GetProcAddress(h, "kiln_proxy_test_helper") == nullptr;
                const auto asked = game / (j == 0 ? L"xinput9_1_0.dll" : L"kiln-chain-swap-control.dll");
                wchar_t asked_canon[kPath], reported[kPath], reported_canon[kPath];
                CHECK(canonical(asked.c_str(), asked_canon));
                const DWORD n = GetModuleFileNameW(h, reported, kPath);
                CHECK(n > 0 && n < kPath);
                const bool same_path = canonical(reported, reported_canon) && wieq(reported_canon, asked_canon);
                const std::string prefix = "chain: " + u8(source_name(source)) + ": ";
                const std::string requested = prefix + "requested " + u8(asked_canon) +
                                              " but another file was loaded in its place";
                wchar_t plain_line[1024];
                wsprintfW(plain_line, L"chain: %s: %s loaded at %p", source_name(source), asked_canon, h);
                const std::string plain = u8(plain_line) + "\r\n";
                if (swapped) {
                    // Wine reports the asked path for its builtin: the warning then names it "Wine's own xinput9_1_0"
                    const std::string warning = requested +
                        (same_path ? std::string(": Wine's own xinput9_1_0") : ": " + u8(reported)) +
                        "; on Wine or Proton a DLL override is needed: WINEDLLOVERRIDES=\"xinput9_1_0=n,b\"\r\n";
                    CHECK(count(log, warning) == 1);
                    CHECK(log.find(plain) == std::string::npos);
                    if (source == KILN_CHAIN_SOURCE_CHAIN) std::printf("proxy.log: %s", warning.c_str());
                } else {
                    CHECK(count(log, plain) == 1);
                    CHECK(log.find(requested) == std::string::npos);
                }
                std::printf("module swap (%s, %s): %s; module reports %s\n", u8(source_name(source)).c_str(),
                            j == 0 ? "xinput9_1_0" : "control",
                            swapped ? "Wine's builtin in its place (no helper export): warning" : "the file asked for: loaded line",
                            same_path ? "the asked path" : u8(reported).c_str());
                if (j == 1) CHECK(!swapped);
                if (j == 0 && expect) CHECK(swapped == (expect[0] == '1'));
            }
        }
    }
    // The comparison itself, under any Wine and on Windows: a module handed a file that is not its own. The control's
    // module against copies of its file with one kept header field changed (each one counts, ImageBase does not), its
    // own file, a missing and a cut-short file (no proof, no warning); a Wine builtin module (kernel32 under Wine)
    // against a file of its own header page without the marker (only the second sign can tell).
    {
        namespace im = kiln::image_match;
        auto u8 = [](const wchar_t* text) {
            const auto v = utf8(text);
            return std::string(v.ptr, v.len);
        };
        const auto dir = root / L"seam";
        fs::create_directories(dir / L"kiln");
        const auto control = dir / L"seam-control.dll";
        fs::copy_file(fs::absolute(argv[3]), control, fs::copy_options::overwrite_existing);
        const HMODULE h = LoadLibraryW(control.c_str());
        CHECK(h && GetProcAddress(h, "kiln_proxy_test_helper"));
        std::ifstream in(control, std::ios::binary);
        const std::string bytes(std::istreambuf_iterator<char>(in), {});
        CHECK(bytes.size() > 0x400);
        uint32_t nt = 0;
        memcpy(&nt, bytes.data() + 0x3c, 4);
        CHECK(nt + 0x100 < bytes.size());
        auto variant = [&](const wchar_t* name, size_t offset, size_t width) {  // offset from the PE signature
            std::string b = bytes;
            for (size_t i = 0; i < width; ++i) b[nt + offset + i] = static_cast<char>(b[nt + offset + i] ^ 0x5a);
            std::ofstream(dir / name, std::ios::binary) << b;
            return dir / name;
        };
        im::Id loaded;
        CHECK(swap_signs(control.c_str(), h, &loaded) == 0 && loaded.ok && !loaded.wine_builtin);
        const struct {
            const wchar_t* name;
            size_t offset, width;
            unsigned want;
        } cases[] = {
            {L"time.dll", 8, 4, im::kFieldsDiffer},           // FileHeader.TimeDateStamp
            {L"sections.dll", 6, 2, im::kFieldsDiffer},       // FileHeader.NumberOfSections
            {L"entry.dll", 24 + 16, 4, im::kFieldsDiffer},    // OptionalHeader.AddressOfEntryPoint
            {L"size.dll", 24 + 56, 4, im::kFieldsDiffer},     // OptionalHeader.SizeOfImage
            {L"checksum.dll", 24 + 64, 4, im::kFieldsDiffer}, // OptionalHeader.CheckSum
            {L"imagebase.dll", 24 + 24, 8, 0},                // OptionalHeader.ImageBase: relocation rewrites it
        };
        for (const auto& c : cases) {
            const auto p = variant(c.name, c.offset, c.width);
            const unsigned got = swap_signs(p.c_str(), h, &loaded);
            if (got != c.want) std::fprintf(stderr, "seam %ls: signs %u, want %u\n", c.name, got, c.want);
            CHECK(got == c.want);
        }
        CHECK(swap_signs((dir / L"missing.dll").c_str(), h, &loaded) == 0);
        std::ofstream(dir / L"short.dll", std::ios::binary) << bytes.substr(0, 0x100);
        CHECK(swap_signs((dir / L"short.dll").c_str(), h, &loaded) == 0);
        // Through the logger: a proven mismatch writes the warning naming the file the module reports; none, nothing.
        lstrcpynW(g_kiln, (dir / L"kiln").c_str(), kPath);
        open_log();
        const auto time = dir / L"time.dll";
        wchar_t time_canon[kPath], reported[kPath];
        CHECK(canonical(time.c_str(), time_canon));
        CHECK(GetModuleFileNameW(h, reported, kPath) > 0);
        CHECK(log_chain_swap(KILN_CHAIN_SOURCE_PROFILE, time_canon, time_canon, h));
        CHECK(!log_chain_swap(KILN_CHAIN_SOURCE_PROFILE, control.c_str(), nullptr, h));
        CHECK(!log_chain_swap(KILN_CHAIN_SOURCE_PROFILE, (dir / L"missing.dll").c_str(), nullptr, h));
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
        {
            std::ifstream logf(dir / L"kiln" / L"logs" / L"proxy.log", std::ios::binary);
            const std::string log(std::istreambuf_iterator<char>(logf), {});
            const std::string head = log.substr(0, log.find("\r\n") + 2);  // the "Kiln proxy" line of open_log
            CHECK(log.substr(head.size()) == "chain: chain.ini [profile]: requested " + u8(time_canon) +
                                                 " but another file was loaded in its place: " + u8(reported) +
                                                 "; on Wine or Proton a DLL override is needed: "
                                                 "WINEDLLOVERRIDES=\"time=n,b\"\r\n");
        }
        // The second sign alone: a builtin's header page as the file, without its marker (the fields all match).
        const HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        CHECK(im::module_headers(k32).ok);
        if (im::module_headers(k32).wine_builtin) {
            std::string page(reinterpret_cast<const char*>(k32), 4096);
            std::ofstream(dir / L"builtin-marked.dll", std::ios::binary) << page;
            for (size_t i = 0; i < 16; ++i) page[0x40 + i] = 0;
            std::ofstream(dir / L"builtin-unmarked.dll", std::ios::binary) << page;
            CHECK(swap_signs((dir / L"builtin-marked.dll").c_str(), k32, &loaded) == 0);  // a builtin file is no swap
            CHECK(swap_signs((dir / L"builtin-unmarked.dll").c_str(), k32, &loaded) == im::kWineBuiltin);
            std::printf("seam: header fields and Wine's builtin marker each prove a swap on their own\n");
        } else {
            std::printf("seam: header fields prove a swap; kernel32 carries no Wine marker here, that sign is not run\n");
        }
        // The wording, wide (the proxy) and UTF-8 (the core's chain.ini [late]) alike; a line that does not fit is not
        // written cut short.
        wchar_t w[512];
        char a[512];
        const std::string own = "p: requested C:\\g\\XInput9_1_0.DLL but another file was loaded in its place: Wine's own "
                                "XInput9_1_0; on Wine or Proton a DLL override is needed: WINEDLLOVERRIDES=\"XInput9_1_0=n,b\"";
        CHECK(im::warning(w, 512, L"p: ", L"C:\\g\\XInput9_1_0.DLL", im::Instead::wine_own, L"ignored") && u8(w) == own);
        CHECK(im::warning(a, 512, "p: ", "C:\\g\\XInput9_1_0.DLL", im::Instead::wine_own, "ignored") && a == own);
        CHECK(im::warning(a, 512, "", "d/x.dll", im::Instead::reported_path, "C:\\other.dll") &&
              std::string(a) == "requested d/x.dll but another file was loaded in its place: C:\\other.dll; on Wine or "
                                "Proton a DLL override is needed: WINEDLLOVERRIDES=\"x=n,b\"");
        CHECK(im::warning(a, 512, "", "x.dll", im::Instead::unknown, static_cast<const char*>(nullptr)) &&
              std::string(a) == "requested x.dll but another file was loaded in its place; on Wine or Proton a DLL "
                                "override is needed: WINEDLLOVERRIDES=\"x=n,b\"");
        CHECK(!im::warning(a, 40, "", "x.dll", im::Instead::unknown, static_cast<const char*>(nullptr)) && a[0] == 0);
        CHECK(im::instead(true, loaded) == im::Instead::reported_path);
    }
    std::printf("proxy chain: %u checks passed\n", checks);
    return 0;
}
