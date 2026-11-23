// SPDX-License-Identifier: MIT
#define main source_boundary_fixture
#include "presentation_api.cpp"
#undef main
struct SourceSetup {uint32_t ids[16]{};uint8_t cats[16]{},flags{},total{},available{},initialized{1};};
static std::map<std::pair<uint32_t,uint32_t>,std::vector<uint8_t>> components;
static bool fail_read,change_setup;
template<class T> static void set_component(uint32_t entity,uint32_t hash,const T& value) {
    const auto* p=reinterpret_cast<const uint8_t*>(&value);components[{entity,hash}]={p,p+sizeof value};
}
static uint64_t KILN_CALL source_session(uint32_t kind){return kind==KILN_WORLD_CLIENT?9:0;}
static KilnResult KILN_CALL source_open(KilnOwner,uint64_t,uint32_t,uint64_t,KilnEntity entity,KilnObjectRef* out){out->entity=entity;return KILN_OK;}
static KilnResult KILN_CALL source_snapshot(KilnOwner,const KilnObjectRef* ref,uint32_t hash,void* out,uint32_t capacity,uint32_t* required) {
    const auto i=components.find({ref->entity,hash});if(fail_read||i==components.end())return KILN_ERR_NOT_FOUND;
    *required=uint32_t(i->second.size());if(capacity<*required)return KILN_ERR_BUFFER_TOO_SMALL;
    std::memcpy(out,i->second.data(),*required);
    if(change_setup&&hash==0xc24df82au)components[{ref->entity,hash}][80]^=1;
    return KILN_OK;
}
int main() {
    KilnWorldApi world{};world.game_world_session=&source_session;world.entity_open=&source_open;world.ecs_snapshot=&source_snapshot;
    ammo::world_api=&world;child=88;ammo::backpack_probe=nullptr;
    SourceSetup parent;parent.ids[0]=2;parent.cats[0]=5;
    SourceSetup nested;nested.ids[0]=3;nested.cats[0]=4;nested.ids[1]=4;nested.cats[1]=2;
    uint32_t stacks[24]{};stacks[0]=555;stacks[1]=8;
    set_component(1,0xc24df82au,parent);set_component(2,0xc24df82au,nested);set_component(3,0xb19528a9u,stacks);
    CHECK(ammo::backpack(1,555)==1);CHECK(ammo::backpack(1,999)==0);
    stacks[1]=0;set_component(3,0xb19528a9u,stacks);CHECK(ammo::backpack(1,555)==0); // Empty stack is not equip permission.
    stacks[1]=8;set_component(4,0xb19528a9u,stacks);CHECK(ammo::backpack(1,555)==0); // Equipment inventory is not backpack.
    fail_read=true;CHECK(ammo::backpack(1,555)==-1);fail_read=false;
    change_setup=true;CHECK(ammo::backpack(1,555)==-1);change_setup=false;
    nested.ids[1]=1;nested.cats[1]=5;set_component(2,0xc24df82au,nested);CHECK(ammo::backpack(1,555)==-1); // Cycle fails closed.
    std::array<uint8_t,0x90> registry{};std::array<std::array<uint8_t,0xce>,4> infos{};
    uint32_t keys[4]={10,20,30,40};const uint8_t* values[4]{};uint64_t occupied=15,capacity=4;uint32_t count=4;
    for(unsigned i=0;i<4;++i){std::memcpy(infos[i].data(),&keys[i],4);values[i]=infos[i].data();infos[i][0xcc]=8;infos[i][0xcd]=uint8_t(i+1);}
    auto put=[&](size_t offset,const auto& value){std::memcpy(registry.data()+0x38+offset,&value,sizeof value);};
    const uint64_t* bits=&occupied;put(8,bits);put(0x10,capacity);const uint32_t* k=keys;put(0x28,k);const uint8_t* const* v=values;put(0x40,v);put(0x54,count);
    const uint8_t* access[16]{};access[10]=registry.data();ammo::catalog_session=0;
    CHECK(ammo::copy_catalog(const_cast<void*>(static_cast<const void*>(access)),9));
    CHECK(ammo::catalog[0].contains(10)&&ammo::catalog[0].size()==1);CHECK(ammo::catalog[1].contains(20)&&ammo::catalog[1].size()==1);
    CHECK(!ammo::catalog[0].contains(30)&&!ammo::catalog[1].contains(40));
    capacity=3;put(0x10,capacity);ammo::catalog_session=0;CHECK(!ammo::copy_catalog(const_cast<void*>(static_cast<const void*>(access)),10));
    capacity=4;put(0x10,capacity);keys[0]=99;CHECK(!ammo::copy_catalog(const_cast<void*>(static_cast<const void*>(access)),10));
#ifdef _WIN32
    access[10]=reinterpret_cast<const uint8_t*>(1);CHECK(!ammo::copy_catalog(const_cast<void*>(static_cast<const void*>(access)),10));
#endif
    child=0;ammo::world_api=nullptr;
    std::cout<<"ammo backpack/catalog sources: "<<checks<<" checks passed\n";
}
