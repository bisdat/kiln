// SPDX-License-Identifier: MIT
// Real Windows handle semantics, no game. Built here; run by the coordinator through run.sh under Wine.
#include "../../../core/abi/services/kiln_capacity_util.hpp"
#include <cstdio>
#include <fstream>
namespace fs=std::filesystem;
namespace cap=kiln::capacity;
int main() {
    int failed=0;
    auto check=[&](bool ok,const char* name){std::printf("%s pending-win: %s\n",ok?"PASS":"FAIL",name);if(!ok)++failed;};
    try {
        wchar_t temp[MAX_PATH]{}, unique[MAX_PATH]{};
        if(!GetTempPathW(MAX_PATH,temp) || !GetTempFileNameW(temp,L"kpd",0,unique)) {
            check(false,"create temporary base");return 1;
        }
        DeleteFileW(unique);
        fs::path base=unique;
        fs::create_directories(base);
        cap::pending_io::TrustedBase trusted{base};
        auto root=base/L"pending",file=root/L"mod"/L"a.json";
        cap::pending_io::diagnostic=[](const auto& p,const auto& why){std::printf("pending-win diagnostic: %ls: %s\n",p.c_str(),why.c_str());};
        check(cap::pending_io::write_atomic(trusted,file,"old"),"create missing store and mod; write");
        check(cap::pending_io::write_atomic(trusted,file,"new"),"replace");
        std::string value;
        check(cap::pending_io::read(trusted,file,&value,100) && value=="new","read complete replacement");
        auto listed=cap::pending_files(trusted,root);
        check(listed.size()==1 && listed.front().name=="a.json","enumerate");
        {std::ofstream out(fs::path(file.wstring()+L".tmp"));out<<"crash";}
        check(cap::pending_io::write_atomic(trusted,file,"after crash") && !fs::exists(file.wstring()+L".tmp"),"regular leftover temporary");
        check(cap::pending_io::remove(trusted,file) && !fs::exists(file),"remove regular file");
        const auto outside=base/L"outside";
        fs::create_directories(outside);
        {std::ofstream out(outside/L"victim");out<<"safe";}
        auto make_link=[&](const fs::path& link,const fs::path& target,DWORD flags,const char* name){
            if(CreateSymbolicLinkW(link.c_str(),target.c_str(),flags|SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) ||
               CreateSymbolicLinkW(link.c_str(),target.c_str(),flags)) return true;
            std::printf("SKIP pending-win: %s (CreateSymbolicLinkW code %lu)\n",name,GetLastError());return false;
        };
        fs::remove(root/L"mod");
        if(make_link(root/L"mod",outside,SYMBOLIC_LINK_FLAG_DIRECTORY,"mod-directory symlink")) {
            check(!cap::pending_io::write_atomic(trusted,file,"bad") && cap::pending_files(trusted,root).empty() && fs::is_symlink(root/L"mod"),"mod-directory link refused and retained");
            fs::remove(root/L"mod");
        }
        fs::create_directories(root/L"mod");
        if(make_link(file,outside/L"victim",0,"file symlink")) {
            check(!cap::pending_io::write_atomic(trusted,file,"bad") && !cap::pending_io::read(trusted,file,&value,100) &&
                  !cap::pending_io::remove(trusted,file) && fs::is_symlink(file),"file link refused and retained");
            fs::remove(file);
        }
        {std::ifstream in(outside/L"victim");in>>value;}
        check(value=="safe","external target unchanged");
        // Same generic signature used by other services: player layout above its containing directory is trusted.
        if(make_link(base/L"layout",outside,SYMBOLIC_LINK_FLAG_DIRECTORY,"trusted-layout symlink")) {
            auto via=base/L"layout"/L"service"/L"pending"/L"mod"/L"a.json";
            check(cap::write_atomic(via,"layout") && cap::pending_io::write_atomic({base/L"layout"/L"service"},via,"layout"),"linked trusted layout works");
            fs::remove(base/L"layout");
        }
        fs::remove_all(base);
    } catch(const std::exception& e) {std::printf("FAIL pending-win: exception: %s\n",e.what());++failed;}
    return failed?1:0;
}
