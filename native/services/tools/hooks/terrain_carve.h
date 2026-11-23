// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "kiln_api.h"

namespace kiln::tools::carve {
// Both qualified executables construct this worker-local descriptor. All fields except the borrowed coefficient
// span are passed through byte-for-byte; the report pointer still names the game's own mining/undo accumulator.
constexpr size_t brush_size = 0x90;
constexpr size_t coefficient_offset = 0x70;
constexpr size_t coefficient_count_offset = 0x78;
// The kernel's removal report: removed volume per material (data +0) and in total (float at +0x18). The kernel adds to
// it for every cell it carves, so a stroke that grew it removed terrain (docs/re/terrain-tool-carve.md).
constexpr size_t report_offset = 0x80;
constexpr size_t report_total_offset = 0x18;
struct alignas(16) BrushCopy { std::array<uint8_t, brush_size> bytes; };
using CopyMemory = bool (*)(void*, const void*, size_t);

// removable (optional): whether an allowed cell kept a positive coefficient, the only kind the kernel carves.
inline bool filter_brush(const void* original, const uint8_t (&remove_cells)[32], BrushCopy& brush,
                         std::array<float, 256>& coefficients, CopyMemory copy, bool* removable = nullptr) {
    if (removable) *removable = false;
    if (!original || !copy || !copy(brush.bytes.data(), original, brush_size)) return false;
    const float* source = nullptr;
    uint64_t count = 0;
    std::memcpy(&source, brush.bytes.data() + coefficient_offset, sizeof source);
    std::memcpy(&count, brush.bytes.data() + coefficient_count_offset, sizeof count);
    if (count > coefficients.size() || (count && !source)) return false;
    coefficients.fill(0);
    if (count && !copy(coefficients.data(), source, static_cast<size_t>(count) * sizeof(float))) return false;
    for (unsigned cell = 0; cell < coefficients.size(); ++cell) {
        if (cell == 0 || cell >= 128 || !(remove_cells[cell / 8] & (1u << (cell % 8)))) coefficients[cell] = 0;
        else if (removable && coefficients[cell] > 0) *removable = true;
    }
    const float* owned = coefficients.data();
    const uint64_t owned_count = coefficients.size();
    std::memcpy(brush.bytes.data() + coefficient_offset, &owned, sizeof owned);
    std::memcpy(brush.bytes.data() + coefficient_count_offset, &owned_count, sizeof owned_count);
    return true;
}
// The report's total before or after the kernel; false when the descriptor names no report or it cannot be read.
inline bool report_total(const BrushCopy& brush, CopyMemory copy, float& total) {
    uintptr_t report = 0;
    std::memcpy(&report, brush.bytes.data() + report_offset, sizeof report);
    return report && copy && copy(&total, reinterpret_cast<const void*>(report + report_total_offset), sizeof total);
}
} // namespace kiln::tools::carve

// Called by the tools service before its action bindings become reachable. Both hooks must be ready for Remove.
// Serialize lifecycle calls; close admission and drain active tool invocations before reinstalling or uninstalling.
void kiln_tools_carve_install(const KilnApi* api, KilnOwner owner);
void kiln_tools_carve_uninstall();
