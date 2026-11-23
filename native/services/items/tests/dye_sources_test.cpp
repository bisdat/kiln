// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "../dye_sources_logic.h"
using namespace kitems::dyes;
int failed=0, checked=0;
void check(bool yes,const char* name){++checked;std::printf("%s %s\n",yes?"PASS":"FAIL",name);failed+=!yes;}
int main(){
 uint32_t item=0;
 check(token_item(token(123),&item)&&item==123,"reserved dye key round trip");
 check(!token_item(0,&item)&&!token_item(123,&item)&&!token_item(token(0),&item),"ordinary and empty keys are not dye requests");
 check(!token_item(token(123)|(1ull<<48),&item),"reserved key rejects nonzero padding");
 std::vector<Stock> stock{{123,2},{456,3}};
 std::vector<View> rows{view(123,7)};
 merge(&rows,stock);
 check(rows.size()==2&&count(rows[0])==9&&key(rows[0])==token(123),"source dyes merge with a bag row and use a reserved key");
 check(id(rows[1])==456&&count(rows[1])==3,"a dye absent from the bag gets its own row");
 check(available(0xffffffffu,1)==0xffffffffu,"counts saturate");
 {
  // #132: the game lists the empty "no dye" row first, then every dye by count, most first (the ones the player has),
  // then the palettes it has none of. A source dye joins the dyes the player has, by its count with the source's.
  View none{};put(none.data(),0,~0ull);
  std::vector<View> palette{none,view(10,9),view(11,2),view(12,0),view(13,0),view(14,0)};
  put(palette[3].data(),0x3c,uint8_t{1});
  std::vector<Stock> sources{{13,5},{20,1},{11,1}};
  merge(&palette,sources);
  order_with_sources(&palette);
  std::vector<uint32_t> ids;for(const auto& v:palette)ids.push_back(id(v));
  check(ids==std::vector<uint32_t>({0,10,13,11,20,12,14}),"source dyes sort among the player's own, by count");
  check(key(palette[0])==~0ull,"the empty row stays first");
  check(count(palette[2])==5&&key(palette[2])==token(13)&&count(palette[3])==3,"a moved row keeps its count and key");
  check(get<uint8_t>(palette[5].data(),0x3c)==1,"the palettes the player has none of keep the game's order");
  std::vector<View> equal{none,view(30,4),view(31,4),view(32,0)};
  merge(&equal,{{32,4}});order_with_sources(&equal);
  ids.clear();for(const auto& v:equal)ids.push_back(id(v));
  check(ids==std::vector<uint32_t>({0,30,31,32}),"equal counts keep the game's order, a source dye after them");
  std::vector<View> plain{none,view(40,1),view(41,0)};
  order_with_sources(&plain);
  check(id(plain[1])==40&&id(plain[2])==41,"a list without source dyes keeps the game's order");
  std::vector<View> empty;order_with_sources(&empty);check(empty.empty(),"an empty list stays empty");
 }
 check(!offer_allowed(false,true,true)&&!offer_allowed(true,false,true)&&!offer_allowed(true,true,false),"vanilla older server and expired offers show no sources");
 check(offer_allowed(true,true,true),"only a supported server's live offer is shown");
 for(int scenario=0;scenario<7;++scenario){
  int bag=scenario==0?1:0,chest=1,palette=0,bag_calls=0,source_calls=0;
  bool access=scenario!=2, live=scenario!=3, guards=scenario!=4;
  auto take=[&](uint32_t who){
   if(who==1){++bag_calls;if(bag){--bag;return Payment{};}return Payment{0,1};}
   ++source_calls;
   if(scenario==5){--chest;throw std::runtime_error("injected removal fault");}
   if(scenario==6)return Payment{0x27,0};
   --chest;return Payment{};
  };
  auto p=pay_one(1,std::vector<uint32_t>{2000},[&](uint32_t){return access&&live;},take,[&]{return guards;});
  if(!p.error&&!p.left)palette=1;else{bag=scenario==0?1:0;chest=1;}
  check(scenario<2?palette==1:palette==0&&chest==1,"payment errors and faults roll back before the palette write");
  if(scenario==0)check(source_calls==0&&bag_calls==1&&chest==1,"bag dye pays before linked chest dye");
  if(scenario==1)check(source_calls==1&&chest==0,"linked chest pays when the bag holds no dye");
  if(scenario==2||scenario==3||scenario==4)check(source_calls==0,"no debit after permission registration or commit-guard refusal");
 }
 auto p=pay_one(1,std::vector<uint32_t>{},[](uint32_t){return true;},[](uint32_t){return Payment{0,1};},[]{return true;});
 check(p.left==1,"another player's unregistered chest cannot pay");
 std::printf("%d checks, %d failures\n",checked,failed);return failed?1:0;
}
