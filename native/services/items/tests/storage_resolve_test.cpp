// SPDX-License-Identifier: MIT
// The production C++ resolver on read-only executable copies, with the existing diagnostic platform shim.
// Does not load an executable, install a hook, call game code or start a game process.
#define __try try
#define __except(x) catch (...)
#include "../../../core/src/sigdb.cpp"
#include <fstream>
#include <iterator>
#include <cstdio>

namespace kiln {
ImageInfo test_image;
const ImageInfo& image() { return test_image; }
const Paths& paths() { static Paths p; return p; }
bool build_verified(uint32_t, const std::string&, std::string*) { return true; }
double now_ms() { return 0; }
std::string to_utf8(std::wstring_view w) { return std::string(w.begin(), w.end()); }
namespace log { void writef(int, const char*, const char*, ...) {} }
}

namespace {
using Json = nlohmann::ordered_json;
unsigned checks = 0, failed = 0;
void check(bool ok, const std::string& text) {
    ++checks;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text.c_str());
    if (!ok) ++failed;
}
void run(const std::string& side, const std::filesystem::path& exe, const Json& doc,
         const std::filesystem::path& scratch, const Json& expected) {
    std::ifstream input(exe, std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(input)), {});
    kiln::pe::FileText ft;
    std::string error;
    if (!kiln::pe::parse_file_text(file.data(), file.size(), ft, error)) {
        check(false, side + " readable PE: " + error); return;
    }
    std::vector<uint8_t> live(ft.size_of_image, 0);
    std::memcpy(live.data() + ft.text_rva, file.data() + ft.text_raw, ft.file_bytes());
    auto& image = kiln::test_image;
    image = {};
    image.base = reinterpret_cast<uintptr_t>(live.data());
    image.size = ft.size_of_image;
    image.text = live.data() + ft.text_rva;
    image.text_size = ft.text_vsize;
    image.timestamp = ft.timestamp;
    image.side = side == "server" ? KILN_SIDE_SERVER : KILN_SIDE_CLIENT;
    image.file = file.data();
    image.file_size = file.size();
    kiln::sigdb::g_code = {};
    // The production code view is once-per-process; this harness deliberately switches image between builds.
    kiln::sigdb::init_code();
    auto resolve = [&](const Json& definition, bool ready) {
        const auto path = scratch / (side + "-storage.json");
        // client.d extends this shared definition; composition sets its side before runtime loading.
        Json composed = definition;
        composed["side"] = side;
        { std::ofstream out(path); out << composed.dump(); }
        kiln::sigdb::Resolver resolver;
        check(resolver.load(path, ""), side + " definition loads");
        resolver.resolve();
        check(resolver.feature("items.storage") == ready, side + (ready ? " storage on" : " incomplete storage off"));
        if (ready) {
            for (const auto& [name, value] : expected["symbols"].items()) {
                uintptr_t address = 0;
                const auto rva = std::stoull(value.get<std::string>(), nullptr, 16);
                check(resolver.symbol(name, &address) && address == image.base + rva, side + " " + name);
            }
            for (const auto& [name, value] : expected["constants"].items()) {
                int64_t result = 0;
                check(resolver.constant(name, &result) && result == value.get<int64_t>(), side + " " + name);
            }
        }
    };
    resolve(doc, true);
    for (const auto& name : doc["features"]["items.storage"]["symbols"]) {
        Json damaged = doc;
        const auto key = name.get<std::string>();
        if (!damaged["symbols"].contains(key)) {
            for (auto& [symbol, definition] : damaged["symbols"].items()) {
                for (auto& strategy : definition["find"]) {
                    auto pattern = strategy["pattern"].get<std::string>();
                    const std::string needle = "{" + key + ":";
                    for (size_t at; (at = pattern.find(needle)) != std::string::npos;)
                        pattern.replace(at, needle.size(), "{omitted:");
                    strategy["pattern"] = pattern;
                }
            }
        } else {
            damaged["symbols"][key]["find"] = Json::array({{{"pattern", "DE AD BE EF 01 23 45 67 89 AB CD EF 98 76 54 32"}}});
        }
        resolve(damaged, false);
    }
    // Change the required vote count: no constant can silently become an optional row.
    for (const auto& [name, value] : doc["constants"].items()) {
        Json damaged = doc;
        damaged["constants"][name]["min_votes"] = 10000;
        resolve(damaged, false);
    }
}
}
int main(int argc, char** argv) {
    if (argc != 6) { std::fprintf(stderr, "usage: storage-resolve server-copy client-copy definition expected scratch\n"); return 2; }
    Json doc, expected;
    std::ifstream(argv[3]) >> doc;
    std::ifstream(argv[4]) >> expected;
    std::filesystem::create_directories(argv[5]);
    run("server", argv[1], doc, argv[5], expected["server"]);
    run("client", argv[2], doc, argv[5], expected["client"]);
    std::printf("native storage resolver: %u checks, %u failed\n", checks, failed);
    return failed ? 1 : 0;
}
