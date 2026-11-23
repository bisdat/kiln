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
    const auto pool_document = parse_relaxed(R"({"toolMaterialPools":{"rubble":{"name":"Rubble","entries":[{"item":"RubbleItem","terrainMaterial":8,"voxelsPerItem":16}]}}})");
    CHECK(pool_document.size()==1 && pool_document.contains("toolMaterialPools"));
    read_blocks(pool_document,&file);
    CHECK(file.errors.empty());
    CHECK(file.pools.size()==1);
    CHECK(file.pools[0].tool && file.pools[0].valid && file.pools[0].block=="rubble");
    Layouts l; l.reg_terrain=0; l.reg_building=8; l.terrain={184,0}; l.building={180,0};
    Bytes registry(16+184+180,0); put<int32_t>(registry,0,16); put<uint32_t>(registry,4,1); registry[16]=8;
    put<int32_t>(registry,8,192); put<uint32_t>(registry,12,1);
    RegistryInfo info; std::string why;
    CHECK(read_registry(registry,l,&info,&why));
    l.pools=0;l.pool_stride=24;l.pool_slot=0;l.pool_id=4;l.pool_materials=8;l.pool_name=16;
    const std::string terrain_name="TerrainMaterials";
    Bytes pools(8+24+4+terrain_name.size()+1,0);
    put<int32_t>(pools,0,8);put<uint32_t>(pools,4,1);
    put<uint32_t>(pools,8,9);put<uint32_t>(pools,12,851579436);
    put<int32_t>(pools,16,16);put<uint32_t>(pools,20,1);put<uint32_t>(pools,32,100);
    put<int32_t>(pools,24,12);put<uint32_t>(pools,28,uint32_t(terrain_name.size()));
    std::memcpy(pools.data()+36,terrain_name.data(),terrain_name.size());
    std::set<uint32_t> vanilla;CHECK(terrain_pool_items(pools,l,&vanilla,&why));
    CHECK(vanilla==std::set<uint32_t>{100});
    const auto game=pool_game_materials({{"RubbleItem",100,8},{"NonVanillaItem",101,8}},registry,info,l,true,vanilla);
    auto resolved=resolve_pools(file.pools,{},game);
    CHECK(resolved.errors.empty() && resolved.pools.size()==1);
    CHECK(resolved.pools[0].items==std::vector<uint32_t>{100});
    Shape wall;wall.mod=file.mod;wall.block="bad-wall";wall.material_pool="rubble";
    std::vector<Shape> walls{wall};std::vector<std::string> shape_errors;
    bind_shape_pools(&walls,file.pools,resolved.pools,&shape_errors);
    CHECK(walls.empty()&&!shape_errors.empty()); // A policy-only pool cannot back an ordinary building blueprint.
    CHECK(shape_errors[0].find("not a game-visible building material pool")!=std::string::npos);
    Bytes untouched;
    CHECK(merge_pools(pools,l,{},resolved.pools,&untouched,&why));
    CHECK(untouched==pools); // Policy registration must not alter vanilla map/rows or enumerate magic stock twice.
    ArrayView original_rows,new_rows;
    CHECK(array_at(pools,0,24,&original_rows)&&array_at(untouched,0,24,&new_rows)&&original_rows.count==new_rows.count);
    auto nonvanilla=file.pools[0];nonvanilla.entries[0].item="NonVanillaItem";
    const auto refused_pool=resolve_pools({nonvanilla},{},game);
    CHECK(refused_pool.arguments_invalid&&refused_pool.pools.empty());
    CHECK(refused_pool.argument_reason.find("not a member of the qualified vanilla TerrainMaterials pool")!=std::string::npos);
    auto forged=resolved.pools;forged[0].items[0]=101;
    const Bytes sentinel{1,2,3};untouched=sentinel;
    CHECK(!merge_pools(pools,l,{},forged,&untouched,&why)&&untouched==sentinel); // Writer rechecks; no partial mutation.
    auto changed=pools;put<uint32_t>(changed,12,123);
    CHECK(!terrain_pool_items(changed,l,&vanilla,&why)&&vanilla.empty());
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
    const auto brush_document = parse_relaxed(R"({"toolBrushShapes":{"small":{"item":"c52ceb9f-b1b0-4b4b-a801-52bf8340c002","shape":"sphere","radius":0.5}}})");
    CHECK(brush_document.size()==1 && brush_document.contains("toolBrushShapes"));
    read_blocks(brush_document,&brush);
    CHECK(brush.errors.empty() && brush.shapes.size()==1);
    CHECK(brush.shapes[0].terrain_brush && brush.shapes[0].block=="small");
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
