// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cmath>
namespace kb {
struct TerrainIoBudget {
    uint64_t step = UINT64_MAX;
    uint32_t reads = 0, writes = 0;
    double milliseconds = 0;
    void begin(uint64_t s) { if (step != s) { step=s; reads=writes=0; milliseconds=0; } }
    bool acquire(uint32_t read, uint32_t write) {
        if (read > 32768 || write > 4096 || reads + read > 32768 || writes + write > 4096 || milliseconds >= 2) return false;
        reads += read; writes += write; return true;
    }
    void charge(double ms) {
        if (!std::isfinite(ms) || ms < 0) milliseconds = 2;
        else milliseconds += ms;
    }
    void reset() { step=UINT64_MAX; reads=writes=0; milliseconds=0; }
};
}
