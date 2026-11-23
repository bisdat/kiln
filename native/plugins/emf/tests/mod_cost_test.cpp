// SPDX-License-Identifier: MIT
// mod_cost.h (the per-mod cost line and emf.json's lastMinute) and ContextScope's per-mod API counts (0.2.0-dev.30.7).
#include <cstdio>
#include <cstdlib>
#include <string>

#include "emf_internal.h"
#include "mod_cost.h"

namespace kemf {
thread_local Context t_ctx;
}

static int g_fail = 0, g_checks = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL %s\n", what);
    }
}
static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

int main() {
    using namespace kemf;
    {  // PepegaJedi's 30.6 report: lua 11.2 ms per server step, one entity listing of 22866 per step, 2 reads per entity
        CostReporter r;
        CostSample s;
        check(!r.minute("lua", s, 0), "the first minute is the baseline");
        s.steps = 3340;
        s.tick_ns = 3340ull * 11200000ull;
        s.scans = 3340;
        s.scan_entities = 3340ull * 22866ull;
        s.scan_ns = 3340ull * 100000ull;
        s.reads = 3340ull * 45732ull;
        auto line = r.minute("lua", s, 60000);
        check(line.has_value(), "11.2 ms per step: a line");
        if (line) {
            std::printf("  line: %s\n", line->text.c_str());
            check(line->warn, "over 4 ms: a warning");
            check(has(line->text, "emf mod lua costs 11.2 ms per server step"), "names the mod and its ms per step");
            check(has(line->text, "1.0 time(s) per step (for_each_entity, 22866 entities each; kiln-emf's share 0.10 ms)"),
                  "the entity listings and kiln-emf's share");
            check(has(line->text, "45732 components per step"), "the component reads");
            check(has(line->text, "11.1 ms of it is the mod's own work"), "the mod's own share");
            check(has(line->text, "lua.perf"), "Lua: points at lua.perf");
        }
        const CostWindow& w = r.last();
        check(w.steps == 3340 && w.ms_per_step > 11.19 && w.ms_per_step < 11.21, "lastMinute ms per step");
        check(w.entities_per_scan > 22865.9 && w.entities_per_scan < 22866.1, "lastMinute entities per scan");
        s.steps += 3340;
        s.tick_ns += 3340ull * 11200000ull;
        check(!r.minute("lua", s, 120000), "still over a minute later: no repeat within 10 minutes");
        s.steps += 3340;
        s.tick_ns += 3340ull * 11200000ull;
        check(r.minute("lua", s, 60000 + 600000).has_value(), "10 minutes later: told again");
    }
    {
        CostReporter r;
        CostSample s;
        r.minute("raids", s, 0);
        s.steps = 3600;
        s.tick_ns = 3600ull * 1000000ull;
        check(!r.minute("raids", s, 60000), "1 ms per step: no line");
        s.steps += 3600;
        s.tick_ns += 3600ull * 2500000ull;
        auto line = r.minute("raids", s, 120000);
        check(line && !line->warn, "2.5 ms per step: an info line, not a warning");
        check(line && !has(line->text, "for_each_entity") && !has(line->text, "lua.perf"), "no scans, not Lua: neither named");
        s.tick_ns += 3600ull * 9000000ull;  // a paused world: steps do not move
        check(!r.minute("raids", s, 900000), "no steps in the minute: no line");
    }
    {  // ContextScope: each scope's calls go to its own mod, the outer scope keeps its own count
        Mod outer, inner;
        {
            ContextScope a(&outer, nullptr, true);
            t_ctx.api.reads += 5;
            {
                ContextScope b(&inner, nullptr, true);
                t_ctx.api.reads += 7;
                t_ctx.api.scans += 1;
                t_ctx.api.scan_entities += 100;
                t_ctx.api.scan_ns += 42;
            }
            check(t_ctx.api.reads == 5, "the outer count is back after the inner scope");
            t_ctx.api.reads += 1;
        }
        check(outer.api_reads == 6 && outer.api_scans == 0, "outer: its own 6 reads");
        check(inner.api_reads == 7 && inner.api_scans == 1 && inner.api_scan_entities == 100 && inner.api_scan_ns == 42,
              "inner: its own reads and scan");
        check(t_ctx.mod == nullptr && t_ctx.api.reads == 0, "outside every scope: nothing left over");
    }
    std::printf("MOD COST %s (%d checks)\n", g_fail ? "FAIL" : "PASS", g_checks);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
