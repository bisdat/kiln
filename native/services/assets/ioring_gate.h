// SPDX-License-Identifier: MIT
// kiln.assets: the game's IoRing lookups, refused while Kiln serves its container (report KR-KZRX, 2026-11-28).
//
// The game's async IO (GameFramework "AsyncIo", client 26430625 0x140f3f700) takes the Windows 11 I/O ring when kernelbase
// has it: it asks GetProcAddress (through the exe's KERNEL32 import, 0x140f4c780) for QueryIoRingCapabilities, then
// CreateIoRing, BuildIoRingReadFile, SubmitIoRing, PopIoRingCompletion, ... and reads every container through the ring
// ("Using IoRing for async io...", "[os] Using IO Ring API for async io queue"). Kiln answers its container only at the
// game's ReadFile and CreateIoCompletionPort imports (hooks.cpp), so on the ring the game read the empty
// kiln/data/assets/kiln-content.dat itself: ERROR_HANDLE_EOF for every blob Kiln serves, and a material texture's mip tail
// that fails is the game's fatal "error loading streamed texture mip tail: eof reached" in the main menu. Without an
// answer for QueryIoRingCapabilities the game logs "[os] Could not find IORing Api function ... - IORing not available"
// and uses its fallback queue (ReadFile + IOCP), as it does with --disable-ioring, on Windows 10 and under Wine: the
// queue Kiln serves.
#ifndef KILN_ASSETS_IORING_GATE_H
#define KILN_ASSETS_IORING_GATE_H
#include <cstdint>
#include <cstring>

namespace ka {

// Whether a GetProcAddress name is one of the I/O ring API's (ioringapi.h: QueryIoRingCapabilities, CreateIoRing,
// IsIoRingOpSupported, GetIoRingInfo, SubmitIoRing, CloseIoRing, PopIoRingCompletion, SetIoRingCompletionEvent,
// BuildIoRing*). An ordinal (a "name" up to 0xFFFF, IS_INTRESOURCE) and a null name are never refused.
inline bool ioring_api_name(const char* name) noexcept {
    if (reinterpret_cast<uintptr_t>(name) <= 0xFFFF) return false;
    return std::strstr(name, "IoRing") != nullptr;
}

}  // namespace ka
#endif
