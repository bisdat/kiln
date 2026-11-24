// SPDX-License-Identifier: MIT
// The actual client probe must refuse a stale table before registering anything.
#define __declspec(x)
using DWORD = unsigned long;
inline DWORD GetCurrentThreadId() { return 1; }
#include "../../../../mods/tests/emf-client-test/probe.cpp"

static int checks = 0, failures = 0, registrations = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
static void log_line(const char*, const char*, ...) {}
int main() {
    EmfApi table{};
    table.log = &log_line;
    table.register_program = [](const char*, uint8_t, EmfProgramCallback, void*) { ++registrations; return true; };
    table.register_program_def = [](const EmfProgramDef*) { ++registrations; return true; };
    table.on_event = [](uint8_t, const char*, uint32_t, EmfWorldEventCallback, void*) { ++registrations; return true; };
    table.on_tick = [](EmfWorldCallback, void*) { ++registrations; };
    table.version = 46;
    table.size = 1144;
    CHECK(!emf_mod_load(&table));
    CHECK(registrations == 0);
    table.version = 47;
    table.size = 1160;  // v47's table: the same size, an older version
    CHECK(!emf_mod_load(&table));
    CHECK(registrations == 0);
    table.version = 48;
    table.size = 1144;
    CHECK(!emf_mod_load(&table));
    CHECK(registrations == 0);
    table.size = 1160;
    CHECK(!emf_mod_load(&table));  // the current-header probe requires v49, even with the unchanged v48 table size
    CHECK(registrations == 0);
    table.version = 49;
    table.size = 1159;
    CHECK(!emf_mod_load(&table));
    CHECK(registrations == 0);
    table.size = 1160;
    CHECK(emf_mod_load(&table));
    CHECK(registrations == 6);
    std::printf("client_probe_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
