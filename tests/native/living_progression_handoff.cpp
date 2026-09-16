#include "LivingProgressionRecovery.h"
#include <cassert>
using namespace LivingActivity;
int main() {
    // Regression reproduction: the old terminal predicate ignored safety and
    // ownership for PREPARE/NONE/COOLDOWN/EXPIRED. No native success is claimed.
    const bool oldExcluded=true, oldActive=true, oldPrepareTimeout=true;
    const bool oldTerminal=oldActive && oldPrepareTimeout;
    assert(oldExcluded && oldTerminal);
    for(unsigned reasons=1;reasons<16;++reasons) {
        assert(!RecoveryMayEndRoute(true,true,reasons&1,reasons&2,reasons&4,reasons&8));
        assert(RecoveryMayEndRoute(false,true,reasons&1,reasons&2,reasons&4,reasons&8));
        assert(!RecoveryMayEndRoute(false,false,reasons&1,reasons&2,reasons&4,reasons&8));
    }
    ExecutionAuthority authority;
    Task service;service.id=service.root="11111111-1111-4111-8111-111111111111";
    service.actor=497;service.source="profession";service.sourceKey=service.id;
    service.context.actor=497;service.context.boot="22222222-2222-4222-8222-222222222222";
    service.context.actorGeneration=service.context.mapGeneration=service.context.policyRevision=1;
    service.createdAtMs=service.updatedAtMs=100;service.mode=Mode::Active;
    service.phase=Phase::Traveling;service.priority=Priority::Delivery;
    authority.Observe(service.context,0);
    assert(!MovementCommitmentBlocksRecovery(authority.Read(497)));
    auto lease=authority.Acquire(service,Mask(Effect::Inventory),100,1000);
    assert(lease.Granted());
    assert(MovementCommitmentBlocksRecovery(authority.Read(497)));
    assert(authority.Inspect(497,1200).code==AuthorityCode::StaleLease);
    assert(MovementCommitmentBlocksRecovery(authority.Read(497)));
    Task human=service;human.id=human.root="33333333-3333-4333-8333-333333333333";
    human.source="human";human.sourceKey=human.id;human.priority=Priority::Human;
    auto party=authority.Acquire(human,Mask(Effect::Movement)|Mask(Effect::TravelTarget),1200,1000);
    assert(party.Granted());
    assert(authority.Release(lease.lease).code!=AuthorityCode::Released);
    assert(MovementCommitmentBlocksRecovery(authority.Read(497)));
    authority.Observe(human.context,uint32_t(Safety::Combat));
    assert(MovementCommitmentBlocksRecovery(authority.Read(497)));
    authority.Observe(human.context,0);
    assert(authority.Release(party.lease).code==AuthorityCode::Released);
    // Accepted tasks in the durable queue are not effective owners.
    assert(!MovementCommitmentBlocksRecovery(authority.Read(497)));
    RecoveryPauseClock clock;
    assert(UpdateRecoveryPause(clock,100,true)==0);
    assert(UpdateRecoveryPause(clock,10000,true)==0 && clock.since==100);
    assert(UpdateRecoveryPause(clock,10100,false)==10000 && !clock.paused);
    assert(UpdateRecoveryPause(clock,20100,false)==0);
    assert(UpdateRecoveryPause(clock,30000,true)==0);
    assert(UpdateRecoveryPause(clock,30100,false)==100);
}
