// SPDX-License-Identifier: MIT
#include "../craft_sources.h"
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
using namespace kitems;
using namespace kitems::stations;
int main() {
    int checks = 0, failed = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; std::printf("FAIL %d %s\n", __LINE__, #x); } } while (0)
    CraftSourceRegistry registry;
    auto before = registry.add(10, 1, 2000, false, false, {7});
    auto after = registry.add(10, 1, 3000, true, true, {8});
    CHECK(before && after);
    CHECK(registry.copy(2).empty());
    CHECK(!registry.remove(11, before));
    auto sources = registry.copy(1);
    CHECK(sources.size() == 2 && sources[0].allows(7) && !sources[0].allows(8));
    CHECK(sources[1].allows(7) && !sources[1].allows(8));
    std::map<std::pair<uint32_t,uint32_t>, uint64_t> inv = {{{1,7},1}, {{1000,7},1}, {{2000,7},2}, {{2500,7},3}, {{3000,7},4}};
    auto have = [&](uint32_t e, uint32_t item) { return inv[{e,item}]; };
    CraftLent lent; lent.by[7] = {2500}; lent.count[7] = 3;
    merge_craft_sources(sources, 1, 0, {1,1000}, {7,8}, [](uint32_t, uint32_t) { return true; }, have, &lent);
    CHECK(lent.by[7] == std::vector<uint32_t>({2000,2500,3000}) && lent.count[7] == 9);
    CHECK(lent.by[8].empty());
    std::vector<std::tuple<uint32_t,uint32_t,uint32_t>> log;
    auto pay = [&](uint32_t e,uint32_t item,uint32_t* left) { auto n = std::min<uint64_t>(*left, inv[{e,item}]); inv[{e,item}] -= n; *left -= static_cast<uint32_t>(n); if(n) log.emplace_back(e,item,static_cast<uint32_t>(n)); return true; };
    std::vector<CraftCost> lines = {{7,10,0,0}}; uint64_t from; uint32_t why;
    CHECK(craft_pay(&lines,{1,1000},lent,have,pay,&from,&why) == kCraftPaid);
    CHECK((log == std::vector<std::tuple<uint32_t,uint32_t,uint32_t>>({{1,7,1},{1000,7,1},{2000,7,2},{2500,7,3},{3000,7,3}})));
    log.clear(); lines = {{7,1,0,0},{8,1,0,0}};
    CHECK(craft_pay(&lines,{1},lent,have,pay,&from,&why) == kCraftGame && log.empty());
    CraftLent refused;
    merge_craft_sources(sources,1,0,{1},{7},[](uint32_t,uint32_t) {return false;},have,&refused);
    CHECK(refused.by[7].empty());
    CraftLent other;
    merge_craft_sources(sources,2,0,{2},{7},[](uint32_t,uint32_t) {return true;},have,&other);
    CHECK(other.by[7].empty());
    CHECK(registry.remove(10,before));
    CHECK(!registry.remove(10,before));
    CraftLent disposed;
    merge_craft_sources(registry.copy(1),1,0,{1},{7},[](uint32_t,uint32_t) {return true;},have,&disposed);
    CHECK(disposed.by[7] == std::vector<uint32_t>({3000}));
    registry.cancel_owner(10); CHECK(registry.copy(1).empty());
    // A new login of the same player frees that player's dead registrations; other players' stay.
    CraftSourceRegistry logins;
    CHECK(logins.add(10, 1, 2000, false, true, {}, 5) && logins.add(10, 1, 2001, false, true, {}, 5) && logins.add(10, 2, 2002, false, true, {}, 9));
    const auto relog = logins.add(10, 1, 2003, false, true, {}, 6);
    CHECK(relog && logins.copy(1).size() == 1 && logins.copy(1)[0].container == 2003 && logins.copy(1)[0].player_session == 6);
    CHECK(logins.copy(2).size() == 1);
    CraftSourceRegistry full;
    for (uint32_t i = 0; i < 4096; ++i) full.add(10, 1, 2000 + i, false, true, {}, 5);
    CHECK(!full.add(10, 2, 9000, false, true, {}, 1));  // still capped for live registrations
    CHECK(full.add(10, 1, 9001, false, true, {}, 6));   // a re-login re-registration is never starved by dead links
    CHECK(full.copy(1).size() == 1 && full.add(10, 2, 9000, false, true, {}, 1));
    std::vector<uint32_t> order;
    CHECK(craft_order(0,1,nullptr,0,&order) && order == std::vector<uint32_t>({1}));
    CHECK(source_identity_ok(1,2000,false,true));
    CHECK(!source_identity_ok(1,2,true,true));
    CHECK(!source_identity_ok(1,2000,false,false));
    CHECK(!source_identity_ok(1024,2000,false,true));
    // ---- GitHub #147: a lock's required item from the player's opted-in links ----
    {
        CraftSourceRegistry locks;
        // registration order: 2100 (crafting only), 2200 (locks, after outputs), 2300 (locks), 2400 (locks, allow 9 only),
        // 2300 again (locks, a second link of one container), 2500 (another player's, locks)
        CHECK(locks.add(10, 1, 2100, false, true, {}, 0, false) && locks.add(10, 1, 2200, true, true, {}, 0, true) &&
              locks.add(10, 1, 2300, false, true, {}, 0, true) && locks.add(10, 1, 2400, false, false, {9}, 0, true) &&
              locks.add(10, 1, 2300, false, true, {}, 0, true) && locks.add(10, 2, 2500, false, true, {}, 0, true));
        CHECK(locks.any_locks() && locks.players() == std::vector<uint32_t>({1, 2}));
        std::map<std::pair<uint32_t,uint32_t>, uint64_t> held = {{{1,7},5}, {{2100,7},9}, {{2200,7},2}, {{2300,7},1}, {{2400,7},4},
                                                                {{2400,9},6}, {{2500,7},8}};
        auto count = [&](uint32_t e, uint32_t item) { return held[{e,item}]; };
        auto all = [](uint32_t, uint32_t) { return true; };
        const auto src = locks.copy(1);
        // opt-in respected (2100 is crafting only), the filter respected (2400 allows 9 only), BeforeStationOutputs links
        // first in registration order, then AfterStationOutputs, one container once, the bag never a source
        auto seven = lock_sources_for(src, 1, 7, 1, all, count);
        CHECK(seven.size() == 2 && seven[0].container == 2300 && seven[0].count == 1 && seven[1].container == 2200 && seven[1].count == 2);
        auto nine = lock_sources_for(src, 1, 9, 1, all, count);
        CHECK(nine.size() == 1 && nine[0].container == 2400 && nine[0].count == 6);
        // the payment's permission and container test refuses a link: it is not counted
        CHECK(lock_sources_for(src, 1, 7, 1, [](uint32_t, uint32_t c) { return c != 2300; }, count).size() == 1);
        // another player's links never count; item 0 names nothing
        CHECK(lock_sources_for(src, 2, 7, 2, all, count).empty() && lock_sources_for(locks.copy(2), 2, 7, 2, all, count).size() == 1);
        CHECK(lock_sources_for(src, 1, 0, 1, all, count).empty());
        // all or nothing: 3 from {2300: 1, 2200: 2} in that order; 4 is more than they hold, so no take at all
        std::vector<LockHeld> takes;
        CHECK(plan_lock_take(seven, 3, &takes) && takes.size() == 2 && takes[0].container == 2300 && takes[0].count == 1 &&
              takes[1].container == 2200 && takes[1].count == 2);
        CHECK(plan_lock_take(seven, 1, &takes) && takes.size() == 1 && takes[0].container == 2300);
        CHECK(!plan_lock_take(seven, 4, &takes) && takes.empty());
        // the count a lock check sees: the bag's when it covers the lock (no link asked), else bag + links
        CHECK(lock_counted(5, 3, 1) == 5 && lock_counted(0, 3, 1) == 3 && lock_counted(1, 2, 2) == 3 && lock_counted(0, 0, 1) == 0);
        CHECK(lock_counted(2, 0xffffffffull, 3) == 0xffffffffu);
        // why the links cannot pay, for the log line
        CHECK(lock_why(src, 1, 7, 3, 3) == kLockNone && lock_why(src, 1, 7, 3, 4) == kLockShort);
        CHECK(lock_why(src, 3, 7, 0, 1) == kLockNoLinks);
        CraftSourceRegistry crafting_only;
        crafting_only.add(10, 1, 2100, false, true, {}, 0, false);
        CHECK(lock_why(crafting_only.copy(1), 1, 7, 0, 1) == kLockNotOptedIn);
        CraftSourceRegistry allow9;
        allow9.add(10, 1, 2400, false, false, {9}, 0, true);
        CHECK(lock_why(allow9.copy(1), 1, 7, 0, 1) == kLockFiltered && lock_why(allow9.copy(1), 1, 9, 6, 1) == kLockNone);
        CHECK(std::string(lock_why_text(kLockNotOptedIn)).find("not opted in") != std::string::npos);
        // an older caller's registration (no Locks) never pays a lock
        CraftSourceRegistry old;
        old.add(10, 1, 2300, false, true, {});
        CHECK(!old.any_locks() && lock_sources_for(old.copy(1), 1, 7, 1, all, count).empty());
    }
    std::printf("craft sources: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
