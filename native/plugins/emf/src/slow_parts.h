// SPDX-License-Identifier: MIT
// Which part of a tick callback took long (0.2.0-dev.29.17). tick-stats.jsonl names the subscriber that held the world
// up (plugin:kiln-emf.dll at 1.9 s), not the part of it: this times each named part of one call, and when the whole
// call took longer than the threshold (100 ms) it gives one line naming every part over 1 ms, at most once per
// `every_ms` (the calls in between are counted and the next line says how many it left out).
//
//   SlowParts parts;                         // one per callback, on its thread
//   parts.begin(now_ns());
//   parts.part("net", now_ns()); ...         // the time since the previous mark goes to "net"
//   if (auto line = parts.end(now_ns(), now_ms())) log("kiln-emf: server pre took " + *line);
//
// Portable (the host test: tests/slow_parts_test.cpp).
#pragma once
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace kemf {

class SlowParts {
public:
    explicit SlowParts(double threshold_ms = 100.0, uint64_t every_ms = 60000) : threshold_ms_(threshold_ms), every_ms_(every_ms) {}

    void begin(uint64_t now_ns) {
        start_ = last_ = now_ns;
        parts_.clear();
    }
    // Ends the part `name` at now_ns (it began at the previous mark).
    void part(const char* name, uint64_t now_ns) {
        parts_.push_back({name, ms(now_ns - last_)});
        last_ = now_ns;
    }
    // The line to log, or nothing (fast enough, or a line was given less than every_ms ago).
    std::optional<std::string> end(uint64_t now_ns, uint64_t now_ms) {
        const double total = ms(now_ns - start_);
        if (total < threshold_ms_) return std::nullopt;
        if (told_ && now_ms - last_told_ < every_ms_) {
            ++skipped_;
            return std::nullopt;
        }
        told_ = true;
        last_told_ = now_ms;
        std::string s = fmt("%.0f ms", total);
        std::string list;
        for (const auto& p : parts_) {
            if (p.ms < 1.0) continue;
            list += (list.empty() ? "" : ", ") + std::string(p.name) + " " + fmt("%.0f ms", p.ms);
        }
        const double rest = total - sum();
        if (rest >= 1.0) list += (list.empty() ? "" : ", ") + std::string("other ") + fmt("%.0f ms", rest);
        s += " (" + (list.empty() ? std::string("no single part over 1 ms") : list) + ")";
        if (skipped_) s += fmt("; %llu more slow call(s) since the last line", static_cast<unsigned long long>(skipped_));
        skipped_ = 0;
        return s;
    }

private:
    struct Part {
        const char* name;
        double ms;
    };
    static double ms(uint64_t ns) { return static_cast<double>(ns) / 1e6; }
    double sum() const {
        double t = 0;
        for (const auto& p : parts_) t += p.ms;
        return t;
    }
    static std::string fmt(const char* f, double v) {
        char b[64];
        std::snprintf(b, sizeof b, f, v);
        return b;
    }
    static std::string fmt(const char* f, unsigned long long v) {
        char b[96];
        std::snprintf(b, sizeof b, f, v);
        return b;
    }

    double threshold_ms_;
    uint64_t every_ms_;
    uint64_t start_ = 0, last_ = 0, last_told_ = 0, skipped_ = 0;
    bool told_ = false;
    std::vector<Part> parts_;
};

}  // namespace kemf
