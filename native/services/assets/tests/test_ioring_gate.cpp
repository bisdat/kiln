// SPDX-License-Identifier: MIT
// KR-KZRX (2026-11-28): Windows 11 players crashed in the main menu ("error loading streamed texture mip tail: eof reached")
// as soon as a mod had blobs Kiln serves, because the game read Kiln's container through the I/O ring. This compiles the
// production hk_GetProcAddress from hooks.cpp (ioring_gate.inc, cut out by run.sh) over a fake import and checks that the
// game's I/O ring lookups are not found, once logged, and that every other lookup reaches the import unchanged.
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "ioring_gate.h"

using DWORD = uint32_t;
using HMODULE = void*;
using LPCSTR = const char*;
using FARPROC = intptr_t (*)();
#define WINAPI
constexpr DWORD ERROR_PROC_NOT_FOUND = 127;

namespace {
DWORD last_error = 0;
std::vector<std::string> logged;
std::vector<std::string> forwarded;  // names the fake import was asked for
intptr_t found() { return 1; }
}  // namespace

void SetLastError(DWORD e) { last_error = e; }

namespace ka {
void log_line(const char* fmt, ...) {
    char b[512];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    logged.emplace_back(b);
}
}  // namespace ka
#define LOGI(...) ::ka::log_line(__VA_ARGS__)

namespace ka {
FARPROC WINAPI fake_import(HMODULE, LPCSTR name) {
    forwarded.emplace_back(reinterpret_cast<uintptr_t>(name) <= 0xFFFF ? "#ordinal" : name);
    return &found;
}
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn g_GetProcAddress = &fake_import;
std::atomic<uint32_t> g_ioring_refused{0};
#include "ioring_gate.inc"
}  // namespace ka

static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::cout << (ok ? "PASS " : "FAIL ") << what << "\n";
    if (!ok) ++failures;
}

int main() {
    // Every I/O ring name the game's async IO asks for (client 26430625's strings at 0x141db4368 on), and the rest of
    // ioringapi.h.
    const char* ring[] = {"QueryIoRingCapabilities", "CreateIoRing", "IsIoRingOpSupported", "GetIoRingInfo", "SubmitIoRing",
                          "CloseIoRing", "PopIoRingCompletion", "SetIoRingCompletionEvent", "BuildIoRingReadFile",
                          "BuildIoRingRegisterFileHandles", "BuildIoRingRegisterBuffers", "BuildIoRingCancelRequest",
                          "BuildIoRingWriteFile", "BuildIoRingFlushFile"};
    bool all = true;
    for (const char* n : ring) all = all && ka::ioring_api_name(n);
    check(all, "ioring.names: every I/O ring API name is refused");
    const char* other[] = {"GetProcAddress", "CreateIoCompletionPort", "ReadFile", "ReadFileEx", "GetQueuedCompletionStatus",
                           "CreateFileW", "vkGetInstanceProcAddr", "QueryIo", "ioring", "SteamAPI_Init", ""};
    bool none = true;
    for (const char* n : other) none = none && !ka::ioring_api_name(n);
    check(none && !ka::ioring_api_name(nullptr) && !ka::ioring_api_name(reinterpret_cast<const char*>(uintptr_t{7})) &&
              !ka::ioring_api_name(reinterpret_cast<const char*>(uintptr_t{0xFFFF})),
          "ioring.names: other names, null and ordinals pass");

    HMODULE kernelbase = reinterpret_cast<HMODULE>(uintptr_t{0x7ffd00000000});
    // The game's first lookup: not found, the error a missing export gives, nothing asked of the import, one kiln.log line.
    FARPROC q = ka::hk_GetProcAddress(kernelbase, "QueryIoRingCapabilities");
    check(q == nullptr && last_error == ERROR_PROC_NOT_FOUND && forwarded.empty(),
          "ioring.hook: QueryIoRingCapabilities is not found and never reaches the import");
    check(logged.size() == 1 && logged[0].find("QueryIoRingCapabilities") != std::string::npos &&
              logged[0].find("--disable-ioring") != std::string::npos,
          "ioring.hook: the first refusal is said once in kiln.log");
    last_error = 0;
    check(ka::hk_GetProcAddress(kernelbase, "CreateIoRing") == nullptr && last_error == ERROR_PROC_NOT_FOUND && logged.size() == 1 &&
              ka::g_ioring_refused.load() == 2,
          "ioring.hook: later ring lookups are refused without another line");
    // Everything else is the import's answer, as it was.
    check(ka::hk_GetProcAddress(kernelbase, "CreateFileW") == &found && forwarded.size() == 1 && forwarded[0] == "CreateFileW",
          "ioring.hook: another name reaches the import");
    check(ka::hk_GetProcAddress(kernelbase, reinterpret_cast<LPCSTR>(uintptr_t{42})) == &found && forwarded.size() == 2 &&
              forwarded[1] == "#ordinal",
          "ioring.hook: an ordinal reaches the import");
    return failures ? 1 : 0;
}
