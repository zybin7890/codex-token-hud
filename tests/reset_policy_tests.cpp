#include "codex_client.hpp"
#include <cstring>
#include <iostream>
int main() {
    FetchResult r; r.ok=true; r.reset_credit_count=2; r.snapshot.pool_count=1;
    auto& p=r.snapshot.pools[0]; std::strcpy(p.id,"codex"); p.primary.present=1;
    int checks=0;
    auto check=[&](bool expected){++checks; if(reset_allowed(r)!=expected){std::cerr<<"FAIL "<<checks<<'\n';return false;}return true;};
    p.primary.used_percent=98.99; if(!check(false))return 1;
    p.primary.used_percent=99; if(!check(true))return 1;
    p.primary.used_percent=100; if(!check(true))return 1;
    r.reset_credit_count=0; if(!check(false))return 1;
    r.reset_credit_count=-1; if(!check(false))return 1;
    r.reset_credit_count=2; r.ok=false; if(!check(false))return 1;
    r.ok=true; p.primary.present=0; if(!check(false))return 1;
    p.secondary.present=1; p.secondary.used_percent=99; if(!check(true))return 1;
    std::strcpy(p.id,"review"); if(!check(false))return 1;
    p.id[0]=0; if(!check(true))return 1;
    r.snapshot.pool_count=0; if(!check(false))return 1;
    std::cout<<"PASS: "<<checks<<" reset policy checks; no redemption RPC\n";
}
