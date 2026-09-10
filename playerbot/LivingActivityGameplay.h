#ifndef LIVING_ACTIVITY_GAMEPLAY_H
#define LIVING_ACTIVITY_GAMEPLAY_H
namespace LivingActivity {
    // Native roll ownership, not the cached 'active rolls' flag, establishes
    // eligibility. World context, effect limits and atomic-operation exclusion
    // are separately checked at the common permission boundary.
    template<class NativePlayer, class NativeRoll, class Vote>
    bool ReadyForNativeLootVote(NativePlayer& actor, NativeRoll* roll, Vote pending, bool humanWait) {
        return actor.IsInWorld() && !actor.IsBeingTeleported() && actor.GetGroup() && roll &&
            roll->GetPlayerVote(actor.GetObjectGuid()) == pending && !humanWait;
    }
}
#endif
