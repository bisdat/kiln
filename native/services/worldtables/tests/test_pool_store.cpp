// SPDX-License-Identifier: MIT
#include "../pool_store.h"
#include <iostream>
#include <unistd.h>
using namespace kwt;
int main() {
 int checks=0,failed=0;auto check=[&](const char* name,bool ok){++checks;if(!ok)++failed;std::cout<<(ok?"PASS ":"FAIL ")<<name<<'\n';};
 namespace fs=std::filesystem;auto root=fs::temp_directory_path()/("kiln-pool-store-"+std::to_string(getpid()));fs::remove_all(root);
 kiln::capacity::pending_io::TrustedBase base{root};std::vector<std::string> errors;
 kiln::capacity::pending_io::diagnostic=[&](const auto&,const auto& why){errors.push_back(why);};
 PoolBootStore empty;empty.capture(base);check("missing store is silent",empty.files.empty() && errors.empty());
 ModFile mf;mf.mod="p";mf.clients="mod";read_blocks(parse_relaxed(R"({"buildingMaterialPools":{"walls":{"name":"Walls","materials":[{"gameMaterial":"WoodRough"}]}}})"),&mf);
 PoolRegistrations ledger;auto receipt=ledger.prepare(mf.pools.at(0),KILN_DEFINE_ALLOW_NEXT_START,false);auto path=root/"pending/p"/("@pool-"+receipt.definition.guid.text()+".json");std::string reason;
 check("durable write",kiln::capacity::pending_store_write(base,root/"pending",path,receipt.envelope(),&reason));
 check("same process file waits",empty.replay(base,{"p"}).empty());
 PoolBootStore boot;boot.capture(base);check("inactive mod not applied",boot.replay(base,{}).empty());check("inactive file retained",fs::exists(path));
 auto rows=boot.replay(base,{"p"});check("loaded mod replays exactly one",rows.size()==1 && rows[0].definition==receipt.definition.definition);
 auto wrong=parse_relaxed(receipt.envelope());wrong["format"]="1";PoolDefinition def;check("wrong envelope type refused",!pool_envelope(wrong,"p",&def,&reason));
 check("oversize refused on write",!kiln::capacity::pending_store_write(base,root/"pending",path,std::string(65537,'x'),&reason));
 auto newer=receipt;newer.definition.definition+=" ";check("changed file write",kiln::capacity::pending_store_write(base,root/"pending",path,newer.envelope()+" ",&reason));
 check("changed boot fingerprint waits",boot.replay(base,{"p"}).empty());
 check("torn file write",kiln::capacity::pending_store_write(base,root/"pending",path,"{",&reason));PoolBootStore torn;torn.capture(base);check("torn never replays",torn.replay(base,{"p"}).empty() && fs::exists(path));
 fs::remove(path);auto bad=path.parent_path()/"@pool-wrong.json";kiln::capacity::pending_store_write(base,root/"pending",bad,receipt.envelope(),&reason);PoolBootStore names;names.capture(base);check("wrong filename refused",names.replay(base,{"p"}).empty());
 fs::remove(bad);fs::remove_all(root/"pending/p");fs::create_directory_symlink(root,root/"pending/p");check("link write refused",!kiln::capacity::pending_store_write(base,root/"pending",path,receipt.envelope(),&reason));
 fs::remove_all(root);std::cout<<checks<<" checks, "<<failed<<" failures\n";return failed?1:0;
}
