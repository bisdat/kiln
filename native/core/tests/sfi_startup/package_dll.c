/* SPDX-License-Identifier: MIT
 * a synthetic ShroudForge package DLL for the two-DLL startup host (host.cpp).
 * No game or third-party bytes; no C runtime (entry DllMain, link /NODEFAULTLIB). Built once per package with its own
 * G14D_DLL_ID, so every package ships its own bytes. What its initializer does is chosen when it loads, by the
 * environment variable G14D_DLL_<FILE STEM IN UPPER CASE> (G14D_DLL_DLLPACK for dllpack.dll):
 *   plain (default)   nothing;
 *   touch             appends to the file G14D_TOUCH_FILE names (an input kiln-eml's startup witness covers);
 *   sleep:<ms>        holds the loader lock that long (a slow initializer);
 *   hang              never returns (a hung initializer; the host ends the process);
 *   rewrite-highest   rewrites in place (its first bytes inverted, same size) the highest-numbered non-empty
 *                     <game>\enshrouded_NNN.dat (G14D_GAME): after a publication that is a container the published
 *                     generation added (a cached generation's added container).
 * Each step is a line in G14D_DLL_LOG: "qpc=<QueryPerformanceCounter> tid=<thread> id=<id> <stem> <event>",
 * attach-begin says whether <game>\enshrouded.kfc.bak (G14D_BASELINE: the working baseline kiln-eml's witness captures
 * before any package DLL loads) exists.
 */
#include <windows.h>
#ifndef G14D_DLL_ID
#define G14D_DLL_ID 0
#endif
__declspec(dllexport) int __cdecl g14d_package_id(void) { return G14D_DLL_ID; }

static char g_stem[64];
static WCHAR g_var[96];

static int put(char* out, int at, int cap, const char* s) {
    while (*s && at < cap - 1) out[at++] = *s++;
    return at;
}
static int put_u(char* out, int at, int cap, unsigned long long v) {
    char tmp[24];
    int k = 0;
    do {
        tmp[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && k < 24);
    while (k && at < cap - 1) out[at++] = tmp[--k];
    return at;
}
static void say(const char* event, const char* detail) {
    static WCHAR path[1024];
    static char line[1400];
    int n = 0;
    LARGE_INTEGER now;
    DWORD length = GetEnvironmentVariableW(L"G14D_DLL_LOG", path, 1024), written = 0;
    HANDLE file;
    if (!length || length >= 1024) return;
    QueryPerformanceCounter(&now);
    n = put(line, n, sizeof line, "qpc=");
    n = put_u(line, n, sizeof line, (unsigned long long)now.QuadPart);
    n = put(line, n, sizeof line, " tid=");
    n = put_u(line, n, sizeof line, GetCurrentThreadId());
    n = put(line, n, sizeof line, " id=");
    n = put_u(line, n, sizeof line, G14D_DLL_ID);
    n = put(line, n, sizeof line, " ");
    n = put(line, n, sizeof line, g_stem);
    n = put(line, n, sizeof line, " ");
    n = put(line, n, sizeof line, event);
    if (detail) {
        n = put(line, n, sizeof line, " ");
        n = put(line, n, sizeof line, detail);
    }
    line[n++] = '\n';
    file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, line, (DWORD)n, &written, NULL);
    CloseHandle(file);
}
static int starts(const WCHAR* s, const WCHAR* prefix) {
    while (*prefix)
        if (*s++ != *prefix++) return 0;
    return 1;
}
static void touch(void) {
    static WCHAR path[1024];
    DWORD length = GetEnvironmentVariableW(L"G14D_TOUCH_FILE", path, 1024), written = 0;
    HANDLE file;
    if (!length || length >= 1024) {
        say("touch", "no G14D_TOUCH_FILE");
        return;
    }
    file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        say("touch", "cannot open");
        return;
    }
    WriteFile(file, "changed by a DLL", 16, &written, NULL);
    CloseHandle(file);
    say("touch", "appended 16 bytes");
}
static void rewrite_highest(void) {
    static WCHAR game[1024], pattern[1100], best[MAX_PATH], path[1400];
    static char detail[MAX_PATH + 64];
    WIN32_FIND_DATAW found;
    HANDLE find, file;
    unsigned char bytes[16];
    DWORD length = GetEnvironmentVariableW(L"G14D_GAME", game, 1024), got = 0, written = 0;
    int i, k, n;
    if (!length || length >= 1024) {
        say("rewrite", "no G14D_GAME");
        return;
    }
    for (i = 0; game[i]; ++i) pattern[i] = game[i];
    for (k = 0; L"\\enshrouded_*.dat"[k]; ++k) pattern[i + k] = L"\\enshrouded_*.dat"[k];
    pattern[i + k] = 0;
    best[0] = 0;
    find = FindFirstFileW(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) {
        say("rewrite", "no container");
        return;
    }
    do {
        if ((found.nFileSizeLow || found.nFileSizeHigh) && lstrcmpiW(found.cFileName, best) > 0)
            lstrcpynW(best, found.cFileName, MAX_PATH);
    } while (FindNextFileW(find, &found));
    FindClose(find);
    if (!best[0]) {
        say("rewrite", "no non-empty container");
        return;
    }
    for (i = 0; game[i]; ++i) path[i] = game[i];
    path[i++] = L'\\';
    for (k = 0; best[k] && i < 1399; ++k) path[i++] = best[k];
    path[i] = 0;
    file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    n = 0;
    for (k = 0; best[k] && n < MAX_PATH; ++k) detail[n++] = (char)best[k];
    detail[n] = 0;
    if (file == INVALID_HANDLE_VALUE) {
        say("rewrite cannot-open", detail);
        return;
    }
    if (!ReadFile(file, bytes, sizeof bytes, &got, NULL) || !got) {
        CloseHandle(file);
        say("rewrite empty", detail);
        return;
    }
    for (k = 0; k < (int)got; ++k) bytes[k] = (unsigned char)~bytes[k];
    SetFilePointer(file, 0, NULL, FILE_BEGIN);
    WriteFile(file, bytes, got, &written, NULL);
    CloseHandle(file);
    say("rewrote", detail);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    static WCHAR module[1024], action[128], baseline[1024];
    DWORD length, i, start = 0, stop, k;
    char note[160];
    int n;
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    length = GetModuleFileNameW(instance, module, 1024);
    for (i = 0; i < length; ++i)
        if (module[i] == L'\\' || module[i] == L'/') start = i + 1;
    stop = length;
    for (i = start; i < length; ++i)
        if (module[i] == L'.') stop = i;
    for (k = 0, i = start; i < stop && k < 63; ++i, ++k) {
        WCHAR c = module[i];
        g_stem[k] = (char)(c >= L'A' && c <= L'Z' ? c - L'A' + L'a' : c);
    }
    g_stem[k] = 0;
    for (k = 0; L"G14D_DLL_"[k]; ++k) g_var[k] = L"G14D_DLL_"[k];
    for (i = 0; g_stem[i] && k < 94; ++i) {
        char c = g_stem[i];
        g_var[k++] = (WCHAR)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c == '-' ? '_' : c);
    }
    g_var[k] = 0;
    length = GetEnvironmentVariableW(g_var, action, 128);
    if (!length || length >= 128) lstrcpyW(action, L"plain");
    length = GetEnvironmentVariableW(L"G14D_BASELINE", baseline, 1024);
    n = 0;
    n = put(note, n, sizeof note, "action=");
    for (k = 0; action[k] && n < 100; ++k) note[n++] = (char)action[k];
    note[n] = 0;
    n = put(note, n, sizeof note,
            length && length < 1024 && GetFileAttributesW(baseline) != INVALID_FILE_ATTRIBUTES ? " baseline=1"
                                                                                                 : " baseline=0");
    note[n] = 0;
    say("attach-begin", note);
    if (starts(action, L"touch")) touch();
    else if (starts(action, L"sleep:")) {
        DWORD ms = 0;
        for (k = 6; action[k] >= L'0' && action[k] <= L'9'; ++k) ms = ms * 10 + (DWORD)(action[k] - L'0');
        Sleep(ms);
    } else if (starts(action, L"hang")) {
        say("hang", NULL);
        Sleep(INFINITE);
    } else if (starts(action, L"rewrite-highest"))
        rewrite_highest();
    say("attach-end", NULL);
    return TRUE;
}
