// SPDX-License-Identifier: MIT
#include "../held_state.h"
#include <cstdlib>
#include <iostream>
using namespace kiln::tools;
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
struct Source final:HeldComponents {
    std::array<uint8_t,216> selection{};std::array<uint8_t,96> inventory{};
    uint32_t inventory_entity=42;bool change=false,missing=false;unsigned selections=0;
    bool snapshot(uint32_t entity,uint32_t hash,void* out,size_t size) override {
        if(missing)return false;
        if(hash==0x34bc3e2f&&entity==1&&size==selection.size()) {if(change&&selections++)selection[0xc0]^=1;std::memcpy(out,selection.data(),size);return true;}
        if(hash==0xb19528a9&&entity==inventory_entity&&size==inventory.size()) {std::memcpy(out,inventory.data(),size);return true;}
        return false;
    }
    void put(std::array<uint8_t,216>& data,size_t at,uint32_t value){std::memcpy(data.data()+at,&value,4);}
    void stack(unsigned slot,uint32_t item,uint32_t count){std::memcpy(inventory.data()+12*slot,&item,4);std::memcpy(inventory.data()+12*slot+4,&count,4);}
};
int main(){
    Source s;s.put(s.selection,0xa0,42);s.stack(0,100,3);CHECK(read_held_item(s,1)==100);
    s.stack(0,100,0);CHECK(read_held_item(s,1)==0); // A stale id in an empty inventory slot is not held.
    s.put(s.selection,0xb0,200);CHECK(read_held_item(s,1)==200); // Running action's cached item wins, even without slot bytes.
    s.put(s.selection,0xb0,0);s.selection[0xc0]=8;s.put(s.selection,0xa4,42);s.stack(0,300,1);CHECK(read_held_item(s,1)==300);
    s.selection[0xc0]=15;s.stack(7,400,2);CHECK(read_held_item(s,1)==400);
    s.selection[0xc0]=16;CHECK(read_held_item(s,1)==0);
    s.selection[0xc0]=0;s.change=true;s.selections=0;CHECK(read_held_item(s,1)==0); // Selection changes between component reads.
    s.change=false;s.missing=true;CHECK(read_held_item(s,1)==0);CHECK(read_held_item(s,0)==0);
    std::cout<<"held state: "<<checks<<" checks passed\n";
}
