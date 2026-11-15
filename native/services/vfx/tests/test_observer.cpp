// Exercises boot.cpp's actual observation code with a controllable registry; no game memory.
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include "../ledger.h"
#include "services/kiln_caps.h"
#include "../cook/vfxcook.h"
namespace ledger = kvx::ledger;
struct Registry { uint64_t count = 1575, slots = 4096; uint32_t limit = 2048, init_state = 2; };
namespace kvx {
struct Game {
    Registry reg;
    bool readable = true;
    bool has_guard() { return true; }
    bool registry(void*, Registry* out) { *out = reg; return readable; }
    int is_registered(void*, const uint8_t*) { return 0; }
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
int main() {
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
    std::printf("observer: 5 checks, %u failures\n", failures);
    return failures ? 1 : 0;
}
