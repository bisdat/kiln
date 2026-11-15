// SPDX-License-Identifier: MIT
// Real native apply-report serialization, with no game or mutation. The managed consumer has 64 KiB.
#include "../packs.cpp"
#include <iostream>
namespace kc { std::vector<Conflict> conflicts; std::vector<Conflict> store_conflicts() {return conflicts;} }
int main() {
    int checks=0,failed=0;
    auto check=[&](bool ok,const char* name){++checks;if(!ok){++failed;std::cerr<<"FAIL "<<name<<'\n';}};
    kc::ApplyResult r;r.applied=4096;
    for(int i=0;i<4096;++i)r.warnings.push_back("buff CapsBuff_"+std::to_string(i)+": defined unchanged; the existing cloned record is reused");
    auto text=kc::apply_result_json(r);
    auto j=kc::json::parse(text.substr(0,65535),nullptr,false);
    check(text.size()<65536 && j.is_object() && j["applied"]==4096 && j["errors"].empty(),"4096-clone success fits the client buffer and remains successful");
    r.errors.push_back("CapsBuff_4095: CapacityExceeded: no added-buff room");
    text=kc::apply_result_json(r);j=kc::json::parse(text.substr(0,65535),nullptr,false);
    check(j.is_object() && j.contains("errors") && j["errors"].size()==1 && j["errors"][0]==r.errors[0],"one refusal among 4096 entries is preserved with its reason");
    r.errors.assign(4096,std::string(2048,'x')+"☃");
    text=kc::apply_result_json(r);j=kc::json::parse(text.substr(0,65535),nullptr,false);
    check(text.size()<65536 && j.is_object() && j["errorCount"]==4096 && !j["errors"].empty() && j["more"]>0,"even huge refusal text has a bounded well-formed summary with exact outcome counts");
    kc::Conflict c; c.what=c.winner=c.loser=c.winner_value=c.loser_value=std::string(512,'\\');
    kc::conflicts.assign(4096,c);r.errors.assign(4096,std::string(512,'\\'));r.warnings=r.errors;
    text=kc::apply_result_json(r);j=kc::json::parse(text.substr(0,65535),nullptr,false);
    check(text.size()<65536 && j.is_object() && j["conflictCount"]==4096 && j["more"]==3*(4096-8),"worst escaped messages in all outcome lists fit and account for omissions");
    r.errors={std::string(127,'x')+"☃"+std::string(2048,'x')};r.warnings.clear();kc::conflicts.clear();
    text=kc::apply_result_json(r);j=kc::json::parse(text,nullptr,false);
    check(j.is_object() && !j["errors"].empty(),"a truncated UTF-8 diagnostic stays valid JSON");
    std::cout<<"apply report: "<<checks<<" checks, "<<failed<<" failures\n";return failed?1:0;
}
