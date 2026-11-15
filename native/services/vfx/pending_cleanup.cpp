// SPDX-License-Identifier: MIT
#include "pending_cleanup.h"
#include <string>
#include <algorithm>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace kvx {
std::mutex& pending_io_mutex(){static std::mutex mutex;return mutex;}
namespace {
#ifdef _WIN32
struct Handle { HANDLE h; ~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);} };
std::wstring normalize(std::wstring path) {
    if(path.starts_with(L"\\\\?\\UNC\\")) path=L"\\\\"+path.substr(8);
    else if(path.starts_with(L"\\\\?\\")) path.erase(0,4);
    std::replace(path.begin(),path.end(),L'/',L'\\'); return path;
}
#else
struct Fd { int fd; ~Fd(){if(fd>=0)close(fd);} };
#endif
}
bool discard_pending(const std::filesystem::path& file,std::string_view expected) noexcept {
    try {
        if(expected.size()>65536)return false;
#ifdef _WIN32
        // Deny writers/renames while comparing, then delete this opened object rather than reopening its path.
        Handle handle{CreateFileW(file.c_str(),GENERIC_READ|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
        BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{};
        if(handle.h==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle.h,&info) ||
           (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) || GetFileType(handle.h)!=FILE_TYPE_DISK ||
           !GetFileSizeEx(handle.h,&size) || size.QuadPart<0 || uint64_t(size.QuadPart)!=expected.size()) return false;
        std::wstring absolute(32768,L'\0'),actual(32768,L'\0');
        DWORD a=GetFullPathNameW(file.c_str(),static_cast<DWORD>(absolute.size()),absolute.data(),nullptr);
        DWORD b=GetFinalPathNameByHandleW(handle.h,actual.data(),static_cast<DWORD>(actual.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
        if(!a || a>=absolute.size() || !b || b>=actual.size()) return false;
        absolute.resize(a); actual.resize(b); absolute=normalize(std::move(absolute)); actual=normalize(std::move(actual));
        if(CompareStringOrdinal(absolute.c_str(),static_cast<int>(absolute.size()),actual.c_str(),static_cast<int>(actual.size()),TRUE)!=CSTR_EQUAL)return false;
        std::string bytes(expected.size(),'\0'); DWORD got=0;
        if(!ReadFile(handle.h,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr) || got!=bytes.size() || bytes!=expected)return false;
        FILE_DISPOSITION_INFO disposition{TRUE};
        return SetFileInformationByHandle(handle.h,FileDispositionInfo,&disposition,sizeof disposition)!=0;
#else
        std::error_code error;const auto absolute=std::filesystem::absolute(file,error).lexically_normal();
        if(error)return false;
        Fd parent{open(absolute.parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)};
        if(parent.fd<0)return false;
        char actual[4096];const std::string proc="/proc/self/fd/"+std::to_string(parent.fd);
        const auto length=readlink(proc.c_str(),actual,sizeof actual);
        if(length<=0 || size_t(length)>=sizeof actual || std::filesystem::path(std::string(actual,length))!=absolute.parent_path())return false;
        const auto name=absolute.filename().native();
        Fd handle{openat(parent.fd,name.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC)};
        struct stat before{},now{};
        if(handle.fd<0 || fstat(handle.fd,&before)!=0 || !S_ISREG(before.st_mode) || before.st_size<0 || uint64_t(before.st_size)!=expected.size())return false;
        std::string bytes(expected.size(),'\0');size_t got=0;
        while(got<bytes.size()){const auto n=read(handle.fd,bytes.data()+got,bytes.size()-got);if(n<=0)return false;got+=n;}
        if(bytes!=expected || fstatat(parent.fd,name.c_str(),&now,AT_SYMLINK_NOFOLLOW)!=0 ||
           now.st_dev!=before.st_dev || now.st_ino!=before.st_ino)return false;
        return unlinkat(parent.fd,name.c_str(),0)==0;
#endif
    } catch(...) { return false; }
}
}
