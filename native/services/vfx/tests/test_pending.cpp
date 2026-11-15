// SPDX-License-Identifier: MIT
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace fs=std::filesystem;
#ifdef PENDING_BASELINE
struct Budget { size_t files=0, entries=0; uint64_t bytes=0, byte_limit=473ull*65536; };
struct Input { std::vector<std::pair<std::string,std::string>> documents; };
std::string to_utf8(const fs::path& p){return p.string();}
std::string read_text(const fs::path& p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
#include "pending_baseline.inc"
#else
#include "../pending.h"
#ifndef OLD_PENDING_CLEANUP
#include "../pending_cleanup.h"
#endif
using Budget=kvx_pending::Budget;
#endif
static unsigned checks=0,failures=0;
static void check(bool ok,const char* why){++checks;if(!ok){++failures;std::printf("FAIL %s\n",why);}}
static void put(const fs::path& p,size_t n=2){fs::create_directories(p.parent_path());std::ofstream(p)<<std::string(n,' ');}
static auto load(const fs::path& root,const fs::path& folder,Budget& b,std::vector<std::string>& logs){
#ifdef PENDING_BASELINE
    return baseline(folder);
#else
    return kvx_pending::read(root,folder,b,[](const fs::path& p,size_t max,std::string* out,std::string* why){
        int fd=open(p.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK);if(fd<0){*why="cannot open without following a link";return false;}
        struct stat st{};bool ok=fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_size>=0&&uint64_t(st.st_size)<=max;
        if(ok){out->resize(st.st_size);auto n=::read(fd,out->data(),out->size());char extra;ok=n==st.st_size&&::read(fd,&extra,1)==0;}
        close(fd);if(!ok)*why="not regular, too large, or changed during reading";return ok;
    },[&](const fs::path& p,const std::string& why){logs.push_back(p.filename().string()+": "+why);});
#endif
}
int main(int argc,char** argv){
    fs::path base=argv[1];fs::remove_all(base);fs::create_directories(base);
    std::vector<std::string> logs;
    for(int i=0;i<474;++i)put(base/"count"/(std::to_string(i)+".json"));
    Budget count;auto files=load(base,base/"count",count,logs);
    check(files.size()==473&&!logs.empty(),"the 474th file is named and left unread");
    put(base/"large"/"too-large.json",65537);logs.clear();Budget large;
    check(load(base,base/"large",large,logs).empty()&&logs.size()==1&&logs[0].find("too-large.json")!=std::string::npos,"oversized file is named once, not read");
    put(base/"secret.json",20);fs::create_directories(base/"links");fs::create_symlink(base/"secret.json",base/"links"/"linked.json");
    logs.clear();Budget links;check(load(base,base/"links",links,logs).empty()&&logs.size()==1,"linked file is refused");
    fs::create_directory_symlink(base/"large",base/"linked-folder");logs.clear();Budget folder;
    check(load(base,base/"linked-folder",folder,logs).empty()&&!logs.empty(),"linked mod directory is refused");
    put(base/"total"/"a.json",4);put(base/"total"/"b.json",4);logs.clear();Budget total;total.byte_limit=5;
    check(load(base,base/"total",total,logs).size()==1&&logs.size()==1,"aggregate bytes are bounded before reading");
    for(int i=0;i<237;++i){put(base/"one"/(std::to_string(i)+".json"));put(base/"two"/(std::to_string(i)+".json"));}
    Budget shared;logs.clear();auto one=load(base,base/"one",shared,logs);auto two=load(base,base/"two",shared,logs);
    check(one.size()+two.size()==473,"the file quota is shared across mods in one replay pass");
#ifndef PENDING_BASELINE
    Budget skipped; skipped.skipped.push_back("one"); logs.clear();
    auto ignored=load(base,base/"one",skipped,logs); auto active=load(base,base/"two",skipped,logs);
    check(ignored.empty()&&active.size()==237&&skipped.files==237,"policy-skipped mods do not consume the replay allowance of active mods");
#endif
#ifndef PENDING_BASELINE
    auto discard = [&](const fs::path& p,std::string_view bytes) {
#ifdef OLD_PENDING_CLEANUP
        std::error_code ec;return fs::remove(p,ec);
#else
        return kvx::discard_pending(p,bytes);
#endif
    };
    auto text = [](const fs::path& p,const char* bytes){fs::create_directories(p.parent_path());std::ofstream(p)<<bytes;};
    text(base/"owned"/"ok.json","ours");
    check(discard(base/"owned"/"ok.json","ours")&&!fs::exists(base/"owned"/"ok.json"),"cleanup removes its own matching file");
    text(base/"owned"/"changed.json","mine");
    check(!discard(base/"owned"/"changed.json","ours")&&fs::exists(base/"owned"/"changed.json"),"cleanup preserves another writer's replacement");
    text(base/"outside"/"cancel.json","ours");fs::create_directories(base/"cancel");
    fs::rename(base/"cancel",base/"old-cancel");fs::create_directory_symlink(base/"outside",base/"cancel");
    check(!discard(base/"cancel"/"cancel.json","ours")&&fs::exists(base/"outside"/"cancel.json"),"cleanup refuses a redirected parent instead of deleting outside pending");
    text(base/"outside"/"link.json","ours");fs::create_symlink(base/"outside"/"link.json",base/"owned"/"link.json");
    check(!discard(base/"owned"/"link.json","ours")&&fs::exists(base/"outside"/"link.json"),"cleanup refuses linked files");
#endif
    fs::remove_all(base);std::printf("pending replay: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
