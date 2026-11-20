// Execute the production ABI status entry; count expensive resolver copies.
#include <cstdint>
#include <string>
#include <string_view>
#include <cstdio>
#define KILN_CALL
#define KILN_GUARD(name, fallback, ...) __VA_ARGS__
using KilnResult = int;
constexpr int KILN_OK=0, KILN_ERR_INVALID_ARGUMENT=1, KILN_ERR_INTERNAL=2,
 KILN_ERR_UNAVAILABLE=3, KILN_ERR_NOT_FOUND=4, KILN_HOOKS_ERR_SIG_MISMATCH=100, KILN_HOOKS_ERR_SIG_UNKNOWN=101;
struct KilnStr { const char* ptr; size_t len; };
std::string_view sv(KilnStr s) { return {s.ptr,s.len}; }
std::string& typed_refusal_text() { static std::string s; return s; }
namespace sigdb {
enum class SigStatus { Match, Mismatch, Unknown };
struct SigInfo { uintptr_t address; SigStatus status; std::string text; };
uint64_t revision=1;
unsigned copies=0;
bool racing=false;
uint64_t sig_revision() { return revision; }
bool sig_status(std::string_view, uintptr_t* a, SigStatus* s, std::string* text) {
 *a=revision*16; *s=revision==1 ? SigStatus::Match : SigStatus::Mismatch; *text=revision==1 ? "" : "changed";
 if (racing) ++revision;
 return true;
}
bool sig(std::string_view name, SigInfo* info) {
 ++copies; return sig_status(name,&info->address,&info->status,&info->text);
}
}
#include "hooks_status.g.h"
int main() {
 uintptr_t a=0; uint64_t r=0; unsigned fail=0;
 KilnStr name{"test/symbol",11};
 for (unsigned i=0;i<100;++i) if(h_sig_status(name,&a,&r)!=KILN_OK || a!=16 || r!=1) ++fail;
 if(sigdb::copies) { std::printf("FAIL: %u full descriptor copies for 100 calls\n",sigdb::copies); ++fail; }
 ++sigdb::revision;
 if(h_sig_status(name,&a,&r)!=100 || a!=32 || r!=2 || typed_refusal_text()!="changed") ++fail;
 sigdb::racing=true;
 if(h_sig_status(name,&a,&r)!=KILN_ERR_UNAVAILABLE) ++fail;
 std::printf("hooks_status_test: 103 checks, %u failures\n",fail);
 return fail!=0;
}
