#ifndef LIVING_PROGRESSION_RECOVERY_H
#define LIVING_PROGRESSION_RECOVERY_H
#include "LivingActivityAuthority.h"

namespace LivingActivity {
// This is a watchdog gate, not another activity owner or task scheduler.
inline bool MovementCommitmentBlocksRecovery(const AuthoritySnapshot& owner) {
    // Match the common native action boundary: an expired but still held lease
    // is not permission for the watchdog to mutate or charge a route failure.
    // Even an inventory-only atomic step owns its commitment's execution.
    return owner.commitmentEffects || owner.lease.actor || owner.invalidated || owner.compatibility ||
        !owner.operation.empty() || owner.operationExecuting || owner.operationDispatched;
}
struct RecoveryPauseClock { uint64_t since=0;bool paused=false; };
inline uint64_t UpdateRecoveryPause(RecoveryPauseClock& clock,uint64_t now,bool blocked) {
    if(blocked) {
        if(!clock.paused) {clock.since=now;clock.paused=true;}
        return 0;
    }
    if(!clock.paused)return 0;
    const auto elapsed=now>=clock.since?now-clock.since:0;
    clock={};return elapsed;
}
inline bool RecoveryMayEndRoute(bool blocked,bool active,bool terminalStatus,
    bool preparationTimeout,bool movementTimeout,bool gameplayTimeout) {
    return !blocked && active && (terminalStatus || preparationTimeout || movementTimeout || gameplayTimeout);
}
}
#endif
