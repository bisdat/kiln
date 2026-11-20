// SPDX-License-Identifier: MIT
// Stand-in Windows host for kiln-engine.dll (headless Wine; never a game binary): the real DLL reads this process's own
// PE headers, file name and <exe>.kfc header, finds no exact profile, and says so in identity and the rows. The
// internal table's copy runs under the real SEH: guard, no-access and reserved pages fail the copy, not the process.
// Mode "gate-server"/"gate-client" loads kiln-engine-test.dll instead (tests/test_facts_win.cpp): this host names an
// exact profile and a fake engine laid out from it, and the real Interlocked words, SEH writes and the SEH tracker call
// run the Scene gate; the tracker's guard bytes sit on a non-executable page, so the call faults and is contained.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "fake_engine.h"
#include "include/kiln_engine_native.h"
#include "profiles.g.h"
#include "services/kiln_caps.h"
#include "services/kiln_engine.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}
std::map<std::string, const void*> tables;
std::map<std::string, uint32_t> rows;
KilnCapsApi caps{};
KilnResult KILN_CALL report(KilnOwner, KilnStr n, uint32_t s, uint32_t, KilnStr) {
    rows[std::string(n.ptr, n.len)] = s;
    return KILN_OK;
}
const void* KILN_CALL service(KilnStr n, uint32_t) {
    const std::string name(n.ptr, n.len);
    if (name == KILN_CAPS_SERVICE) return &caps;
    const auto at = tables.find(name);
    return at == tables.end() ? nullptr : at->second;
}
KilnResult KILN_CALL publish(KilnOwner, KilnStr n, const void* t) {
    tables[std::string(n.ptr, n.len)] = t;
    return KILN_OK;
}
void KILN_CALL log(KilnOwner, int32_t, KilnStr t) { std::printf("log: %.*s\n", static_cast<int>(t.len), t.ptr); }
int g_world_thread = -1;
bool KILN_CALL on_world(uint32_t kind) { return g_world_thread >= 0 && kind == static_cast<uint32_t>(g_world_thread); }
KilnResult KILN_CALL build(KilnBuildInfo* out) {
    std::memset(out->exe_sha256, 'a', 64);
    out->exe_sha256[64] = 0;
    out->side = KILN_SIDE_CLIENT;
    return KILN_OK;
}
}  // namespace

using TestSet = void(KILN_CALL*)(const char*, const char*, std::uint32_t, std::uint32_t, std::uintptr_t);

int gate_mode(const wchar_t* dll_path, bool server) {
    using namespace kiln::engine;
    const Profile* profile = nullptr;
    for (const auto& p : generated::kProfiles)
        if ((std::strcmp(p.target, "enshrouded_server.exe") == 0) == server) profile = &p;
    check(profile != nullptr, "a ported profile for this side");
    if (!profile) return 1;
    const Layout& L = profile->layout;
    fake::Engine e(L);
    const HMODULE dll = LoadLibraryW(dll_path);
    check(dll != nullptr, "kiln-engine-test.dll loads");
    if (!dll) return 1;
    auto set = reinterpret_cast<TestSet>(GetProcAddress(dll, "kiln_engine_test_set"));
    auto main_fn = reinterpret_cast<KilnPluginMainFn>(GetProcAddress(dll, "kiln_plugin_main"));
    check(set && main_fn, "test exports");
    if (!set || !main_fn) return 1;
    set(profile->target, profile->sha256, profile->timestamp, profile->size_of_image, e.image.at());
    caps.header = {sizeof caps, KILN_CAPS_VERSION};
    caps.report = &report;
    KilnApi api{};
    api.size = sizeof api;
    api.abi_version = KILN_ABI_VERSION;
    api.service = &service;
    api.register_service = &publish;
    api.log = &log;
    api.build_info = &build;
    api.on_world_thread = &on_world;
    KilnPluginInfo info{};
    info.size = sizeof info;
    check(main_fn(&api, 5, &info) == KILN_OK, "kiln_plugin_main OK");
    const auto* n = static_cast<const KilnEngineNativeApi*>(tables["kiln.internal.engine"]);
    check(n && n->header.size == sizeof(KilnEngineNativeApi) && n->profile() && std::strcmp(n->profile()->id, profile->id) == 0,
          "exact profile selected for the fake engine");
    if (!n || !n->profile()) return 1;
    const auto words_free = [&] { return *fake::word(e.registry_word()) == 0 && *fake::word(e.scene_word()) == 0; };
    EcsView view{};
    std::uint32_t reason = 0;
    check(n->ecs_view(0, &view, &reason) == KILN_OK && view.role == Role::LocalAuthority, "authority view (real SEH reads)");
    {
        g_world_thread = KILN_WORLD_SERVER;
        EcsGateScope in_frame(*n, view, 1, 100);
        check(!in_frame.held() && in_frame.reason() == Reason::InFrame && words_free(), "in-frame refused, words untouched");
        g_world_thread = -1;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    {
        EcsGateScope scope(*n, view, 1, 101);
        check(scope.held() && *fake::word(e.registry_word()) == 1 &&
                  *fake::word(e.scene_word()) == static_cast<std::int32_t>(0x80000000u),
              "acquired with the real Interlocked words");
        check(GetThreadPriority(GetCurrentThread()) >= THREAD_PRIORITY_ABOVE_NORMAL,
              "the holder is raised above normal (Wine/Proton may ignore this; to be confirmed on Windows)");
        check(scope.validate() && scope.component_supported(3), "validate and component policy");
        const std::uint64_t payload = 0x0102030405060708ull;
        check(scope.write(e.table.at(512), &payload, sizeof payload) == KILN_OK &&
                  fake::get<std::uint64_t>(e.table.at(512)) == payload,
              "guarded write");
        std::vector<unsigned char> big(n->profile()->layout.write.maximum_bytes + 1, 0xcd);
        check(scope.write(e.table.at(512), big.data(), big.size()) == KILN_ERR_BUFFER_TOO_SMALL &&
                  fake::get<std::uint64_t>(e.table.at(512)) == payload,
              "a write over the budget refuses with BUFFER_TOO_SMALL and changes nothing");
        check(scope.write(fake::unmapped(), &payload, sizeof payload) == KILN_ERR_UNAVAILABLE && scope.held(),
              "a write to a no-access page faults under SEH; the gate stays held");
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(reinterpret_cast<void*>(e.image.at(L.write.tracker)), &mbi, sizeof mbi);
        const bool executable = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        check(!executable, "the fake tracker's page is not executable");
        if (!executable)
            check(!scope.track_change(0x5000, 3) && scope.held() && scope.validate(),
                  "the tracker call faults (no execute) and is contained; the gate stays held and valid");
    }
    check(words_free() && GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_BELOW_NORMAL,
          "the scope released both words and restored the thread priority after the faults");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    KilnEngineGateStats st{};
    st.size = sizeof st;
    const auto* t = static_cast<const KilnEngineApi*>(tables[KILN_ENGINE_SERVICE]);
    check(t && t->gate_stats(&st) == KILN_OK && st.acquired == 1 && st.refused_in_frame == 1 && st.hold_ns_max > 0,
          "gate stats: one hold, one in-frame refusal, a hold time");
    std::printf("engine Windows gate (%s): %d failure(s), max hold %llu ns\n", profile->id, failures,
                static_cast<unsigned long long>(st.hold_ns_max));
    return failures ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    if (std::wstring(argv[2]) == L"gate-server" || std::wstring(argv[2]) == L"gate-client")
        return gate_mode(argv[1], std::wstring(argv[2]) == L"gate-server");
    const std::wstring kfc_mode = argv[2];  // "kfc4": write <exe>.kfc with KFC4 first; "none": remove it
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring kfc = self;
    kfc.replace(kfc.size() - 4, 4, L".kfc");
    DeleteFileW(kfc.c_str());
    if (kfc_mode == L"kfc4") {
        const HANDLE f = CreateFileW(kfc.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD n = 0;
        WriteFile(f, "KFC4rest", 8, &n, nullptr);
        CloseHandle(f);
    }
    const HMODULE dll = LoadLibraryW(argv[1]);
    check(dll != nullptr, "kiln-engine.dll loads");
    if (!dll) return 1;
    auto main_fn = reinterpret_cast<KilnPluginMainFn>(GetProcAddress(dll, "kiln_plugin_main"));
    check(main_fn != nullptr, "kiln_plugin_main export");
    caps.header = {sizeof caps, KILN_CAPS_VERSION};
    caps.report = &report;
    KilnApi api{};
    api.size = sizeof api;
    api.abi_version = KILN_ABI_VERSION;
    api.service = &service;
    api.register_service = &publish;
    api.log = &log;
    api.build_info = &build;
    KilnPluginInfo info{};
    info.size = sizeof info;
    check(main_fn && main_fn(&api, 5, &info) == KILN_OK, "kiln_plugin_main OK on an unknown build");
    const auto* t = static_cast<const KilnEngineApi*>(service({KILN_ENGINE_SERVICE, 11}, 1));
    check(t && tables.count("kiln.internal.engine"), "public and internal tables published");
    KilnEngineIdentity id{};
    id.size = sizeof id;
    check(t && t->identity(&id) == KILN_OK, "identity");
    const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    std::printf("identity: status=%u target=%s timestamp=%u size=%u kfc=%u sha=%.8s... profile='%s'\n", id.status, id.target,
                id.timestamp, id.size_of_image, id.archive_format, id.sha256, id.profile_id);
    check(id.status == KILN_ENGINE_UNKNOWN_BUILD && !id.profile_id[0], "no exact profile for a stand-in exe");
    check(id.timestamp == nt->FileHeader.TimeDateStamp && id.size_of_image == nt->OptionalHeader.SizeOfImage,
          "TimeDateStamp and SizeOfImage come from the loaded image");
    check(std::strcmp(id.target, "engine-host.exe") == 0, "target is the exe file name");
    check(id.archive_format == (kfc_mode == L"kfc4" ? 4u : 0u), "KFC header read from <exe>.kfc");
    check(rows["engine.profile"] == KILN_CAP_UNAVAILABLE && rows["engine.ecs_write"] == KILN_CAP_UNAVAILABLE, "rows unavailable");
    const auto* n = static_cast<const KilnEngineNativeApi*>(tables["kiln.internal.engine"]);
    check(n && n->header.size == sizeof(KilnEngineNativeApi) && !n->layout() && !n->profile(), "internal table, no profile");
    if (n) {
        unsigned long long value = 0x0123456789abcdefull, out = 0;
        check(n->copy(reinterpret_cast<std::uintptr_t>(&value), &out, sizeof out) == KILN_OK && out == value, "copy reads");
        auto* guard = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD));
        auto* none = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
        auto* reserved = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS));
        auto* pair = static_cast<unsigned char*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD old = 0;
        check(guard && none && reserved && pair && VirtualProtect(pair + 4096, 4096, PAGE_NOACCESS, &old), "test pages");
        const auto at = [](const void* p) { return reinterpret_cast<std::uintptr_t>(p); };
        check(n->copy(at(guard), &out, sizeof out) == KILN_ERR_UNAVAILABLE, "a guard-page fault fails the copy");
        std::printf("guard page after the fault: copy %s (Windows clears PAGE_GUARD on first touch)\n",
                    n->copy(at(guard), &out, sizeof out) == KILN_OK ? "reads" : "still fails");
        check(n->copy(at(none), &out, sizeof out) == KILN_ERR_UNAVAILABLE, "a no-access page fails the copy");
        check(n->copy(at(reserved), &out, sizeof out) == KILN_ERR_UNAVAILABLE, "a reserved page fails the copy");
        check(n->copy(at(pair + 4092), &out, sizeof out) == KILN_ERR_UNAVAILABLE, "a range running into a no-access page fails");
        check(n->copy(at(pair + 4088), &out, sizeof out) == KILN_OK, "the readable part still reads");
        kiln::engine::EcsView view{};
        std::uint32_t reason = 0;
        check(n->ecs_view(0, &view, &reason) == KILN_ERR_UNAVAILABLE &&
                  reason == static_cast<std::uint32_t>(kiln::engine::Reason::EcsReadOwnerProfileUnavailable) &&
                  n->ecs_write_supported(&view, 3) == 0,
              "no view and no write support without an exact profile");
    }
    DeleteFileW(kfc.c_str());
    std::printf("engine Windows host: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
