// SPDX-License-Identifier: MIT
#include "../merge.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
using namespace kwt;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
template<class T> static void put(Bytes& b, size_t at, T value) { std::memcpy(b.data()+at,&value,sizeof value); }
int main() {
    ModFile file; file.mod="tool-test"; file.clients="mod";
    read_blocks(parse_relaxed(R"({"toolMaterialPools":{"rubble":{"name":"Rubble","entries":[{"item":"RubbleItem","terrainMaterial":8,"voxelsPerItem":16}]}}})"),&file);
    CHECK(file.errors.empty());
    CHECK(file.pools.size()==1);
    Layouts l; l.reg_terrain=0; l.reg_building=8; l.terrain={184,0}; l.building={180,0};
    Bytes registry(16+184+180,0); put<int32_t>(registry,0,16); put<uint32_t>(registry,4,1); registry[16]=8;
    put<int32_t>(registry,8,192); put<uint32_t>(registry,12,1);
    RegistryInfo info; std::string why;
    CHECK(read_registry(registry,l,&info,&why));
    const auto game=pool_game_materials({{"RubbleItem",100,8}},registry,info,l,true);
    auto resolved=resolve_pools(file.pools,{},game);
    CHECK(resolved.errors.empty() && resolved.pools.size()==1);
    CHECK(resolved.pools[0].items==std::vector<uint32_t>{100});
    auto too_many=file.pools[0]; too_many.entries.resize(65,too_many.entries[0]);
    CHECK(resolve_pools({too_many},{},game).arguments_invalid);
    auto duplicate=file.pools[0]; duplicate.entries.push_back(duplicate.entries[0]);
    CHECK(resolve_pools({duplicate},{},game).arguments_invalid);
    ModFile wrong; wrong.mod=file.mod; wrong.clients="mod";
    read_blocks(parse_relaxed(R"({"toolMaterialPools":{"bad":{"name":"Bad","entries":[{"item":"RubbleItem","terrainMaterial":128,"voxelsPerItem":16}]}}})"),&wrong);
    CHECK(!wrong.errors.empty());
    ModFile building; building.mod=file.mod; building.clients="mod";
    read_blocks(parse_relaxed(R"({"buildingMaterialPools":{"wall":{"name":"Wall","materials":[{"material":"terrain"}]}}})"),&building);
    Material terrain; terrain.mod=file.mod; terrain.block="terrain"; terrain.terrain=true; terrain.item=true; terrain.item_id=100; terrain.guid=derive_guid(file.mod,"terrain","terrain");
    CHECK(!resolve_pools(building.pools,{terrain},game).errors.empty()); // Existing building pool still refuses terrain.
    ModFile brush; brush.mod=file.mod; brush.clients="mod";
    read_blocks(parse_relaxed(R"({"toolBrushShapes":{"small":{"item":"c52ceb9f-b1b0-4b4b-a801-52bf8340c002","shape":"sphere","radius":0.5}}})"),&brush);
    CHECK(brush.errors.empty() && brush.shapes.size()==1);
    CHECK(brush.shapes[0].item_id==3155237504u);
    Bytes empty(8,0), merged; std::vector<std::string> refused;
    l.blueprints=0;l.bp_stride=28;l.bp_item=0;l.bp_size=4;l.bp_data=16;l.bp_compressed=24;
    CHECK(merge_shapes(empty,l,brush.shapes,&merged,&refused,&why));
    ArrayView rows; CHECK(array_at(merged,0,28,&rows) && rows.count==1);
    CHECK(rows.base[24]==0); // Terrain brush uses fill bytes, not compressed building bits.
    ArrayView fills; CHECK(array_at(merged,uint32_t(rows.base-merged.data()+16),1,&fills));
    CHECK(fills.count==8);
    std::cout<<"tool registry: "<<checks<<" checks passed\n";
}
