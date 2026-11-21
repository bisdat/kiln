// SPDX-License-Identifier: MIT
// boot_archive_eml.cpp on the Windows code path (headless Wine and native Windows): kiln.boot's kiln-backend sources
// as kiln-core.dll registers them when it loads. kiln.eml.archive is found only through the export of a loaded module
// named kiln-eml.dll (a stand-in, boot_archive_eml_standin.c), never one of another name, never by loading one, and
// not after it was unloaded; the kiln backend and its launch facts come from kiln.launch (stubbed here); the identity
// is kiln.eml.startup's session-free published_identity from the same loaded kiln-eml.dll only: handed with the
// launch's key once kiln-eml granted, refused with its reason before, by a kiln-eml whose table has no such member, by
// a module of another name and once kiln-eml.dll is unloaded. Run by test-boot-archive-eml-wine.sh (and on native
// Windows).
// PRIVATE (tools/sfi/public_cut.py). Prints "BOOT ARCHIVE EML WINDOWS <n> PASS <m> FAIL"; exits 1 when anything failed.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "launch.h"
#include "services/boot_archive_eml.h"

namespace kiln::launch {
uint32_t t_backend = KILN_LAUNCH_BACKEND_NONE;
KilnResult t_status_rc = KILN_OK;
uint32_t backend() { return t_backend; }
static KilnResult KILN_CALL status(KilnLaunchStatus* out) {
    if (t_status_rc != KILN_OK) return t_status_rc;
    out->backend = t_backend;
    out->frozen = 1;
    out->launch_id = 0x1122334455667788ull;
    for (int i = 0; i < 32; ++i) out->plan_hash[i] = static_cast<uint8_t>(i + 1);
    return KILN_OK;
}
const KilnLaunchApi* table() {
    static const KilnLaunchApi t{{sizeof(KilnLaunchApi), KILN_LAUNCH_VERSION}, &status, nullptr};
    return &t;
}
}  // namespace kiln::launch

namespace kiln::boot_svc {
const boot_view::CommittedSource* t_source = nullptr;
void use_committed_generation(const boot_view::CommittedSource* source) { t_source = source; }
}  // namespace kiln::boot_svc

static int t_pass = 0, t_fail = 0;
#define CHECK(x) check((x), #x, __LINE__)
static void check(bool ok, const char* what, int line) {
    if (ok) {
        ++t_pass;
        return;
    }
    ++t_fail;
    std::printf("FAIL %s (line %d)\n", what, line);
}

int main() {
    using kiln::boot_svc::t_source;
    // registered when the code loaded, before main
    CHECK(t_source && t_source->kiln_mode && t_source->table && t_source->identity);
    if (!t_source) return 1;
    // no kiln-eml.dll in the process: no table, and nothing is loaded to find one
    CHECK(GetModuleHandleW(L"kiln-eml.dll") == nullptr && t_source->table() == nullptr && GetModuleHandleW(L"kiln-eml.dll") == nullptr);
    // a module of another name exporting the same getter is not kiln-eml
    HMODULE other = LoadLibraryW(L"other-name.dll");
    CHECK(other != nullptr && t_source->table() == nullptr);
    // kiln-eml.dll loaded: its getter's table, asked for version 1
    HMODULE eml = LoadLibraryW(L"kiln-eml.dll");
    CHECK(eml != nullptr);
    const KilnEmlArchiveApiV1* table = t_source->table();
    using Asked = unsigned(__cdecl*)();
    const auto asked = reinterpret_cast<Asked>(GetProcAddress(eml, "standin_asked"));
    CHECK(table != nullptr && table->header.size == sizeof(KilnEmlArchiveApiV1) && asked && asked() == KILN_EML_ARCHIVE_VERSION);
    // unloaded: none again (no reference was taken that kept it loaded)
    CHECK(FreeLibrary(eml) && GetModuleHandleW(L"kiln-eml.dll") == nullptr && t_source->table() == nullptr);
    if (other) FreeLibrary(other);
    // kiln.launch: hosted and none are no kiln backend; kiln is, with its launch id and plan hash
    kiln::boot_view::LaunchFacts facts;
    kiln::launch::t_backend = KILN_LAUNCH_BACKEND_HOSTED;
    CHECK(!t_source->kiln_mode(&facts) && facts.launch_id == 0);
    kiln::launch::t_backend = KILN_LAUNCH_BACKEND_NONE;
    CHECK(!t_source->kiln_mode(&facts));
    kiln::launch::t_backend = KILN_LAUNCH_BACKEND_KILN;
    CHECK(t_source->kiln_mode(&facts) && facts.launch_id == 0x1122334455667788ull && facts.plan_sha256[0] == 1 && facts.plan_sha256[31] == 32);
    // the kiln backend with a record that cannot be read: still the kiln backend (no snapshot), with no launch facts
    kiln::boot_view::LaunchFacts none;
    kiln::launch::t_status_rc = KILN_ERR_UNAVAILABLE;
    CHECK(t_source->kiln_mode(&none) && none.launch_id == 0);
    // the identity: no kiln-eml.dll loaded: no kiln.eml.startup table, and nothing is loaded to find one
    KilnEmlArchiveIdentityV1 id{};
    id.size = sizeof id;
    std::string why;
    CHECK(t_source->identity(facts, &id, &why) == KILN_ERR_UNAVAILABLE && why.find("offers no kiln.eml.startup table") != std::string::npos &&
          id.generation == 0 && GetModuleHandleW(L"kiln-eml.dll") == nullptr);
    // a module of another name exporting the same getter, even one that grants, is not kiln-eml
    HMODULE other2 = LoadLibraryW(L"other-name.dll");
    using Grant = void(__cdecl*)(int);
    if (other2) reinterpret_cast<Grant>(GetProcAddress(other2, "standin_grant"))(1);
    CHECK(other2 != nullptr && t_source->identity(facts, &id, &why) == KILN_ERR_UNAVAILABLE && id.generation == 0);
    // kiln-eml.dll before it granted: kiln-eml's own refusal, the session-free key with this launch
    HMODULE eml2 = LoadLibraryW(L"kiln-eml.dll");
    CHECK(eml2 != nullptr);
    const auto grant = reinterpret_cast<Grant>(GetProcAddress(eml2, "standin_grant"));
    using KeyLaunch = uint64_t(__cdecl*)();
    const auto key_launch = reinterpret_cast<KeyLaunch>(GetProcAddress(eml2, "standin_key_launch"));
    const auto startup_asked = reinterpret_cast<Asked>(GetProcAddress(eml2, "standin_startup_asked"));
    CHECK(grant && key_launch && startup_asked);
    if (grant && key_launch && startup_asked) {
        CHECK(t_source->identity(facts, &id, &why) == KILN_ERR_UNAVAILABLE && why.find("granted no archive generation") != std::string::npos &&
              id.generation == 0 && key_launch() == facts.launch_id && startup_asked() == KILN_EML_STARTUP_VERSION);
        // granted: the identity it hands, for this launch
        grant(1);
        CHECK(t_source->identity(facts, &id, &why) == KILN_OK && id.generation == 77 && id.phase == KILN_EML_ARCHIVE_PUBLISHED &&
              id.launch_id[15] == static_cast<uint8_t>(facts.launch_id) && id.plan_sha256[31] == 32 && id.size == sizeof id);
    }
    // unloaded: none again
    if (eml2) CHECK(FreeLibrary(eml2) && GetModuleHandleW(L"kiln-eml.dll") == nullptr);
    KilnEmlArchiveIdentityV1 none_id{};
    none_id.size = sizeof none_id;
    CHECK(t_source->identity(facts, &none_id, &why) == KILN_ERR_UNAVAILABLE && none_id.generation == 0);
    if (other2) FreeLibrary(other2);
    // a kiln-eml.dll from before published_identity (a 48-byte kiln.eml.startup table): too old, with that reason
    wchar_t self[MAX_PATH]{};
    const DWORD self_len = GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring old_path(self, self_len);
    old_path = old_path.substr(0, old_path.find_last_of(L"\\/") + 1) + L"old\\kiln-eml.dll";
    HMODULE old_eml = LoadLibraryW(old_path.c_str());
    CHECK(old_eml != nullptr && GetModuleHandleW(L"kiln-eml.dll") == old_eml);
    CHECK(t_source->identity(facts, &none_id, &why) == KILN_ERR_UNAVAILABLE && why.find("no published_identity") != std::string::npos &&
          none_id.generation == 0);
    if (old_eml) FreeLibrary(old_eml);
    std::printf("BOOT ARCHIVE EML WINDOWS %d PASS %d FAIL\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
