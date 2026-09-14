#ifndef LIVING_ACTIVITY_TARGETING_H
#define LIVING_ACTIVITY_TARGETING_H
#include "LivingActivityGameplay.h"

namespace LivingActivity {
    // Selecting a replacement is part of an established native encounter, not
    // permission to pull a nearby creature. After an encounter, a dead/removed
    // cached target may be cleaned up; each child attack still validates its
    // exact target independently at AttackAction::Attack.
    template<class NativePlayer>
    bool ReadyForNativeTargetSelection(const NativePlayer& actor, bool engaged,
        bool cachedTarget, bool cachedTargetValid) {
        return actor.IsInWorld() && actor.IsAlive() && !actor.IsBeingTeleported() &&
            (engaged || (cachedTarget && !cachedTargetValid));
    }
    NativePermit NativeTargetSelectionPermit(PlayerbotAI& ai);
}
#endif
