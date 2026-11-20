// SPDX-License-Identifier: MIT
// toolwear-test (native part): a native rule owner of kiln.toolwear, for the case toolwear-native-owner of the managed
// mod (ToolwearTestMod.cs). A plugin for a lab server only: copy kiln-toolwear-test.dll to <lab>/kiln/plugins/. The
// managed mod drives it through files in its data folder (<kiln>/data/toolwear-test/), read every 5 steps on the tick:
//   native-go.txt      (the pickaxe's item id, hex): two owners of this plugin change the pickaxe's rule four times in
//                      this one tick: owner c defines 5, replaces it by 6, owner d defines 6 on top, then removes it.
//                      The plugin reads the four changes itself with rule_changes (its own cursor: nothing is taken
//                      from Kiln's managed events) and answers in native-ack.txt.
//   native-retire.txt  both owners are revoked (owner_revoke): owner c's rule goes by kiln.toolwear's retirement
//                      observer, revealing the managed mod's rule. Answer: native-ack.txt "retired".
// Every line it writes starts with "toolwear-test: native".
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "kiln_api.h"
#include "services/kiln_toolwear.h"

namespace {

const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0, g_c = 0, g_d = 0;
const KilnToolWearApi* TW = nullptr;
std::wstring g_dir;  // <kiln>/data/toolwear-test/
uint64_t g_steps = 0;

KilnStr ks(const char* s) { return KilnStr{s, std::strlen(s)}; }

void say(const char* fmt, ...) {
    char b[1200];
    int n = std::snprintf(b, sizeof b, "toolwear-test: native ");
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b + n, sizeof b - n, fmt, ap);
    va_end(ap);
    g_api->log(g_owner, KILN_LOG_INFO, KilnStr{b, std::strlen(b)});
}

std::wstring wide(const char* s, size_t n) {
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s, static_cast<int>(n), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, static_cast<int>(n), w.data(), static_cast<int>(w.size()));
    return w;
}

// Reads and deletes <dir>/name. False: not there.
bool take(const wchar_t* name, std::string* text) {
    const std::wstring path = g_dir + name;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char b[256] = {};
    const size_t n = std::fread(b, 1, sizeof b - 1, f);
    std::fclose(f);
    DeleteFileW(path.c_str());
    text->assign(b, n);
    return true;
}

// Writes <dir>/name whole (a temporary file moved over it, so the reader never sees half of it).
void put(const wchar_t* name, const std::string& text) {
    const std::wstring path = g_dir + name, tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

uint64_t generation() {
    KilnToolWearState s{};
    s.size = sizeof s;
    return TW->status(0, &s) == KILN_OK ? s.generation : 0;
}

void go(uint32_t item) {
    const uint64_t g0 = generation();
    g_c = g_api->owner_create(ks("toolwear-native-c"));
    g_d = g_api->owner_create(ks("toolwear-native-d"));
    KilnHandle c1 = 0, c2 = 0, d = 0;
    const KilnResult r1 = TW->define(g_c, ks("toolwear-native-c"), item, 5, &c1);
    const KilnResult r2 = TW->define(g_c, ks("toolwear-native-c"), item, 6, &c2);
    const KilnResult r3 = TW->define(g_d, ks("toolwear-native-d"), item, 6, &d);
    const KilnResult r4 = TW->remove(g_d, d);
    KilnToolWearRuleChange rec[8] = {};
    uint32_t n = 0;
    uint64_t lost = 0;
    const KilnResult r5 = TW->rule_changes(g0, rec, 8, sizeof rec[0], &n, &lost);
    bool order = n == 4 && lost == 0;
    static const int32_t kAfter[4] = {5, 6, 6, 6};
    static const uint32_t kCause[4] = {KILN_TOOLWEAR_RULE_DEFINED, KILN_TOOLWEAR_RULE_DEFINED, KILN_TOOLWEAR_RULE_DEFINED, KILN_TOOLWEAR_RULE_REMOVED};
    for (uint32_t i = 0; order && i < 4; ++i)
        order = rec[i].generation == g0 + 1 + i && rec[i].item == item && rec[i].after_cost == kAfter[i] && rec[i].cause == kCause[i];
    const bool ok = r1 == KILN_OK && r2 == KILN_OK && r3 == KILN_OK && r4 == KILN_OK && r5 == KILN_OK && c1 && c1 == c2 && d && order;
    char b[300];
    std::snprintf(b, sizeof b, "phase1 ok=%d rc=%d,%d,%d,%d,%d g0=%llu read=%u lost=%llu first=%d@%s", ok ? 1 : 0, r1, r2, r3, r4, r5,
                  static_cast<unsigned long long>(g0), n, static_cast<unsigned long long>(lost), n ? rec[0].before_cost : 0, n ? rec[0].before_mod_id : "");
    say("%s", b);
    put(L"native-ack.txt", b);
}

void KILN_CALL on_pre(const KilnTickInfo*, void*) {
    if (!TW || ++g_steps % 5) return;
    std::string text;
    if (take(L"native-go.txt", &text)) go(static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 16)));
    if (take(L"native-retire.txt", &text)) {
        const uint64_t g0 = generation();
        g_api->owner_revoke(g_c);
        g_api->owner_revoke(g_d);
        char b[120];
        std::snprintf(b, sizeof b, "retired generation %llu->%llu", static_cast<unsigned long long>(g0), static_cast<unsigned long long>(generation()));
        say("%s", b);
        put(L"native-ack.txt", b);
    }
}

}  // namespace

extern "C" __declspec(dllexport) KilnResult KILN_CALL kiln_plugin_main(const KilnApi* api, KilnOwner owner, KilnPluginInfo* info) {
    if (!api || api->abi_version != KILN_ABI_VERSION) return KILN_ERR_UNAVAILABLE;
    g_api = api;
    g_owner = owner;
    info->id = ks("toolwear-test-native");
    info->version = ks("0.1.0");
    char path[1024];
    size_t len = 0;
    if (api->path(KILN_PATH_DATA, path, sizeof path, &len) == KILN_OK) g_dir = wide(path, len) + L"\\toolwear-test\\";
    if (info->size >= offsetof(KilnPluginInfo, on_ready) + sizeof(info->on_ready))
        info->on_ready = [](void*) {
            TW = static_cast<const KilnToolWearApi*>(g_api->service(ks(KILN_TOOLWEAR_SERVICE), 1));
            if (TW && TW->header.size < offsetof(KilnToolWearApi, rule_changes) + sizeof(void*)) TW = nullptr;
            say("kiln.toolwear %s; data folder %s", TW ? "with rule_changes found" : "with rule_changes is not here", g_dir.empty() ? "unknown" : "found");
        };
    KilnHandle h = 0;
    api->subscribe_tick(owner, KILN_TICK_PRE, &on_pre, nullptr, &h);
    return KILN_OK;
}
