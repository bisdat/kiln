// SPDX-License-Identifier: MIT
#include "../capabilities.h"
#include <cassert>
#include <cstdio>
int main(){int checks=0;auto check=[&](bool b){assert(b);++checks;};
    auto a=kt::action_availability(false,false,KILN_SESSION_NONE,false);check(a.reason==KILN_CAP_REASON_OFFLINE&&a.state==KILN_CAP_UNAVAILABLE);
    a=kt::action_availability(true,false,KILN_SESSION_HOST,true);check(a.reason==KILN_CAP_REASON_SYMBOL_MISSING&&a.state==KILN_CAP_UNAVAILABLE);
    a=kt::action_availability(true,true,KILN_SESSION_REMOTE,false);check(a.reason==KILN_CAP_REASON_WRONG_SIDE&&a.state==KILN_CAP_UNAVAILABLE);
    a=kt::action_availability(true,true,KILN_SESSION_NONE,false);check(a.reason==KILN_CAP_REASON_SESSION&&a.state==KILN_CAP_PENDING);
    a=kt::action_availability(true,true,KILN_SESSION_HOST,false);check(a.reason==KILN_CAP_REASON_SESSION&&a.state==KILN_CAP_PENDING);
    a=kt::action_availability(true,true,KILN_SESSION_HOST,true);check(a.reason==KILN_CAP_REASON_NONE&&a.state==KILN_CAP_AVAILABLE);
    a=kt::action_availability(true,true,KILN_SESSION_DEDICATED,true);check(a.reason==KILN_CAP_REASON_NONE&&a.state==KILN_CAP_AVAILABLE);
    std::printf("%d tool capability checks passed\n",checks);
}
