// SPDX-License-Identifier: MIT
// Host test of a native mod the profile has off whose DLL also loads as a chain proxy (../src/chain_copy.h): Kiln's
// proxy loads kiln/chain.ini [chain] and dinput8-chain.ini whatever the profile says, so native.json must not say only
// "disabled in the profile" for it. tests/test-chain-copy.sh builds and runs it. Made-up files; a temporary folder.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "chain_copy.h"

namespace fs = std::filesystem;
namespace cc = kiln::chain_copy;
static int failures = 0, checks = 0;

#define EXPECT(cond)                                                    \
    do {                                                                \
        ++checks;                                                       \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}
static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

int main() {
    const fs::path game = fs::temp_directory_path() / ("kiln-chain-copy-" + std::to_string(std::rand()));
    fs::remove_all(game);
    const std::string body(200000, 'x');  // more than one read block
    write(game / "library/native/fps-unlock/2.0.0/fpsunlock.dll", "MZ" + body + "fps");
    write(game / "kiln/chain/fps-unlock-dinput8.dll", "MZ" + body + "fps");  // the same bytes, put in as a proxy
    write(game / "kiln/chain/other-dinput8.dll", "MZ" + body + "oth");       // same size, other bytes
    write(game / "xinput9_1_0.dll", "MZ asi");
    const fs::path dll = game / "library/native/fps-unlock/2.0.0/fpsunlock.dll";

    std::vector<cc::Row> rows = {
        {game / "xinput9_1_0.dll", "C:\\game\\xinput9_1_0.dll", false, "loaded"},
        {game / "kiln/chain/other-dinput8.dll", "C:\\game\\kiln\\chain\\other-dinput8.dll", false, "loaded"},
        {game / "kiln/chain/fps-unlock-dinput8.dll", "C:\\game\\kiln\\chain\\fps-unlock-dinput8.dll", false, "loaded"},
    };
    EXPECT(cc::find(dll, rows) == 2);
    const std::string why = cc::reason("disabled in the profile", "fpsunlock.dll", rows[2], true);
    std::printf("dllmain: %s\n", why.c_str());
    EXPECT(has(why, "disabled in the profile, but its DLL fpsunlock.dll loads all the same as a chain proxy"));
    EXPECT(has(why, "kiln\\chain.ini [chain] names C:\\game\\kiln\\chain\\fps-unlock-dinput8.dll"));
    EXPECT(has(why, "The mod runs"));
    EXPECT(has(why, "put ';' in front of its line in kiln\\chain.ini [chain]"));
    const std::string boot = cc::reason("disabled in the profile", "fpsunlock.dll", rows[2], false);
    EXPECT(has(boot, "Kiln does not call its init"));

    // a proxy line that did not load (refused, missing, failed) runs nothing
    rows[2].status = "refused";
    EXPECT(cc::find(dll, rows) == -1);
    rows[2].status = "late";
    EXPECT(cc::find(dll, rows) == 2);
    EXPECT(has(cc::reason("disabled in the profile", "fpsunlock.dll", rows[2], true), "after the plugins ([late])"));
    // EMF's list
    rows[2].status = "duplicate";
    rows[2].emf_list = true;
    EXPECT(cc::find(dll, rows) == 2);
    const std::string emf = cc::reason("switched off in kiln\\config\\native.json", "fpsunlock.dll", rows[2], true);
    std::printf("emf list: %s\n", emf.c_str());
    EXPECT(has(emf, "dinput8-chain.ini names") && has(emf, "remove its line from <game>\\dinput8-chain.ini"));
    // nothing of it in the chain: no row
    rows.erase(rows.begin() + 2);
    EXPECT(cc::find(dll, rows) == -1);
    EXPECT(cc::find(game / "missing.dll", rows) == -1);

    fs::remove_all(game);
    std::printf("%d of %d checks failed\n", failures, checks);
    return failures ? 1 : 0;
}
