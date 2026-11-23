// SPDX-License-Identifier: MIT
#include "../bootstrap.h"
#include <cassert>
#include <cstdio>
#include <vector>
namespace {
std::vector<int> order;bool drain=true;int failing=-1;
bool start0(const KilnApi*,KilnOwner){order.push_back(0);return failing!=0;}
bool start1(const KilnApi*,KilnOwner){order.push_back(1);return failing!=1;}
bool start2(const KilnApi*,KilnOwner){order.push_back(2);return failing!=2;}
void stop0(){order.push_back(10);}void stop1(){order.push_back(11);}void stop2(){order.push_back(12);}
bool close(){order.push_back(99);return drain;}
}
int main(){
    kt::ToolPackage packages[]={{start0,stop0},{start1,stop1},{start2,stop2}};int checks=0;
    auto check=[&](bool value){assert(value);++checks;};
    check(kt::install_packages(nullptr,1,packages));check(order==std::vector<int>({0,1,2}));
    order.clear();check(kt::uninstall_packages(packages,close));check(order==std::vector<int>({99,12,11,10}));
    order.clear();failing=1;check(!kt::install_packages(nullptr,1,packages));check(order==std::vector<int>({0,1,2}));
    order.clear();drain=false;check(!kt::uninstall_packages(packages,close));check(order==std::vector<int>({99}));
    std::printf("%d bootstrap lifecycle checks passed\n",checks);
}
