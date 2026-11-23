// SPDX-License-Identifier: MIT
#include "../ammo_list.h"
#include <cstdlib>
#include <iostream>
using namespace kiln::tools;
static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
int main() {
    SelectorRow game[3]{};for(unsigned i=0;i<3;++i){game[i].item=10+i;game[i].count=i+1;game[i].source[0]=42;}
    KilnUiAmmoRow rows[]={{12,90,0,4},{30,8,0,5},{10,77,0,6}};
    const auto replace=compose_ammo(game,3,rows,3,KUA_MODE_REPLACE,30);
    CHECK(replace.count==3);CHECK(replace.rows[0].item==12&&replace.rows[0].count==90);
    CHECK(replace.rows[0].source[0]==42);CHECK(replace.rows[1].source[0]==0);
    CHECK(replace.selected==1&&replace.has_selected);
    const auto append=compose_ammo(game,3,rows,3,KUA_MODE_APPEND,12);
    CHECK(append.count==4);CHECK(append.rows[0].item==10&&append.rows[2].item==12&&append.rows[3].item==30);
    CHECK(append.selected==2&&append.has_selected);CHECK(append.rows[2].count==3);
    const auto gone=compose_ammo(game,3,rows,3,KUA_MODE_REPLACE,999);CHECK(!gone.has_selected);
    const auto empty=compose_ammo(game,3,nullptr,0,KUA_MODE_REPLACE,10);CHECK(empty.count==0&&!empty.has_selected);
    KilnUiAmmoRow many[64]{};for(unsigned i=0;i<64;++i)many[i]={100+i,1,0,i};
    const auto cap=compose_ammo(game,3,many,64,KUA_MODE_APPEND,163);CHECK(cap.count==64&&cap.truncated&&!cap.has_selected);
    const auto vanilla=compose_ammo(game,3,rows,3,KUA_MODE_GAME,11);CHECK(vanilla.count==3&&vanilla.rows[1].count==2&&vanilla.selected==1);
    std::cout<<"ammo composition: "<<checks<<" checks passed\n";
}
