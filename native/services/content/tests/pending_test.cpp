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
 // The player's layout above the service data directory is trusted, including links.
 fs::create_directories(base/"layout"/"service");
 fs::create_directory_symlink(base/"layout",base/"linked-layout");
 auto via=base/"linked-layout"/"service"/"pending"/"mod"/"a.json";
 check(cap::write_atomic(via,"layout") && cap::pending_io::write_atomic(cap::pending_io::TrustedBase{via.parent_path().parent_path().parent_path()},via,"layout"),"trusted base reached through a linked ancestor works and creates missing store folders");
 std::string unavailable;
 cap::pending_io::diagnostic=[&](const auto&,const auto& reason){unavailable=reason;};
 {std::ofstream f(base/"not-a-directory"); f<<"x";}
 {cap::pending_io::Directory d;check(!d.open(base/"not-a-directory"),"unavailable base is refused");}
 check(unavailable.find("unavailable")!=std::string::npos && unavailable.find("code ")!=std::string::npos,
       "unavailable base reports the system error code rather than unsafe");
 fs::create_directories(base/"denied-base");
 fs::permissions(base/"denied-base",fs::perms::none);
 unavailable.clear();
 {cap::pending_io::Directory d;check(!d.open(base/"denied-base"),"unreadable trusted base is refused");}
 fs::permissions(base/"denied-base",fs::perms::owner_all);
 check(unavailable.find("unavailable")!=std::string::npos && unavailable.find("code 13")!=std::string::npos,
       "unreadable base reports EACCES rather than a link");
 {std::ofstream out(base/"unreadable-file");out<<"x";}
 fs::permissions(base/"unreadable-file",fs::perms::none);
 unavailable.clear();
 {cap::pending_io::Directory d;d.open(base);check(d.size("unreadable-file")==UINT64_MAX,"unreadable file cannot be counted as empty");}
 fs::permissions(base/"unreadable-file",fs::perms::owner_all);
 check(unavailable.find("unavailable")!=std::string::npos && unavailable.find("code 13")!=std::string::npos,
       "file inspection refusal reports its component and EACCES code");
 cap::pending_io::diagnostic={};

 check(cap::pending_io::write_atomic(cap::pending_io::TrustedBase{base},file,"old"),"ordinary write");
 for (auto part : {root,root/"mod"}) {
  fs::remove_all(root); fs::create_directories(part.parent_path());
  fs::create_directory_symlink(base/"outside",part);
  check(!cap::pending_io::write_atomic(cap::pending_io::TrustedBase{base},file,"bad"),"directory link refused");
  std::string ignored;
  check(!cap::pending_io::read(cap::pending_io::TrustedBase{base},file,&ignored,100) &&
        !cap::pending_io::remove(cap::pending_io::TrustedBase{base},file) && fs::is_symlink(part),"directory link blocks reads and removals and stays in place");
  check(cap::pending_files(cap::pending_io::TrustedBase{base},root).empty(),"directory link not enumerated");
 }
 fs::remove_all(root); fs::create_directories(file.parent_path());
 {std::ofstream out(base/"outside"/"victim");out<<"safe";}
 for(auto p:{file,fs::path(file.string()+".tmp")}) {
  fs::create_symlink(base/"outside"/"victim",p);
  check(!cap::pending_io::write_atomic(cap::pending_io::TrustedBase{base},file,"bad"),"file or tmp link refused");
  check(fs::is_symlink(p),"refused link left in place");
  if(p==file) {std::string ignored;check(!cap::pending_io::read(cap::pending_io::TrustedBase{base},file,&ignored,100) &&
      !cap::pending_io::remove(cap::pending_io::TrustedBase{base},file) && fs::is_symlink(file),"file link blocks reads and removals");}
  fs::remove(p); fs::remove(file);
 }
 std::ifstream in(base/"outside"/"victim");std::string text;in>>text;
 check(text=="safe","external file untouched");
 fs::create_symlink(base/"outside"/"victim",file);
 check(cap::pending_files(cap::pending_io::TrustedBase{base},root).empty(),"file link not enumerated");
 fs::remove(file);
 {std::ofstream out(fs::path(file.string()+".tmp"));out<<"torn crash leftover";}
 cap::pending_files(cap::pending_io::TrustedBase{base},root);
 check(!fs::exists(fs::path(file.string()+".tmp")),"enumeration removes regular crash leftovers without following links");
 for (const char* step : {"create","write","flush","rename","directory-flush"}) {
  check(cap::pending_io::write_atomic(cap::pending_io::TrustedBase{base},file,"old"),"seed durable state");
  cap::pending_io::fail_step=step;
  check(!cap::pending_io::write_atomic(cap::pending_io::TrustedBase{base},file,"new"),"injected I/O failure is not acknowledged");
  cap::pending_io::fail_step=nullptr;
  std::string value;check(cap::pending_io::read(cap::pending_io::TrustedBase{base},file,&value,100)&&(value=="old"||value=="new"),"fault leaves whole old or new state");
 }
 for(const char* step : {"enumerate", "stat"}) {
  cap::pending_io::fail_step=step;
  cap::PendingScan unreadable;cap::pending_files_bounded(cap::pending_io::TrustedBase{base},root,&unreadable);
  std::string error;
  bool rejected=!cap::pending_store_write(cap::pending_io::TrustedBase{base},root,root/"mod"/"uninspected.json","x",&error);
  cap::pending_io::fail_step=nullptr;
  check(!unreadable.complete && rejected,"an inspection I/O failure cannot become an empty ready store or accept a write");
 }
 for(int i=0;i<4097;++i){std::ofstream out(root/"mod"/(std::to_string(i)+".json"));out<<"x";}
 cap::PendingScan scan;cap::pending_files_bounded(cap::pending_io::TrustedBase{base},root,&scan);
 check(cap::pending_files(cap::pending_io::TrustedBase{base},root).size()<=4096 && scan.unread>=1,"enumeration is capped and counts unread files");
 std::string reason;
 check(!cap::pending_store_write(cap::pending_io::TrustedBase{base},root,root/"mod"/"new.json","x",&reason)&&reason.find("full")!=std::string::npos,"full store refuses acceptance with a reason");
 fs::remove_all(root);fs::create_directories(root/"mod");
 {std::ofstream out(file);out<<"x";}
 fs::resize_file(file,cap::pending_max_bytes);
 check(!cap::pending_store_write(cap::pending_io::TrustedBase{base},root,root/"mod"/"new.json","x",&reason),"aggregate byte bound refuses acceptance");
 fs::remove_all(root);fs::create_directories(root/"mod");
 for(int i=0;i<16385;++i){std::ofstream out(root/"mod"/("bad@"+std::to_string(i)+".json"));out<<"x";}
 cap::PendingScan hostile;cap::pending_files_bounded(cap::pending_io::TrustedBase{base},root,&hostile);
 check(!hostile.complete && hostile.unread>=1,"excess invalid entries stop enumeration instead of bypassing its work bound");
 // A store that was never written: the ordinary state of every installation. Nothing is refused and nothing is logged.
 {
  fs::remove_all(base); fs::create_directories(base);
  int said=0; std::string what;
  cap::pending_io::diagnostic=[&](const auto& p,const auto& reason){++said;what=p.string()+": "+reason;};
  const auto service=base/"never"/"service";   // the trusted base itself does not exist yet
  cap::PendingScan fresh;
  check(cap::pending_files_bounded(cap::pending_io::TrustedBase{service},service/"pending",&fresh).empty() && fresh.complete && fresh.files==0,
        "a trusted base that does not exist is an empty, complete store");
  fs::create_directories(service);
  cap::PendingScan no_root;
  check(cap::pending_files_bounded(cap::pending_io::TrustedBase{service},service/"pending",&no_root).empty() && no_root.complete,
        "a store root that does not exist is an empty, complete store");
  std::string none;
  check(!cap::pending_io::read(cap::pending_io::TrustedBase{service},service/"pending"/"mod"/"a.json",&none,100),"a file in a folder that does not exist is not there");
  check(said==0,"a store that was never written says nothing");
  if(said) std::cerr<<"  said: "<<what<<'\n';
  std::string why;
  check(cap::pending_store_write(cap::pending_io::TrustedBase{service},service/"pending",service/"pending"/"mod"/"a.json","first",&why),"the first write creates the store");
  check(said==0,"the first write into a fresh store says nothing");
  if(said) std::cerr<<"  said: "<<what<<'\n';
  cap::PendingScan after;
  check(cap::pending_files_bounded(cap::pending_io::TrustedBase{service},service/"pending",&after).size()==1 && after.complete,"and the store then holds the one file");
  // a link where the store root should be is still a refusal, with its line
  fs::remove_all(service/"pending"); fs::create_directories(base/"elsewhere"); fs::create_directory_symlink(base/"elsewhere",service/"pending");
  cap::PendingScan linked; cap::pending_files_bounded(cap::pending_io::TrustedBase{service},service/"pending",&linked);
  check(!linked.complete && said==1,"a link at the store root is refused and named, not read as an empty store");
  cap::pending_io::diagnostic={};
 }
 fs::remove_all(base);
 std::cout<<"pending boundary: "<<checks<<" checks, "<<fails<<" failures\n";
 return fails?1:0;
}
