// SPDX-License-Identifier: MIT
#include "../new_world.h"
#include <fstream>
#include <cstdio>
#include <unistd.h>
int main() {
    char dir[]="/tmp/kiln-new-world-XXXXXX";
    if(!mkdtemp(dir)) return 2;
    const auto path=std::filesystem::path(dir)/"world.ksd";
    int fail=0;
    auto check=[&](bool ok,const char* name) {if(!ok){++fail;std::fprintf(stderr,"FAIL %s\n",name);}};
    check(!kiln::saves::new_world_file({},false),"unknown slot is not new-world proof");
    check(kiln::saves::new_world_file(path,false),"confirmed absent world is new");
    check(!kiln::saves::new_world_file(path,false,true,7,true),"a missing Steam mirror does not erase an existing engine save identity");
    check(!kiln::saves::new_world_file(path,false,false),"unreadable engine identity is not new-world proof");
    {std::ofstream f(path);f<<"unreadable header";}
    check(!kiln::saves::new_world_file(path,false),"existing damaged header is not a new world");
    check(!kiln::saves::new_world_file(path,true),"existing readable save is not new");
    std::filesystem::remove(path);
    std::filesystem::create_symlink(path.parent_path()/"missing",path);
    check(!kiln::saves::new_world_file(path,false),"dangling symlink is not new-world proof");
    std::filesystem::remove_all(dir);
    std::printf("new world proof: %d passed, %d failed\n",7-fail,fail);
    return fail?1:0;
}
