// SPDX-License-Identifier: MIT
// Stand-in Windows host for kiln-engine.dll (headless Wine; never a game binary): the real DLL reads this process's own
// PE headers, file name and <exe>.kfc header, finds no exact profile, and says so in identity and the rows.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

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
KilnResult KILN_CALL build(KilnBuildInfo* out) {
    std::memset(out->exe_sha256, 'a', 64);
    out->exe_sha256[64] = 0;
    out->side = KILN_SIDE_CLIENT;
    return KILN_OK;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
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
    DeleteFileW(kfc.c_str());
    std::printf("engine Windows host: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
