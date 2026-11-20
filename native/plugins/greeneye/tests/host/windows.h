// SPDX-License-Identifier: MIT
// Only jumppatch's pattern matcher is run on the host. Its Windows memory APIs are inert here.
#pragma once
#include <cstddef>
#include <cstdint>
#define __declspec(x)
#define __try try
#define __except(x) catch (...)
using DWORD = unsigned long;
struct SYSTEM_INFO { uintptr_t dwAllocationGranularity; void* lpMinimumApplicationAddress; void* lpMaximumApplicationAddress; };
struct MEMORY_BASIC_INFORMATION { void* BaseAddress; size_t RegionSize; unsigned State, Protect; };
constexpr unsigned MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_FREE = 0x10000, PAGE_EXECUTE_READWRITE = 0x40;
inline void GetSystemInfo(SYSTEM_INFO* s) { *s = {}; }
inline size_t VirtualQuery(const void*, MEMORY_BASIC_INFORMATION*, size_t) { return 0; }
inline void* VirtualAlloc(void*, size_t, unsigned, unsigned) { return nullptr; }
inline bool VirtualProtect(void*, size_t, unsigned, DWORD*) { return false; }
inline void* GetCurrentProcess() { return nullptr; }
inline bool FlushInstructionCache(void*, const void*, size_t) { return false; }
