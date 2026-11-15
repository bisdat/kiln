// SPDX-License-Identifier: MIT
// Pending I/O: follow the player's layout to an explicit trusted base, then refuse links below it.
// Windows pins opened directories against rename; POSIX operations are relative to directory descriptors.
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
// A file's name in a store. worldtables names its pool envelopes "@pool-<guid>.json": that prefix is a file name's only
// (never a folder's, so not a mod's or one on the way to the store) and once; the rest keeps the component rule.
inline bool file_component(const std::string& s) {return component(s) || (s.rfind("@pool-",0)==0 && component(s.substr(6)));}
inline std::function<void(const fs::path&, const std::string&)> diagnostic;
inline void refused(const fs::path& p,const std::string& why) {
    static std::mutex mu; static std::set<fs::path> seen;
    std::lock_guard lock(mu);
    if(diagnostic && seen.insert(p).second) diagnostic(p,why);
}
// The service chooses this boundary; do not infer it by walking to a drive or filesystem root.
struct TrustedBase { fs::path path; };
inline bool unavailable(const fs::path& p,const char* operation,unsigned long code) {
    refused(p,std::string("unavailable: ")+operation+" (code "+std::to_string(code)+": "+
        std::system_category().message(static_cast<int>(code))+")");
    return false;
}
inline bool unsafe(const fs::path& p,const char* reason) {refused(p,std::string("unsafe: ")+reason);return false;}
#ifdef _WIN32
using Handle=HANDLE;
inline const HANDLE invalid=INVALID_HANDLE_VALUE;
inline void close(Handle h){if(h!=invalid) CloseHandle(h);}
inline bool attributes(Handle h,bool directory,const fs::path& path) {
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(h,&info))return unavailable(path,"read handle attributes",GetLastError());
    if(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)return unsafe(path,"file is a reparse point");
    return bool(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)==directory || unsafe(path,"unexpected file type");
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
    // Set by open() without create when the directory (the base or a store component) is simply not there: nothing is
    // stored yet. That is the ordinary state of every installation that never stored a definition, so it is not a
    // refusal and nothing is logged; the caller reads it as an empty store.
    bool missing=false;
    Directory()=default;
    Directory(const Directory&)=delete;
    ~Directory(){close(h);
#ifdef _WIN32
      for(auto x:ancestors) close(x);
#endif
    }
    // Ordinary open of a trusted directory: its ancestors are the player's layout, not our store.
    bool open(const fs::path& input,bool create=false) {
        std::error_code ec;
        missing=false;
        path=fs::absolute(input,ec);
        if(ec)return unavailable(input,"resolve trusted base",ec.value());
        if(create) {
            fs::create_directories(path,ec);
            if(ec)return unavailable(path,"create trusted base",ec.value());
        }
#ifdef _WIN32
        h=CreateFileW(path.c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
        if(h==invalid) {
            const DWORD code=GetLastError();
            if(!create && (code==ERROR_FILE_NOT_FOUND || code==ERROR_PATH_NOT_FOUND)) {missing=true;return false;}
            return unavailable(path,"open trusted base",code);
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(h,&info))return unavailable(path,"read trusted base attributes",GetLastError());
        if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))return unavailable(path,"trusted base is not a directory",ERROR_DIRECTORY);
        // Use the resolved target, not the player's junction/symlink spelling, for protected child opens.
        std::vector<wchar_t> resolved(32768);
        auto n=GetFinalPathNameByHandleW(h,resolved.data(),static_cast<DWORD>(resolved.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
        if(!n || n>=resolved.size())return unavailable(path,"resolve trusted base handle",n?ERROR_INSUFFICIENT_BUFFER:GetLastError());
        path=fs::path(std::wstring(resolved.data(),n));
#else
        h=::open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if(h<0) {
            const int code=errno;
            if(!create && code==ENOENT) {missing=true;return false;}
            return unavailable(path,"open trusted base",code);
        }
#endif
        return true;
    }
    bool open(const TrustedBase& base,const fs::path& input,bool create=false) {
        std::error_code ec;
        const auto anchor=fs::absolute(base.path,ec);
        if(ec)return unavailable(base.path,"resolve trusted base",ec.value());
        const auto target=fs::absolute(input,ec);
        if(ec)return unavailable(input,"resolve store path",ec.value());
        const auto relative=target.lexically_relative(anchor);
        if(relative.empty())return unsafe(input,"store path is outside trusted base");
        for(const auto& part:relative) if(part!="." && !component(part.string()))return unsafe(input,"invalid store component");
        if(!open(base.path,create))return false;
        for(const auto& part:relative) {
            if(part==".")continue;
            const auto child=path/part;
#ifdef _WIN32
            auto acquire=[&](){return CreateFileW(child.c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);};
            auto next=acquire();
            if(next==invalid && create && (GetLastError()==ERROR_PATH_NOT_FOUND || GetLastError()==ERROR_FILE_NOT_FOUND)) {
                if(!CreateDirectoryW(child.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS)
                    return unavailable(child,"create store directory",GetLastError());
                next=acquire();
            }
            if(next==invalid) {
                const DWORD code=GetLastError();
                if(!create && (code==ERROR_FILE_NOT_FOUND || code==ERROR_PATH_NOT_FOUND)) {missing=true;return false;}
                return unavailable(child,"open store directory",code);
            }
            BY_HANDLE_FILE_INFORMATION info{};
            if(!GetFileInformationByHandle(next,&info)) {auto code=GetLastError();close(next);return unavailable(child,"read store attributes",code);}
            if(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) {close(next);return unsafe(child,"store directory is a reparse point");}
            if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) {close(next);return unsafe(child,"store component is not a directory");}
            ancestors.push_back(h);h=next;
#else
            int next=openat(h,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(next<0 && create && errno==ENOENT) {
                if(mkdirat(h,part.c_str(),0700)==0) {
                    if(fsync(h)!=0)return unavailable(path,"flush created directory",errno);
                } else if(errno!=EEXIST)return unavailable(child,"create store directory",errno);
                next=openat(h,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            }
            if(next<0) {
                const int code=errno;struct stat st{};
                if(fstatat(h,part.c_str(),&st,AT_SYMLINK_NOFOLLOW)==0 && S_ISLNK(st.st_mode))return unsafe(child,"store directory is a link");
                if(!create && code==ENOENT) {missing=true;return false;}
                return unavailable(child,"open store directory",code);
            }
            close(h);h=next;
#endif
            path=child;
        }
        return true;
    }
    // Missing=0, regular=1, unsafe=-1, unknown I/O failure=-2. A reparse point is always unsafe, including a dangling one.
    int regular(const fs::path& name) const {
        if(injected("stat"))return -2;  // unknown I/O failure must not be counted as absent/non-regular
#ifdef _WIN32
        auto f=CreateFileW((path/name).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
          FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
        if(f==invalid) {auto code=GetLastError();if(code==ERROR_FILE_NOT_FOUND)return 0;unavailable(path/name,"open file attributes",code);return -2;}
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(f,&info)){auto code=GetLastError();close(f);unavailable(path/name,"read file attributes",code);return -2;}
        const int kind=(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))?-1:1;
        close(f);return kind;
#else
        struct stat st{};
        if(fstatat(h,name.c_str(),&st,AT_SYMLINK_NOFOLLOW)<0) {if(errno==ENOENT)return 0;unavailable(path/name,"read file attributes",errno);return -2;}
        return S_ISREG(st.st_mode)?1:-1;
#endif
    }
    bool remove(const fs::path& name) const {
        const auto kind=regular(name);
        if(kind!=1) {if(kind==-1)unsafe(path/name,"removal requires a regular file");return false;}
#ifdef _WIN32
        auto f=CreateFileW((path/name).c_str(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
          FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(f==invalid)return unavailable(path/name,"open file",GetLastError());
        FILE_DISPOSITION_INFO info{TRUE};
        bool ok=attributes(f,false,path/name)&&SetFileInformationByHandle(f,FileDispositionInfo,&info,sizeof info);
        if(!ok)unavailable(path/name,"remove regular file",GetLastError());close(f);return ok;
#else
        // unlinkat never follows a replacement link. It can only unlink this entry inside the pinned directory.
        return unlinkat(h,name.c_str(),0)==0 || unavailable(path/name,"remove regular file",errno);
#endif
    }
    bool read(const fs::path& name,std::string* out,size_t limit) const {
#ifdef _WIN32
        auto f=CreateFileW((path/name).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(f==invalid)return unavailable(path/name,"open file",GetLastError());
        if(!attributes(f,false,path/name)){close(f);refused(path/name,"read refused reparse point");return false;}
        out->resize(limit+1); DWORD n=0;bool ok=ReadFile(f,out->data(),static_cast<DWORD>(out->size()),&n,nullptr);out->resize(n);if(!ok)unavailable(path/name,"read file",GetLastError());close(f);
#else
        int f=openat(h,name.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        if(f<0)return errno==ELOOP?unsafe(path/name,"file is a link"):unavailable(path/name,"open file",errno);
        struct stat st{};if(fstat(f,&st)){const auto code=errno;close(f);return unavailable(path/name,"read file attributes",code);}
        if(!S_ISREG(st.st_mode)){close(f);return unsafe(path/name,"read requires a regular file");}
        out->clear();char buf[4096];bool ok=true;
        while(out->size()<=limit){auto n=::read(f,buf,std::min(sizeof buf,limit+1-out->size()));if(n<0){if(errno==EINTR)continue;unavailable(path/name,"read file",errno);ok=false;break;}if(!n)break;out->append(buf,static_cast<size_t>(n));}close(f);
#endif
        if(out->size()>limit)refused(path/name,"file exceeds the pending read limit");
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
            if(!ok)unavailable(path,"enumerate directory",GetLastError());
            return ok;
        }
        do { fs::path n=data.cFileName; if(n!=L"."&&n!=L"..") {if(!allowed())break;fn(n);} }
        while(FindNextFileW(f,&data));
        ok=(stopped&&*stopped)||GetLastError()==ERROR_NO_MORE_FILES;
        if(!ok)unavailable(path,"enumerate directory",GetLastError());
        FindClose(f);
#else
        int fd=openat(h,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(fd<0)return unavailable(path,"open directory enumeration",errno);
        auto stream=fdopendir(fd);
        if(!stream){const auto code=errno;close(fd);return unavailable(path,"open directory stream",code);}
        for(;;) {
            errno=0; auto* entry=readdir(stream);
            if(!entry){ok=errno==0;if(!ok)unavailable(path,"enumerate directory",errno);break;}
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
        if(f==invalid) {unavailable(path/name,"open file for size",GetLastError());return UINT64_MAX;}
        LARGE_INTEGER n{};
        bool ok=attributes(f,false,path/name) && GetFileSizeEx(f,&n);
        if(!ok)unavailable(path/name,"read file size",GetLastError());
        close(f);return ok?static_cast<uint64_t>(n.QuadPart):UINT64_MAX;
#else
        int f=openat(h,name.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        if(f<0) {if(errno==ELOOP)unsafe(path/name,"file is a link");else unavailable(path/name,"open file for size",errno);return UINT64_MAX;}
        struct stat st{};bool ok=fstat(f,&st)==0;
        if(!ok)unavailable(path/name,"read file size",errno);
        else if(!S_ISREG(st.st_mode))ok=unsafe(path/name,"size requires a regular file");
        close(f);return ok?static_cast<uint64_t>(st.st_size):UINT64_MAX;
#endif
    }

};
inline bool read(const TrustedBase& base,const fs::path& file,std::string* out,size_t limit) {
    Directory d;return d.open(base,file.parent_path())&&file_component(file.filename().string())&&d.read(file.filename(),out,limit);
}
inline bool read(const fs::path& file,std::string* out,size_t limit) {return read(TrustedBase{file.parent_path()},file,out,limit);}
inline bool remove(const TrustedBase& base,const fs::path& file) {
    Directory d;return d.open(base,file.parent_path())&&file_component(file.filename().string())&&d.remove(file.filename());
}
inline bool remove(const fs::path& file) {return remove(TrustedBase{file.parent_path()},file);}
inline bool write_atomic(const TrustedBase& base,const fs::path& file,std::string_view bytes) {
    if(file.filename().empty() || file.filename()=="." || file.filename()=="..")return unsafe(file,"invalid filename");
    Directory d;if(!d.open(base,file.parent_path(),true))return false;
    auto name=file.filename(),tmp=fs::path(name.string()+".tmp");
    for(const auto& entry:{name,tmp}) {
        const auto kind=d.regular(entry);
        if(kind<0) {if(kind==-1)unsafe(d.path/entry,"file or temporary is a link/non-regular file");return false;}
    }
    if(d.regular(tmp)==1 && !d.remove(tmp))return false;
#ifdef _WIN32
    if(injected("create")) return false;
    auto f=CreateFileW((d.path/tmp).c_str(),GENERIC_WRITE|DELETE|FILE_READ_ATTRIBUTES,0,nullptr,CREATE_NEW,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(f==invalid)return unavailable(d.path/tmp,"create temporary file",GetLastError());
    const char* operation="write temporary file";
    DWORD n=0;bool ok=!injected("write")&&attributes(f,false,d.path/tmp)&&bytes.size()<=MAXDWORD&&WriteFile(f,bytes.data(),static_cast<DWORD>(bytes.size()),&n,nullptr)&&n==bytes.size();
    if(ok) {operation="flush temporary file";ok=!injected("flush") && FlushFileBuffers(f);}
    if(ok) {operation="rename temporary file";if(injected("rename"))ok=false;}
    if(ok) {
        const auto target=(d.path/name).wstring();
        std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO)+target.size()*sizeof(wchar_t));
        auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        rename->ReplaceIfExists=TRUE;rename->RootDirectory=nullptr;rename->FileNameLength=static_cast<DWORD>(target.size()*sizeof(wchar_t));
        std::memcpy(rename->FileName,target.data(),rename->FileNameLength);
        ok=SetFileInformationByHandle(f,FileRenameInfo,rename,static_cast<DWORD>(buffer.size()))!=0;
    }
    if(!ok)unavailable(d.path/tmp,operation,GetLastError());
    if(!ok){FILE_DISPOSITION_INFO info{TRUE};SetFileInformationByHandle(f,FileDispositionInfo,&info,sizeof info);}
    if(ok) {ok=!injected("directory-flush") && FlushFileBuffers(f);if(!ok)unavailable(d.path/name,"flush renamed file",GetLastError());}
    close(f);
#else
    if(injected("create")) return false;
    int f=openat(d.h,tmp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(f<0)return unavailable(d.path/tmp,"create temporary file",errno);
    const char* operation="write temporary file";
    bool ok=!injected("write");size_t pos=0;while(ok && pos<bytes.size()){auto n=::write(f,bytes.data()+pos,bytes.size()-pos);if(n<0&&errno==EINTR)continue;if(n<=0){ok=false;break;}pos+=static_cast<size_t>(n);}
    if(ok) {operation="flush temporary file";ok=!injected("flush") && fsync(f)==0;}
    if(!ok)unavailable(d.path/tmp,operation,errno);
    close(f);
    if(ok && injected("rename")) ok=false;
    if(ok) {ok=renameat(d.h,tmp.c_str(),d.h,name.c_str())==0;if(!ok)unavailable(d.path/name,"rename temporary file",errno);}
    if(ok) {ok=!injected("directory-flush") && fsync(d.h)==0;if(!ok)unavailable(d.path,"flush store directory",errno);}
#endif
    if(!ok && d.regular(tmp)==1)d.remove(tmp);
    return ok;
}
// Legacy generic atomic writes trust their containing folder, preserving player-layout links for other services.
inline bool write_atomic(const fs::path& file,std::string_view bytes) {return write_atomic(TrustedBase{file.parent_path()},file,bytes);}
} // namespace
