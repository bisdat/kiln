// SPDX-License-Identifier: MIT
// The loaded image's identity facts on Windows: the core's exe-file sha256 (KilnBuildInfo), the PE headers of the
// process image, its file name, and the 4-byte header of <exe>.kfc. Adapted from ShroudForge (MIT, Copyright (c) 2026
// bonsaibauer), modules/game-runtime/native/profiles/identify_game_image.cpp:44-67.
#include <windows.h>

#include <string>

#include "engine_state.h"
#include "memory.h"

namespace kiln::engine {

ImageFacts gather_facts(const KilnApi* api) {
    ImageFacts facts;
    KilnBuildInfo build{};
    build.size = sizeof build;
    if (api->build_info && api->build_info(&build) == KILN_OK) {
        facts.side = build.side;
        build.exe_sha256[sizeof build.exe_sha256 - 1] = 0;
        facts.sha256 = build.exe_sha256;
    }
    const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (base && dos->e_magic == IMAGE_DOS_SIGNATURE) {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            facts.headers = true;
            facts.timestamp = nt->FileHeader.TimeDateStamp;
            facts.size_of_image = nt->OptionalHeader.SizeOfImage;
        }
    }
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n >= path.size()) {
        facts.headers = false;
        return facts;
    }
    path.resize(n);
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring file = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, file.c_str(), static_cast<int>(file.size()), nullptr, 0, nullptr, nullptr);
    if (bytes > 0) {
        facts.target.resize(static_cast<size_t>(bytes));
        WideCharToMultiByte(CP_UTF8, 0, file.c_str(), static_cast<int>(file.size()), facts.target.data(), bytes, nullptr, nullptr);
    }
    // ShroudForge: replace_extension(L".kfc") on the process path, then the first four bytes.
    std::wstring kfc = path;
    const size_t dot = kfc.find_last_of(L'.');
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) kfc.resize(dot);
    kfc += L".kfc";
    const HANDLE f = CreateFileW(kfc.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        unsigned char header[4]{};
        DWORD got = 0;
        if (ReadFile(f, header, sizeof header, &got, nullptr)) facts.archive_format = archive_format_of(header, got);
        CloseHandle(f);
    }
    return facts;
}

// The process image the profile was matched against (layout RVAs are relative to it).
std::uintptr_t mem::image_base() { return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)); }

}  // namespace kiln::engine
