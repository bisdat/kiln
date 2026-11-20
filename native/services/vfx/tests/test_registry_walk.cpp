// The boot observer against SimGame's real record/map pointer walk, shared with game.cpp.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include "../game.h"
#include "../ledger.h"
#include "../resource_limits.h"
#include "../cook/vfxcook.h"
#include "services/kiln_caps.h"
namespace kvx {
void vlog(int, const char*, ...) {}
uint32_t thread_id() { return 1; }
uint64_t now_ms() { return 0; }
}
using namespace kvx;
struct Built { uint32_t served_size; uint64_t served; };
std::map<kvb::Guid, Built> g_built;
std::mutex g_mu;
ledger::Id id_of(const kvb::Guid& g) { ledger::Id id; std::memcpy(id.data(),g.b,16); return id; }
uint64_t bytes_hash(const uint8_t* p,size_t n) { uint64_t h=1469598103934665603ull; for(size_t i=0;i<n;++i) h=(h^p[i])*1099511628211ull; return h; }
void lower_rows(bool,bool,const char*,uint32_t) {}
#include "observer.inc"
int main() {
    core_set_game(sim_game()); ledger::reset(); ledger::set_available(true,true);
    kvb::Guid guid{}; sim_known_guid(1,guid.b);
    uint8_t bytes[16]; sim_game()->read_resource(nullptr,guid.b,bytes,sizeof bytes);
    g_built[guid]={sizeof bytes,bytes_hash(bytes,sizeof bytes)};
    ledger::Boot b; b.game_counted=true; b.game_vfx=1; b.admission=2048;
    b.built.push_back({id_of(guid),"fixture","effect",1}); ledger::boot(b);
    unsigned fail=0;
    auto poll=[&](uint32_t expected) {
        g_observe_n=0; observe_registry(reinterpret_cast<void*>(1));
        KilnCapacityInfo n{}; n.size=sizeof n; ledger::budget(&n);
        if(n.readiness!=expected) ++fail;
    };
    poll(KILN_CAPACITY_VALID);
    sim_resource_table(1); poll(KILN_CAPACITY_ACCOUNTING_MISMATCH);
    sim_resource_table(2,true); poll(KILN_CAPACITY_ACCOUNTING_MISMATCH);
    sim_resource_table(2); g_built[guid].served_size=kMaxVerifiedResourceBytes+1; poll(KILN_CAPACITY_ACCOUNTING_MISMATCH);
    std::printf("registry walk: 4 checks, %u failures\n",fail); return fail?1:0;
}
