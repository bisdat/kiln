// SPDX-License-Identifier: MIT
// The actual define_json keep lambda, with only its filesystem writer replaced by a gate.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include "nlohmann/json.hpp"
#include "../pending.h"
#include "../pending_cleanup.h"
namespace kvx { std::mutex& pending_io_mutex(){static std::mutex mutex;return mutex;} }
namespace fs=std::filesystem;
using ojson=nlohmann::ordered_json;
std::mutex observer_mutex;
std::promise<void>* entered;
std::shared_future<void> resume;
bool write_ok;
std::atomic<int> notifications=0;
namespace kiln::capacity { bool test_write_atomic(const fs::path&,std::string_view){entered->set_value();resume.wait();return write_ok;} }
namespace ledger { void kept(const std::string&,const std::string&,uint64_t){++notifications;} }
fs::path to_path(const std::string& s){return fs::path(s);}
#define KV_W(...) ((void)0)
#define write_atomic test_write_atomic
void produce() {
    struct {std::string id="legacy";} m;
    ojson doc={{"vfx",{{"effect",ojson::object()}}}};
    fs::path pending="unused-test-path";
    std::unique_lock lk(observer_mutex);
#include "legacy_keep.inc"
    keep("vfx","effect","effect.json",1);
}
#undef write_atomic
int main(){
    using namespace std::chrono_literals;
    unsigned fail=0;
    for(bool ok:{false,true}){
        std::promise<void> started,unblock;entered=&started;resume=unblock.get_future().share();write_ok=ok;notifications=0;
        auto writer=std::async(std::launch::async,[]{produce();});started.get_future().wait();
        auto observer=std::async(std::launch::async,[]{std::lock_guard lk(observer_mutex);});
        if(observer.wait_for(300ms)!=std::future_status::ready){++fail;std::puts("FAIL legacy file write holds observer mutex");}
        fail+=notifications!=0;
        unblock.set_value();writer.get();observer.get();fail+=notifications!=(ok?1:0);
    }
    std::printf("legacy pending: 6 checks, %u failures\n",fail);return fail?1:0;
}
