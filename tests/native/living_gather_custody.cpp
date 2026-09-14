#include "LivingGatherCustody.h"
#include <cassert>
#include <thread>
#include <vector>
using namespace LivingActivity;
int main() {
    GatherCustody custody;
    assert(!custody.Begin(1,"task",0,2,3));
    assert(custody.Begin(1,"task",1,2,3));
    assert(!custody.Begin(1,"competitor",1,2,4));
    assert(custody.Holds(1,1,2,3,0)); // Native effect not finished yet.
    custody.Opened(1,"task",1,2,100);
    // No execution lease is supplied: cast receipt, lease release, pause and
    // revision advancement must not let legacy packet handling take this loot.
    assert(custody.Holds(1,1,2,3,100));
    assert(!custody.Holds(1,1,2,4,100));
    custody.Release(1,"other-task");assert(custody.Holds(1,1,2,3,100));
    assert(!custody.Holds(1,1,2,3,101)); // Respawn is not the reserved generation.
    assert(custody.Begin(1,"task",1,2,3));
    assert(!custody.Holds(1,1,3,3,0)); // Transfer revokes native object context.
    assert(custody.Begin(1,"task",2,3,3));
    custody.Release(1,"task",1,2); // An old cast callback cannot release a new context.
    assert(custody.Holds(1,2,3,3,0));
    custody.Release(1,"task");assert(!custody.Holds(1,2,3,3,0));
    std::vector<std::thread> workers;
    for(unsigned a=1;a<=8;++a)workers.emplace_back([&,a] {
        for(unsigned n=0;n<1000;++n) {
            assert(custody.Begin(a,"task",a,1,a));custody.Opened(a,"task",a,1,100);
            assert(custody.Holds(a,a,1,a,100));custody.Release(a,"task");
        }
    });
    for(auto& worker:workers)worker.join();
}
