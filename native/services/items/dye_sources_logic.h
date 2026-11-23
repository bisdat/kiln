// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
namespace kitems::dyes {
constexpr uint32_t max_rows=256;
constexpr size_t view_bytes=72;
using View=std::array<uint8_t,view_bytes>;
struct Stock {uint32_t item,count;};
struct Payment {uint8_t error=0;uint32_t left=0;};
inline uint64_t token(uint32_t item){return item|(0xffull<<32);}
inline bool token_item(uint64_t key,uint32_t* item){
 if((key>>32)!=0xff||!static_cast<uint32_t>(key))return false;
 *item=static_cast<uint32_t>(key);return true;
}
template<class T> T get(const void* p,size_t off){T v{};std::memcpy(&v,static_cast<const uint8_t*>(p)+off,sizeof v);return v;}
template<class T> void put(void* p,size_t off,T v){std::memcpy(static_cast<uint8_t*>(p)+off,&v,sizeof v);}
inline uint32_t id(const View& v){return get<uint32_t>(v.data(),8);}
inline uint32_t count(const View& v){return get<uint32_t>(v.data(),16);}
inline uint64_t key(const View& v){return get<uint64_t>(v.data(),0);}
inline uint32_t available(uint32_t normal,uint32_t extra){return static_cast<uint32_t>(std::min<uint64_t>(uint64_t(normal)+extra,0xffffffffu));}
inline View view(uint32_t item,uint32_t amount){View v{};put(v.data(),0,token(item));put(v.data(),8,item);put(v.data(),16,amount);return v;}
inline bool offer_allowed(bool server_kiln,bool supported,bool live){return server_kiln&&supported&&live;}
inline uint32_t extra(const std::vector<Stock>& rows,uint32_t item){for(const auto& r:rows)if(r.item==item)return r.count;return 0;}
inline void merge(std::vector<View>* rows,const std::vector<Stock>& stock){
 for(const auto& s:stock){
  if(!s.item||!s.count)continue;
  auto it=std::find_if(rows->begin(),rows->end(),[&](const View& v){return id(v)==s.item;});
  if(it==rows->end()){if(rows->size()<4096)rows->push_back(view(s.item,s.count));}
  else {put(it->data(),0,token(s.item));put(it->data(),16,available(count(*it),s.count));}
 }
}
// The game's palette order (builder C 0xd14ce0, GitHub #132): its empty "no dye" row first (item 0), then every row by
// count, most first (the dyes the player has), then the palettes the player has none of (count 0) in the game's own
// order. After merge() a source dye still sits where the game put it (among the ones the player has none of) or at the
// end; this puts it with the dyes the player has, by its count with the sources', as the game orders a dye it sees in
// the bag. Stable: equal counts and the count-0 rows keep the game's order.
inline void order_with_sources(std::vector<View>* rows){
 auto first=rows->begin();
 while(first!=rows->end()&&id(*first)==0)++first;
 std::stable_sort(first,rows->end(),[](const View& a,const View& b){return count(a)>count(b);});
}
// No palette mutation is reachable until this returns success. The caller's own Tint failure branch rolls back the
// game's transaction for every error, shortfall or exception, including a take that faulted after queuing its op.
template<class Allowed,class Take,class Guards>
Payment pay_one(uint32_t player,const std::vector<uint32_t>& sources,Allowed allowed,Take take,Guards guards){
 try {
  if(!guards())return {0x27,0};
  Payment p=take(player);
  if(p.error)return p;
  if(p.left>1)return {0x27,0};
  if(p.left)for(uint32_t container:sources){
   if(!allowed(container))continue;
   p=take(container);
   if(p.error||!p.left)break;
   if(p.left>1)return {0x27,0};
  }
  if(!guards())return {0x27,0};
  return p;
 }catch(...){return {0x27,0};}
}
} // namespace kitems::dyes
