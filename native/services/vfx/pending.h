// SPDX-License-Identifier: MIT
// VFX-local replay bounds. Does not replace the shared pending writer.
#pragma once
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>
namespace kvx_pending {
constexpr size_t kFiles = 2048 - 1575;
constexpr size_t kFileBytes = 65536; // the existing content pending reader's file bound
constexpr size_t kEntries = kFiles * 4;
struct Budget { size_t files=0, entries=0; uint64_t bytes=0, byte_limit=uint64_t{kFiles}*kFileBytes; std::vector<std::string> skipped; };
template<class Read, class Reject>
auto read(const std::filesystem::path& root,const std::filesystem::path& folder,Budget& budget,Read reader,Reject reject) {
    namespace fs=std::filesystem;
    std::vector<std::pair<std::string,std::string>> out;
    const auto name=folder.filename().u8string();
    if(std::find(budget.skipped.begin(),budget.skipped.end(),std::string(name.begin(),name.end()))!=budget.skipped.end()) return out;
    std::error_code ec;
    for(const auto& p : {root,folder}) {
        auto status=fs::symlink_status(p,ec);
        if(ec==std::errc::no_such_file_or_directory || status.type()==fs::file_type::not_found) return out;
        if(ec || fs::is_symlink(status) || !fs::is_directory(status)) { reject(p,"not a readable directory without links"); return out; }
    }
    std::vector<fs::path> files;
    fs::directory_iterator it(folder,ec),end;
    for(;!ec && it!=end;it.increment(ec)) {
        const auto path=it->path();
        if(budget.entries>=kEntries) {reject(path,"pending entry limit reached; remaining entries left unread");break;}
        ++budget.entries;
        std::error_code error;auto status=it->symlink_status(error);
        if(error || fs::is_symlink(status)) {reject(path,"link or uninspectable entry");continue;}
        if(path.extension()!=L".json") continue;
        if(!fs::is_regular_file(status)){reject(path,"not a regular file");continue;}
        if(budget.files+files.size()>=kFiles){reject(path,"pending file limit reached; remaining entries left unread");break;}
        files.push_back(path);
    }
    if(ec) {reject(folder,"pending directory enumeration failed");return out;}
    std::sort(files.begin(),files.end());
    for(const auto& p:files) {
        ++budget.files; // refused files consume inspection allowance too
        if(budget.bytes>=budget.byte_limit){reject(p,"pending total byte limit reached");continue;}
        const auto max=static_cast<size_t>(std::min<uint64_t>(kFileBytes,budget.byte_limit-budget.bytes));
        std::string text,why;
        if(!reader(p,max,&text,&why)){reject(p,why);continue;}
        if(text.size()>max){reject(p,"pending reader exceeded its byte bound");continue;}
        budget.bytes+=text.size();
        const auto name=p.filename().u8string();
        out.emplace_back(std::string(name.begin(),name.end()),std::move(text));
    }
    return out;
}
}
