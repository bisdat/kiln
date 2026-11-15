#define KILN_PENDING_TESTING
#include "services/kiln_capacity_util.hpp"
#include <iostream>
namespace fs = std::filesystem;
namespace cap = kiln::capacity;
int main() {
 auto base=fs::temp_directory_path()/"kiln-pending-boundary-test";
 fs::remove_all(base); fs::create_directories(base/"outside");
 int fails=0, checks=0;
 auto check=[&](bool ok,const char* name){++checks;if(!ok){++fails;std::cerr<<"FAIL "<<name<<'\n';}};
 auto root=base/"pending", file=root/"mod"/"a.json";
 check(cap::write_atomic(file,"old"),"ordinary write");
 for (auto part : {root,root/"mod"}) {
  fs::remove_all(root); fs::create_directories(part.parent_path());
  fs::create_directory_symlink(base/"outside",part);
  check(!cap::write_atomic(file,"bad"),"directory link refused");
  check(cap::pending_files(root).empty(),"directory link not enumerated");
 }
 fs::remove_all(root); fs::create_directories(file.parent_path());
 {std::ofstream out(base/"outside"/"victim");out<<"safe";}
 for(auto p:{file,fs::path(file.string()+".tmp")}) {
  fs::create_symlink(base/"outside"/"victim",p);
  check(!cap::write_atomic(file,"bad"),"file or tmp link refused");
  check(fs::is_symlink(p),"refused link left in place"); fs::remove(p); fs::remove(file);
 }
 std::ifstream in(base/"outside"/"victim");std::string text;in>>text;
 check(text=="safe","external file untouched");
 fs::create_symlink(base/"outside"/"victim",file);
 check(cap::pending_files(root).empty(),"file link not enumerated");
 fs::remove(file);
 {std::ofstream out(fs::path(file.string()+".tmp"));out<<"torn crash leftover";}
 cap::pending_files(root);
 check(!fs::exists(fs::path(file.string()+".tmp")),"enumeration removes regular crash leftovers without following links");
 for (const char* step : {"create","write","flush","rename","directory-flush"}) {
  check(cap::write_atomic(file,"old"),"seed durable state");
  cap::pending_io::fail_step=step;
  check(!cap::write_atomic(file,"new"),"injected I/O failure is not acknowledged");
  cap::pending_io::fail_step=nullptr;
  std::string value;check(cap::pending_io::read(file,&value,100)&&(value=="old"||value=="new"),"fault leaves whole old or new state");
 }
 for(const char* step : {"enumerate", "stat"}) {
  cap::pending_io::fail_step=step;
  cap::PendingScan unreadable;cap::pending_files_bounded(root,&unreadable);
  std::string error;
  bool rejected=!cap::pending_store_write(root,root/"mod"/"uninspected.json","x",&error);
  cap::pending_io::fail_step=nullptr;
  check(!unreadable.complete && rejected,"an inspection I/O failure cannot become an empty ready store or accept a write");
 }
 for(int i=0;i<4097;++i){std::ofstream out(root/"mod"/(std::to_string(i)+".json"));out<<"x";}
 cap::PendingScan scan;cap::pending_files_bounded(root,&scan);
 check(cap::pending_files(root).size()<=4096 && scan.unread>=1,"enumeration is capped and counts unread files");
 std::string reason;
 check(!cap::pending_store_write(root,root/"mod"/"new.json","x",&reason)&&reason.find("full")!=std::string::npos,"full store refuses acceptance with a reason");
 fs::remove_all(root);fs::create_directories(root/"mod");
 {std::ofstream out(file);out<<"x";}
 fs::resize_file(file,cap::pending_max_bytes);
 check(!cap::pending_store_write(root,root/"mod"/"new.json","x",&reason),"aggregate byte bound refuses acceptance");
 fs::remove_all(root);fs::create_directories(root/"mod");
 for(int i=0;i<16385;++i){std::ofstream out(root/"mod"/("bad@"+std::to_string(i)+".json"));out<<"x";}
 cap::PendingScan hostile;cap::pending_files_bounded(root,&hostile);
 check(!hostile.complete && hostile.unread>=1,"excess invalid entries stop enumeration instead of bypassing its work bound");
 fs::remove_all(base);
 std::cout<<"pending boundary: "<<checks<<" checks, "<<fails<<" failures\n";
 return fails?1:0;
}
