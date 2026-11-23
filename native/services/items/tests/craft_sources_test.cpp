// SPDX-License-Identifier: MIT
#include "../craft_sources.h"
#include <cstdio>
#include <map>
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
    std::vector<uint32_t> order;
    CHECK(craft_order(0,1,nullptr,0,&order) && order == std::vector<uint32_t>({1}));
    CHECK(source_identity_ok(1,2000,false,true));
    CHECK(!source_identity_ok(1,2,true,true));
    CHECK(!source_identity_ok(1,2000,false,false));
    CHECK(!source_identity_ok(1024,2000,false,true));
    std::printf("craft sources: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
