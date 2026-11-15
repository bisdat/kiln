// SPDX-License-Identifier: MIT
// Pending I/O: anchor every operation to opened, non-link directories. Windows directory handles deny delete/rename
// while in use; POSIX operations are relative to directory descriptors. No final-file open follows a link.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <filesystem>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#endif
namespace kiln::capacity::pending_io {
namespace fs = std::filesystem;
#ifdef KILN_PENDING_TESTING
inline const char* fail_step = nullptr;
inline bool injected(const char* step) {return fail_step && std::string_view(fail_step)==step;}
#else
inline bool injected(const char*) {return false;}
#endif
inline bool component(const std::string& s) {
    if (s.empty() || s=="." || s==".." || s.back()=='.' || s.back()==' ') return false;
    for (unsigned char c:s) if (!(c>='a'&&c<='z') && !(c>='A'&&c<='Z') && !(c>='0'&&c<='9') && c!='-' && c!='_' && c!='.') return false;
    // Windows device names remain special even with an extension.
    std::string stem=s.substr(0,s.find('.'));
    for(char& c:stem) if(c>='a'&&c<='z') c-=32;
    if(stem=="CON"||stem=="PRN"||stem=="AUX"||stem=="NUL" || (stem.size()==4&&(stem.substr(0,3)=="COM"||stem.substr(0,3)=="LPT")&&stem[3]>='1'&&stem[3]<='9')) return false;
    return true;
}
inline std::function<void(const fs::path&, const std::string&)> diagnostic;
inline void refused(const fs::path& p,const std::string& why) {
    static std::mutex mu; static std::set<fs::path> seen;
    std::lock_guard lock(mu);
    if(seen.insert(p).second && diagnostic) diagnostic(p,why);
}
#ifdef _WIN32
using Handle=HANDLE;
inline const HANDLE invalid=INVALID_HANDLE_VALUE;
inline void close(Handle h){if(h!=invalid) CloseHandle(h);}
inline bool attributes(Handle h,bool directory) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    return GetFileInformationByHandleEx(h,FileAttributeTagInfo,&info,sizeof info) &&
      !(info.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) && bool(info.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)==directory;
}
#else
using Handle=int;
inline constexpr int invalid=-1;
inline void close(Handle h){if(h>=0) ::close(h);}
#endif
struct Directory {
    Handle h=invalid;
    fs::path path;
#ifdef _WIN32
    std::vector<Handle> ancestors;
#endif
    Directory()=default;
    Directory(const Directory&)=delete;
    ~Directory(){close(h);
#ifdef _WIN32
      for(auto x:ancestors) close(x);
#endif
    }
    bool open(const fs::path& input,bool create=false) {
        std::error_code ec;
        auto absolute=fs::absolute(input,ec);
        if(ec) return false;
        path=absolute.root_path();
#ifdef _WIN32
        auto acquire=[&](const fs::path& p){return CreateFileW(p.c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,
          FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);};
        h=acquire(path); if(h==invalid||!attributes(h,true)) return false;
#else
        h=::open(path.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC); if(h<0) return false;
#endif
        for(const auto& part:absolute.relative_path()) {
            if(part=="." || part=="..") {refused(input,"dot path component");return false;}
            path/=part;
#ifdef _WIN32
            auto next=acquire(path);
            if(next==invalid && create && GetLastError()==ERROR_PATH_NOT_FOUND) {CreateDirectoryW(path.c_str(),nullptr);next=acquire(path);}
            if(next==invalid && create && GetLastError()==ERROR_FILE_NOT_FOUND) {CreateDirectoryW(path.c_str(),nullptr);next=acquire(path);}
            if(next==invalid || !attributes(next,true)) {close(next);refused(path,"directory missing or reparse point");return false;}
            ancestors.push_back(h);h=next;
#else
            int next=openat(h,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(next<0 && create && errno==ENOENT) {if(mkdirat(h,part.c_str(),0700)==0 && fsync(h)!=0) return false;next=openat(h,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);}
            if(next<0) {if(errno!=ENOENT)refused(path,"directory missing or link");return false;}
            close(h);h=next;
#endif
        }
        return true;
    }
    // Missing=0, regular=1, unsafe=-1, unknown I/O failure=-2. A reparse point is always unsafe, including a dangling one.
    int regular(const fs::path& name) const {
        if(injected("stat"))return -2;  // unknown I/O failure must not be counted as absent/non-regular
#ifdef _WIN32
        auto f=CreateFileW((path/name).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
          FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
        if(f==invalid) return GetLastError()==ERROR_FILE_NOT_FOUND?0:-2;
        FILE_ATTRIBUTE_TAG_INFO info{};
        if(!GetFileInformationByHandleEx(f,FileAttributeTagInfo,&info,sizeof info)){close(f);return -2;}
        const int kind=(info.FileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))?-1:1;
        close(f);return kind;
#else
        struct stat st{};
        if(fstatat(h,name.c_str(),&st,AT_SYMLINK_NOFOLLOW)<0) return errno==ENOENT?0:-2;
        return S_ISREG(st.st_mode)?1:-1;
#endif
    }
    bool remove(const fs::path& name) const {
        if(regular(name)!=1) {refused(path/name,"removal requires a regular file");return false;}
#ifdef _WIN32
        auto f=CreateFileW((path/name).c_str(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
          FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(f==invalid)return false;
        FILE_DISPOSITION_INFO info{TRUE};
        bool ok=attributes(f,false)&&SetFileInformationByHandle(f,FileDispositionInfo,&info,sizeof info);close(f);return ok;
#else
        // unlinkat never follows a replacement link. It can only unlink this entry inside the pinned directory.
        return unlinkat(h,name.c_str(),0)==0;
#endif
    }
    bool read(const fs::path& name,std::string* out,size_t limit) const {
#ifdef _WIN32
        auto f=CreateFileW((path/name).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(f==invalid)return false;
        if(!attributes(f,false)){close(f);refused(path/name,"read refused reparse point");return false;}
        out->resize(limit+1); DWORD n=0;bool ok=ReadFile(f,out->data(),static_cast<DWORD>(out->size()),&n,nullptr);out->resize(n);close(f);
#else
        int f=openat(h,name.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        if(f<0)return false;
        struct stat st{};if(fstat(f,&st)||!S_ISREG(st.st_mode)){close(f);refused(path/name,"read requires a regular file");return false;}
        out->clear();char buf[4096];bool ok=true;
        while(out->size()<=limit){auto n=::read(f,buf,std::min(sizeof buf,limit+1-out->size()));if(n<0){if(errno==EINTR)continue;ok=false;break;}if(!n)break;out->append(buf,static_cast<size_t>(n));}close(f);
#endif
        return ok && out->size()<=limit;
    }
    template<class F> bool visit(F fn, bool* stopped=nullptr, uint64_t* fuel=nullptr) const {
        if (injected("enumerate")) { refused(path,"pending directory enumeration failed"); return false; }
        auto allowed=[&]() {
            if (stopped && *stopped) return false;
            if (fuel && *fuel==0) { if(stopped)*stopped=true; return false; }
            if (fuel) --*fuel;
            return true;
        };
        bool ok=true;
#ifdef _WIN32
        WIN32_FIND_DATAW data{};auto f=FindFirstFileW((path/L"*").c_str(),&data);
        if(f==INVALID_HANDLE_VALUE) {
            ok=GetLastError()==ERROR_FILE_NOT_FOUND;
            if(!ok)refused(path,"pending directory enumeration failed");
            return ok;
        }
        do { fs::path n=data.cFileName; if(n!=L"."&&n!=L"..") {if(!allowed())break;fn(n);} }
        while(FindNextFileW(f,&data));
        ok=(stopped&&*stopped)||GetLastError()==ERROR_NO_MORE_FILES;
        FindClose(f);
#else
        int fd=openat(h,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(fd<0) {refused(path,"pending directory enumeration failed");return false;}
        auto stream=fdopendir(fd);
        if(!stream){close(fd);refused(path,"pending directory enumeration failed");return false;}
        for(;;) {
            errno=0; auto* entry=readdir(stream);
            if(!entry){ok=errno==0;break;}
            std::string n=entry->d_name;
            if(n!="."&&n!="..") {if(!allowed())break;fn(fs::path(n));}
        }
        closedir(stream);
#endif
        if(!ok)refused(path,"pending directory enumeration failed");
        return ok;
    }
    uint64_t size(const fs::path& name) const {
#ifdef _WIN32
        auto f=CreateFileW((path/name).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        LARGE_INTEGER n{};bool ok=f!=invalid && attributes(f,false) && GetFileSizeEx(f,&n);close(f);return ok?static_cast<uint64_t>(n.QuadPart):UINT64_MAX;
#else
        int f=openat(h,name.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);if(f<0)return UINT64_MAX;
        struct stat st{};bool ok=fstat(f,&st)==0&&S_ISREG(st.st_mode);close(f);return ok?static_cast<uint64_t>(st.st_size):UINT64_MAX;
#endif
    }

};
inline bool read(const fs::path& file,std::string* out,size_t limit) {Directory d;return d.open(file.parent_path())&&component(file.filename().string())&&d.read(file.filename(),out,limit);}
inline bool remove(const fs::path& file) {Directory d;return d.open(file.parent_path())&&component(file.filename().string())&&d.remove(file.filename());}
inline bool write_atomic(const fs::path& file,std::string_view bytes) {
    if(file.filename().empty() || file.filename()=="." || file.filename()=="..") return false;
    Directory d;if(!d.open(file.parent_path(),true))return false;
    auto name=file.filename(),tmp=fs::path(name.string()+".tmp");
    if(d.regular(name)<0||d.regular(tmp)<0){refused(file,"target or temporary name is a link/non-regular file");return false;}
    if(d.regular(tmp)==1 && !d.remove(tmp))return false;
#ifdef _WIN32
    if(injected("create")) return false;
    auto f=CreateFileW((d.path/tmp).c_str(),GENERIC_WRITE|DELETE|FILE_READ_ATTRIBUTES,0,nullptr,CREATE_NEW,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(f==invalid)return false;
    DWORD n=0;bool ok=!injected("write")&&attributes(f,false)&&bytes.size()<=MAXDWORD&&WriteFile(f,bytes.data(),static_cast<DWORD>(bytes.size()),&n,nullptr)&&n==bytes.size();
    if(ok) ok=!injected("flush") && FlushFileBuffers(f);
    if(ok && injected("rename")) ok=false;
    if(ok) {
        const auto target=(d.path/name).wstring();
        std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO)+target.size()*sizeof(wchar_t));
        auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        rename->ReplaceIfExists=TRUE;rename->RootDirectory=nullptr;rename->FileNameLength=static_cast<DWORD>(target.size()*sizeof(wchar_t));
        std::memcpy(rename->FileName,target.data(),rename->FileNameLength);
        ok=SetFileInformationByHandle(f,FileRenameInfo,rename,static_cast<DWORD>(buffer.size()))!=0;
    }
    if(!ok){FILE_DISPOSITION_INFO info{TRUE};SetFileInformationByHandle(f,FileDispositionInfo,&info,sizeof info);}
    if(ok) ok=!injected("directory-flush") && FlushFileBuffers(f);
    close(f);
#else
    if(injected("create")) return false;
    int f=openat(d.h,tmp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(f<0)return false;
    bool ok=!injected("write");size_t pos=0;while(ok && pos<bytes.size()){auto n=::write(f,bytes.data()+pos,bytes.size()-pos);if(n<0&&errno==EINTR)continue;if(n<=0){ok=false;break;}pos+=static_cast<size_t>(n);}
    if(ok) ok=!injected("flush") && fsync(f)==0;
    close(f);
    if(ok && injected("rename")) ok=false;
    if(ok)ok=renameat(d.h,tmp.c_str(),d.h,name.c_str())==0;
    if(ok) ok=!injected("directory-flush") && fsync(d.h)==0;
#endif
    if(!ok && d.regular(tmp)==1)d.remove(tmp);
    return ok;
}
} // namespace
