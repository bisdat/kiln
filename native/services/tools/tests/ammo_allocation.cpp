// SPDX-License-Identifier: MIT
// Fail exactly the next ordinary allocation, after all immutable inputs have been prepared.
#include <cstdlib>
#include <new>
static bool fail_allocation;
void* operator new(std::size_t size) {
    if(fail_allocation){fail_allocation=false;throw std::bad_alloc();}
    if(void* p=std::malloc(size?size:1))return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
#define main allocation_boundary_fixture
#include "presentation_api.cpp"
#undef main
static KilnResult KILN_CALL allocation_state(KilnClientState* out) {
    *out={};out->size=sizeof *out;out->side=KILN_SIDE_CLIENT;out->session=KILN_SESSION_HOST;
    out->generation=7;out->client_session=9;out->local_player=123;return KILN_OK;
}
int main() {
    KilnClientApi local_client{};local_client.state=&allocation_state;
    ammo::client=&local_client;ammo::ready.store(true);ammo::catalog_session=9;ammo::catalog[0]={555};ammo::catalog[1]={555};
    SelectorGroup groups[7]{};
    for(auto& g:groups){g.data=g.inline_rows;g.capacity=64;g.count=1;g.data[0].item=999;g.data[0].count=9;}
    uint32_t held=77;const uint32_t* access[16]{};access[15]=&held;
    auto* context=const_cast<void*>(static_cast<const void*>(access));
    {ammo::Frame frame(groups,context,0);ammo::after(frame,groups,context);}
    auto p=std::make_shared<ammo::Publication>();p->owner=7;p->kind=KUA_KIND_ARROW;p->mode=KUA_MODE_REPLACE;p->session=9;
    p->serial=ammo::observations[0].value.game_serial;p->count=1;p->rows[0]={555,12,0,3};ammo::publications[0]=p;
    {
        ammo::Frame frame(groups,context,0);fail_allocation=true;
        ammo::after(frame,groups,context);
    }
    CHECK(!fail_allocation&&ammo::failed());
    CHECK(groups[0].count==1&&groups[0].data[0].item==999); // No untracked virtual row may be written before allocation.
    CHECK(ammo::bindings.empty());
    ammo::ready.store(false);ammo::faulted.store(false);ammo::client=nullptr;
    std::cout<<"ammo allocation fault: "<<checks<<" checks passed\n";
}
