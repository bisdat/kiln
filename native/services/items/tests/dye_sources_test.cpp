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
