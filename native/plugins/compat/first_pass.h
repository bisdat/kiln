// SPDX-License-Identifier: MIT
// Process-local named Win32 mapping shared by kiln-core and kiln-shroudtopia. The core creates it before early
// natives; the host publishes its start tick, then completion after the first activation pass. Kept until exit.
#pragma once
#include <windows.h>
#include <string>
#include "first_pass_policy.h"

namespace kiln::first_pass {
class Channel {
    struct Data { LONG state; LONG reserved; LONG64 started; };
    HANDLE mapping_ = nullptr;
    Data* data_ = nullptr;
public:
    Channel() = default;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    ~Channel() {
        if (data_) UnmapViewOfFile(data_);
        if (mapping_) CloseHandle(mapping_);
    }
    bool open(bool create) {
        if (data_) return true;
        const auto name = L"Local\\Kiln.Shroudtopia.FirstPass." + std::to_wstring(GetCurrentProcessId());
        mapping_ = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Data), name.c_str())
                          : OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        if (!mapping_) return false;
        data_ = static_cast<Data*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Data)));
        if (!data_) { CloseHandle(mapping_); mapping_ = nullptr; }
        return data_ != nullptr;
    }
    void begin() {
        if (!data_) return;
        InterlockedExchange64(&data_->started, static_cast<LONG64>(GetTickCount64()));
        InterlockedExchange(&data_->state, static_cast<LONG>(State::Running));
    }
    void done() {
        if (data_) InterlockedCompareExchange(&data_->state, static_cast<LONG>(State::Done), static_cast<LONG>(State::Running));
    }
    void bypass() {
        if (!data_) return;
        InterlockedCompareExchange(&data_->state, static_cast<LONG>(State::Bypass), static_cast<LONG>(State::Expected));
        InterlockedCompareExchange(&data_->state, static_cast<LONG>(State::Bypass), static_cast<LONG>(State::Running));
    }
    void stop() { if (data_) InterlockedExchange(&data_->state, static_cast<LONG>(State::Stopped)); }
    void absent() {
        if (data_) InterlockedCompareExchange(&data_->state, static_cast<LONG>(State::Bypass), static_cast<LONG>(State::Expected));
    }
    State state() const {
        return data_ ? static_cast<State>(InterlockedCompareExchange(&data_->state, 0, 0)) : State::Bypass;
    }
    uint64_t started() const {
        return data_ ? static_cast<uint64_t>(InterlockedCompareExchange64(&data_->started, 0, 0)) : 0;
    }
    Decision decision(uint32_t max_wait_ms) const { return decide(state(), started(), GetTickCount64(), max_wait_ms); }
};
}  // namespace kiln::first_pass
