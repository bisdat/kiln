// Exercises boot.cpp's actual observation code with a controllable registry; no game memory.
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <future>
#include "../ledger.h"
#include "services/kiln_caps.h"
#include "../cook/vfxcook.h"
namespace ledger = kvx::ledger;
#include "../resource_limits.h"
using kvx::kMaxVerifiedResourceBytes;
struct Registry { uint64_t count = 1575, slots = 4096; uint32_t limit = 2048, init_state = 2; };
namespace kvx {
struct Game {
    Registry reg;
    bool readable = true;
    bool member = false;
    bool has_guard() { return true; }
    bool registry(void*, Registry* out) { *out = reg; return readable; }
    int is_registered(void*, const uint8_t*) { return member ? 1 : 0; }
    bool read_resource(void*, const uint8_t*, void*, size_t) { return false; }
} fake;
Game* core_game() { return &fake; }
}
struct Built { uint32_t served_size = 0; uint64_t served = 0; };
std::map<kvb::Guid, Built> g_built;
std::mutex g_mu;
ledger::Id id_of(const kvb::Guid&) { return {}; }
uint64_t bytes_hash(const uint8_t*, size_t) { return 0; }
void lower_rows(bool, bool, const char*, uint32_t) {}
#define KV_I(...) ((void)0)
#include "observer.inc"
int main(int argc, char** argv) {
    ledger::reset(); ledger::set_available(true,true);
    ledger::Boot b; b.game_counted = true; b.game_vfx = 1575; ledger::boot(b);
    auto poll = [] { g_observe_n = 0; observe_registry(reinterpret_cast<void*>(1)); };
    poll();
    unsigned failures = !ledger::observed();
    kvx::fake.reg.init_state = 1; poll();
    failures += ledger::observed();
    kvx::fake.reg.init_state = 2; poll();
    failures += !ledger::observed();
    kvx::fake.readable = false; poll();
    failures += ledger::observed();
    kvx::fake.readable = true; poll();
    build_world_ended();
    failures += ledger::observed();
    kvx::fake.member = true;
    kvx::fake.reg.count = 1576;
    b.built.push_back({{}, "fixture", "effect", 1});
    ledger::boot(b);
    g_built[kvb::Guid{}] = Built{(1u << 20) + 1, 0};
    poll();
    KilnCapacityInfo info{}; info.size = sizeof info; ledger::budget(&info);
    failures += info.readiness != KILN_CAPACITY_ACCOUNTING_MISMATCH;
#ifdef KILN_VFX_LEDGER_TEST
    g_built.clear(); kvx::fake.member=false; kvx::fake.reg.count=1575;
    b.built.clear(); b.admission=2048;
    ledger::reset(); ledger::set_available(true,true); ledger::boot(b);
    ledger::set_pending_root(argc>1 ? argv[1] : "/tmp/kiln-vfx-observer-pending"); poll();
    std::promise<void> entered, resume;
    auto release=resume.get_future().share();
    ledger::set_writer_for_tests([&](const auto&,auto){entered.set_value();release.wait();return false;});
    auto writing=std::async(std::launch::async,[&]{
        ledger::Request q; q.mod="writer"; q.name="stalled"; q.guid[0]=1; q.timing=KILN_DEFINE_ALLOW_NEXT_START;
        q.canonical=q.block="{}";
        KilnDefinitionReply r{}; r.size=sizeof r; std::string report; ledger::define(q,&r,&report); return r;
    });
    entered.get_future().wait();
    auto sampling=std::async(std::launch::async,[&]{poll();});
    auto reading=std::async(std::launch::async,[]{KilnCapacityInfo n{};n.size=sizeof n;ledger::budget(&n);});
    auto stopping=std::async(std::launch::async,[]{build_world_ended();});
    using namespace std::chrono_literals;
    auto nonblocking = [&](auto& task,const char* name) {
        if(task.wait_for(300ms)!=std::future_status::ready){++failures;std::printf("FAIL stalled writer blocks %s\n",name);}
    };
    nonblocking(sampling,"the actual observer");
    nonblocking(reading,"the budget copy");
    nonblocking(stopping,"the actual world-stop hook");
    resume.set_value(); sampling.get(); reading.get(); stopping.get();
    failures += writing.get().reason_code!=KILN_DEFINE_STORAGE_FAILURE;
    ledger::set_writer_for_tests({});
#endif
    std::printf("observer: 10 checks, %u failures\n", failures);
    return failures ? 1 : 0;
}
