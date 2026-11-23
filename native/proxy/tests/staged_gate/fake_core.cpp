// SPDX-License-Identifier: MIT
// A stand-in for kiln-core.dll (test-staged-gate.sh): kiln_boot appends CORE_TAG to <kiln>\core-ran.txt, so the test
// sees which core the proxy loaded, and returns. It never confirms a staged Kiln update (Kiln.Host does that).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define KILN_STR2(x) #x
#define KILN_STR(x) KILN_STR2(x)
extern "C" __declspec(dllexport) int __cdecl kiln_boot(const void*) {
    wchar_t path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&kiln_boot), &self);
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    while (n > 0 && path[n - 1] != L'\\') --n;
    path[n] = 0;
    lstrcatW(path, L"core-ran.txt");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const char tag[] = KILN_STR(CORE_TAG) "\r\n";
    DWORD written = 0;
    WriteFile(h, tag, sizeof tag - 1, &written, nullptr);
    CloseHandle(h);
    return 0;
}
