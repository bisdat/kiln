// SPDX-License-Identifier: MIT
// kiln.knowledge stops the game's start (refuse_start, knowledge_internal.h): the roster that cannot be held
// (roster.cpp, package 17c) and the whole build of the mods' native quests and NPCs that failed (native_pack.cpp,
// 0.2.0-dev.29). One way for both; what each says is in start_stop.h. A start-time act only: once a world is present
// or has run in this process, nothing is stopped and an error line says so.
#include <windows.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "kiln_boot_collector.hpp"
#include "knowledge_internal.h"
#include "start_stop.h"

namespace kiln::knowledge {
namespace {

std::wstring wide(const std::string& s) {
    std::wstring w(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
    return w;
}
std::wstring kiln_path(uint32_t kind, const char* leaf) {
    char b[1024];
    size_t n = 0;
    if (g_api->path(kind, b, sizeof b - 1, &n) != KILN_OK) return {};
    return wide(std::string(b, n) + "\\" + leaf);
}

// A world is present now, or one has run in this process (KilnApi.session): the start phase is over.
bool world_started() {
    KilnSessionInfo s{};
    s.size = sizeof s;
    if (g_api->size < offsetof(KilnApi, session) + sizeof(void*) || g_api->session(&s) != KILN_OK) return false;
    for (uint32_t k = 0; k < KILN_WORLD_KINDS; ++k)
        if (s.worlds[k] || s.sessions[k]) return true;
    return false;
}

// The player's message when a stop ends a game client's start: owned by the game's window when it has one, so it is
// not lost behind it, and closed by itself after 60 s so a game behind a fullscreen or Wayland window does not look
// hung (the reason is on disk before it opens).
struct StopBox {
    std::wstring text, title;
};
BOOL CALLBACK find_game_window(HWND h, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    *reinterpret_cast<HWND*>(out) = h;
    return FALSE;
}
void KILN_CALL show_box(void* user) {
    using TimeoutBox = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT, WORD, DWORD);
    const HMODULE u32 = GetModuleHandleW(L"user32.dll");
    const auto fn = u32 ? reinterpret_cast<TimeoutBox>(reinterpret_cast<void*>(GetProcAddress(u32, "MessageBoxTimeoutW"))) : nullptr;
    if (!fn) {
        KW("stop: no message box with a timeout on this system (user32 MessageBoxTimeoutW): the reason is in kiln.log only");
        return;
    }
    HWND owner = nullptr;
    EnumWindows(&find_game_window, reinterpret_cast<LPARAM>(&owner));
    const auto* box = static_cast<const StopBox*>(user);
    fn(owner, box->text.c_str(), box->title.c_str(), MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND, 0, 60000);
}

}  // namespace

void start_stop_main() {
    // the state file of a start a stop ended describes that start only: it moves aside at the next one
    for (const StartStop k : {StartStop::Roster, StartStop::Content}) {
        const std::wstring file = kiln_path(KILN_PATH_STATE, stop::kind(k).state);
        if (!file.empty() && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES &&
            MoveFileExW(file.c_str(), kiln_path(KILN_PATH_STATE, stop::kind(k).last).c_str(), MOVEFILE_REPLACE_EXISTING))
            KW("%s", stop::previous(k).c_str());
    }
}

void refuse_start(StartStop kind, const std::string& why, const std::vector<std::string>& mods) {
    // a start-time call only: a world present now or run in this process (ending it could lose play) is not stopped
    const auto cannot_stop = [&] { KE("%s", stop::cannot_stop(kind, why, mods).c_str()); };
    if (world_started()) return cannot_stop();
    const stop::Kind& k = stop::kind(kind);
    const bool client = g_api->side() == KILN_SIDE_CLIENT;
    const bool by_boot = kiln::boot::can_stop_start(g_api);
    const std::string head = stop::head(kind, GetCurrentProcessId());
    // first the file a launcher reads and the log line: nothing below may keep them from the disk (a player who ends
    // the game while its message is open still has the reason)
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char utc[32];
    std::snprintf(utc, sizeof utc, "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::string doc = stop::state(kind, why, mods, client, by_boot, GetCurrentProcessId(), utc).dump(1) + "\n";
    const std::wstring state = kiln_path(KILN_PATH_STATE, k.state);
    if (!state.empty()) {
        HANDLE f = CreateFileW(state.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            WriteFile(f, doc.data(), static_cast<DWORD>(doc.size()), &wrote, nullptr);
            FlushFileBuffers(f);
            CloseHandle(f);
        }
    }
    KE("%s%s", head.c_str(), stop::line(kind, why, mods).c_str());
    // kiln.boot ends the start the same way with a boot generation or without one, before the game's entry point (a
    // boot generation's prepare) or after it (the game's resource start): its stop line, progress.json "failed",
    // boot.json "boot"."stop", then the message on a client, then the kind's exit code (it does not return)
    StopBox box;
    box.text = wide(stop::box(kind, why, mods));
    box.title = wide(stop::title(kind));
    kiln::boot::stop_start(g_api, g_owner, stop::csv(mods), stop::reason(kind, why, mods), k.exit_code, client ? &show_box : nullptr, &box);
    // kiln.boot did not stop the start. A world present now or run in this process (kiln.boot refuses then too): ending
    // the process could lose play, so it goes on, loudly (a start-time call only)
    if (world_started()) return cannot_stop();
    // a stop under way already (another service): it ends the process within its bound
    if (kiln::boot::can_stop_start(g_api))
        for (;;) Sleep(1000);
    // an older kiln.boot without stop_start: the same by hand (no boot.json stop record, no progress.json "failed")
    if (client) show_box(&box);
    const std::wstring log = kiln_path(KILN_PATH_LOGS, "kiln.log");  // the log line on disk first (its writer is a thread)
    const ULONGLONG until = GetTickCount64() + 1500;
    std::string tail(65536, '\0');
    while (!log.empty() && GetTickCount64() < until) {
        HANDLE f = CreateFileW(log.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        bool found = false;
        if (f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size{};
            GetFileSizeEx(f, &size);
            LARGE_INTEGER at{};
            at.QuadPart = size.QuadPart > static_cast<LONGLONG>(tail.size()) ? size.QuadPart - static_cast<LONGLONG>(tail.size()) : 0;
            SetFilePointerEx(f, at, nullptr, FILE_BEGIN);
            DWORD got = 0;
            if (ReadFile(f, tail.data(), static_cast<DWORD>(tail.size()), &got, nullptr))
                found = std::string_view(tail.data(), got).find(head) != std::string_view::npos;
            CloseHandle(f);
        }
        if (found) break;
        Sleep(10);
    }
    TerminateProcess(GetCurrentProcess(), k.exit_code);
    for (;;) Sleep(1000);
}

}  // namespace kiln::knowledge
