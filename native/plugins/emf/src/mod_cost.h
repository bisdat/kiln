// SPDX-License-Identifier: MIT
// What an EMF mod costs the server world per step, told in kiln.log and emf.json (0.2.0-dev.30.7). tick-stats.jsonl
// names plugin:kiln-emf.dll as one subscriber; emf.json's tickMsPerStep is an average since the start. Once a minute
// kiln-emf compares each loaded mod's totals with the minute before: the time in its callbacks per server step, how many
// for_each_entity scans and get_component reads it made, and kiln-emf's own share (the entity scans). A mod over 2 ms
// per step gets a line (a warning from 4 ms: a quarter of a 60 steps/s step), again every 10 minutes while it stays
// over, so a player and the mod's author can see which mod holds the server's steps up and whether it is the mod's own
// work. Portable (tests/mod_cost_test.cpp).
#pragma once
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace kemf {

struct CostSample {
    uint64_t steps = 0;          // server steps so far
    uint64_t tick_ns = 0;        // time in the mod's after-step callbacks (all worlds)
    uint64_t reads = 0;          // get_component / get_template_component calls
    uint64_t scans = 0;          // for_each_entity calls
    uint64_t scan_entities = 0;  // ids they gave
    uint64_t scan_ns = 0;        // kiln-emf's time reading the entity map for them (not the mod's callbacks)
};

struct CostWindow {
    uint64_t steps = 0;
    double ms_per_step = 0, reads_per_step = 0, scans_per_step = 0, entities_per_scan = 0, scan_ms_per_step = 0;
};

inline CostWindow cost_window(const CostSample& a, const CostSample& b) {
    CostWindow w;
    if (b.steps <= a.steps) return w;
    w.steps = b.steps - a.steps;
    const double s = static_cast<double>(w.steps);
    w.ms_per_step = static_cast<double>(b.tick_ns - a.tick_ns) / 1e6 / s;
    w.reads_per_step = static_cast<double>(b.reads - a.reads) / s;
    w.scans_per_step = static_cast<double>(b.scans - a.scans) / s;
    w.scan_ms_per_step = static_cast<double>(b.scan_ns - a.scan_ns) / 1e6 / s;
    if (b.scans > a.scans) w.entities_per_scan = static_cast<double>(b.scan_entities - a.scan_entities) / static_cast<double>(b.scans - a.scans);
    return w;
}

struct CostLine {
    bool warn = false;
    std::string text;
};

// One per mod, on the server world's thread.
class CostReporter {
public:
    static constexpr double kInfoMs = 2.0, kWarnMs = 4.0;
    static constexpr uint64_t kRepeatMs = 600000;

    // Called about once a minute with the mod's totals; the line to log, if any.
    std::optional<CostLine> minute(std::string_view id, const CostSample& now, uint64_t now_ms) {
        if (!have_) {
            prev_ = now;
            have_ = true;
            return std::nullopt;
        }
        last_ = cost_window(prev_, now);
        prev_ = now;
        if (last_.steps < 60 || last_.ms_per_step < kInfoMs) return std::nullopt;  // a paused world says nothing
        if (told_ && now_ms - told_ms_ < kRepeatMs) return std::nullopt;
        told_ = true;
        told_ms_ = now_ms;
        CostLine line;
        line.warn = last_.ms_per_step >= kWarnMs;
        char buf[768];
        int n = std::snprintf(buf, sizeof buf,
                              "emf mod %.*s costs %.1f ms per server step (the last %llu steps; at 60 steps/s a step has "
                              "16.7 ms for the whole game)",
                              static_cast<int>(id.size()), id.data(), last_.ms_per_step,
                              static_cast<unsigned long long>(last_.steps));
        std::string text(buf, n > 0 ? static_cast<size_t>(n) : 0);
        if (last_.scans_per_step > 0) {
            n = std::snprintf(buf, sizeof buf,
                              "; it lists every entity %.1f time(s) per step (for_each_entity, %.0f entities each; "
                              "kiln-emf's share %.2f ms)",
                              last_.scans_per_step, last_.entities_per_scan, last_.scan_ms_per_step);
            text.append(buf, n > 0 ? static_cast<size_t>(n) : 0);
        }
        if (last_.reads_per_step >= 1) {
            n = std::snprintf(buf, sizeof buf, "; it reads %.0f components per step (get_component)", last_.reads_per_step);
            text.append(buf, n > 0 ? static_cast<size_t>(n) : 0);
        }
        const double own = last_.ms_per_step - last_.scan_ms_per_step;
        n = std::snprintf(buf, sizeof buf, ". %.1f ms of it is the mod's own work and its reads in the game", own > 0 ? own : 0.0);
        text.append(buf, n > 0 ? static_cast<size_t>(n) : 0);
        if (id == "lua") text += "; for Lua mods, lua.perf in the game console says which script it is";
        line.text = std::move(text);
        return line;
    }

    const CostWindow& last() const { return last_; }

private:
    CostSample prev_;
    CostWindow last_;
    bool have_ = false, told_ = false;
    uint64_t told_ms_ = 0;
};

}  // namespace kemf
