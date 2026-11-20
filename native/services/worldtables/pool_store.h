// SPDX-License-Identifier: MIT
#pragma once
#include "pool_registration.h"
namespace kwt {
// Process-start input only. Re-preparing a boot generation must never capture OnLoad's writes.
// The caller supplies the enabled/accepted boot profile before asking for any definitions.
struct PoolBootStore {
    struct File { kiln::capacity::PendingFile file; std::array<uint8_t,32> fingerprint; };
    bool captured=false;
    std::vector<File> files;
    void capture(const kiln::capacity::pending_io::TrustedBase& base) {
        if(captured)return;
        captured=true;
        kiln::capacity::PendingScan scan;
        const auto root=base.path/"pending";
        for(const auto& f:kiln::capacity::pending_files_bounded(base,root,&scan)) {
            std::string bytes;
            if(kiln::capacity::pending_io::read(base,f.path,&bytes,kiln::capacity::pending_max_file_bytes))
                files.push_back({f,kiln::capacity::sha256(bytes)});
        }
        if(!scan.complete)kiln::capacity::pending_io::refused(root,"unsafe: pending inspection incomplete; at least "+std::to_string(scan.unread)+" entries unread (4096 files / 268435456 bytes / bounded work)");
    }
    bool read(const kiln::capacity::pending_io::TrustedBase& base,const File& f,std::string* bytes) const {
        if(!kiln::capacity::pending_io::read(base,f.file.path,bytes,kiln::capacity::pending_max_file_bytes))return false;
        if(kiln::capacity::sha256(*bytes)==f.fingerprint)return true;
        kiln::capacity::pending_io::refused(f.file.path,"unsafe: changed after process boot; left for the next start");return false;
    }
    std::vector<PoolDefinition> replay(const kiln::capacity::pending_io::TrustedBase& base,const std::set<std::string>& loaded) const {
        std::vector<PoolDefinition> out;
        for(const auto& f:files) {
            if(f.file.name.rfind("@pool-",0)!=0 || f.file.path.extension()!=".json")continue;
            if(!loaded.count(f.file.mod))continue;  // kept, not refused: the service notes it once (kept_file_text)
            std::string bytes,why;PoolDefinition definition;
            if(!read(base,f,&bytes))continue;
            if(!pool_envelope(parse_relaxed(bytes),f.file.mod,&definition,&why) || f.file.name!="@pool-"+definition.guid.text()+".json") {
                kiln::capacity::pending_io::refused(f.file.path,"unsafe: torn/invalid pool pending envelope or filename: "+why);continue;
            }
            out.push_back(std::move(definition));
        }
        return out;
    }
};
} // namespace kwt
