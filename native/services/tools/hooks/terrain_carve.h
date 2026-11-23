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
struct alignas(16) BrushCopy { std::array<uint8_t, brush_size> bytes; };
// The kernel's removal report: the descriptor names it at +report, and the kernel adds every cell it carves to the
// float total at report +total, so a stroke that grew the total removed terrain (docs/re/terrain-tool-carve.md). Both
// offsets come only from sigdb, captured from the kernel's own accumulation on each build (tools/carve.report and
// tools/carve.report_total, @tools/TerrainCarveKernel.report); -1 while they are unresolved.
struct ReportLayout { int64_t report = -1, total = -1; };
// A pointer inside the descriptor copy that is not the coefficient table Kiln rewrites, and an aligned float.
inline bool report_layout_ok(const ReportLayout& layout) {
    return layout.report >= 0 && !(layout.report & 7) && uint64_t(layout.report) + sizeof(uintptr_t) <= brush_size &&
           uint64_t(layout.report) != coefficient_offset && uint64_t(layout.report) != coefficient_count_offset &&
           layout.total >= 0 && !(layout.total & 3) && layout.total <= 0x1000;
}
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
// The report's total before or after the kernel; false without a qualified layout, when the descriptor names no report,
// or when it cannot be read (copy is the guarded read).
inline bool report_total(const BrushCopy& brush, const ReportLayout& layout, CopyMemory copy, float& total) {
    if (!copy || !report_layout_ok(layout)) return false;
    uintptr_t report = 0;
    std::memcpy(&report, brush.bytes.data() + layout.report, sizeof report);
    float value = 0;
    if (!report || !copy(&value, reinterpret_cast<const void*>(report + uintptr_t(layout.total)), sizeof value)) return false;
    total = value;
    return true;
}
} // namespace kiln::tools::carve

// Called by the tools service before its action bindings become reachable. Both hooks must be ready for Remove.
// Serialize lifecycle calls; close admission and drain active tool invocations before reinstalling or uninstalling.
void kiln_tools_carve_install(const KilnApi* api, KilnOwner owner);
void kiln_tools_carve_uninstall();
