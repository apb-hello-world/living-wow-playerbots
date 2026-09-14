#ifndef LIVING_ACTIVITY_RECOVERY_H
#define LIVING_ACTIVITY_RECOVERY_H
#include "LivingActivityEffects.h"
class PlayerbotAI;
namespace LivingActivity {
    enum class LifeTransition { None, Died, Resurrected };
    // A native life transition is not a planner choice or a resurrection request.
    // Restart, missing class strategies and a pending profession receipt must not
    // leave an alive/ghost actor executing the wrong engine indefinitely.
    template<class NativePlayer>
    LifeTransition RequiredLifeTransition(const NativePlayer& actor, bool deadEngine) {
        if (!actor.IsInWorld() || actor.IsBeingTeleported()) return LifeTransition::None;
        if (!actor.IsAlive() && !deadEngine) return LifeTransition::Died;
        if (actor.IsAlive() && deadEngine) return LifeTransition::Resurrected;
        return LifeTransition::None;
    }
    enum class CorpseRecovery { Release, Find, Reclaim };
    template<class NativePlayer>
    bool ReadyForCorpseRecovery(const NativePlayer& actor, CorpseRecovery step,
        bool corpse, bool ghost, bool battleground) {
        if (!actor.IsInWorld() || actor.IsBeingTeleported() || actor.IsAlive()) return false;
        if (step == CorpseRecovery::Release) return !corpse && !ghost;
        return corpse && ghost && !battleground;
    }
    constexpr uint32_t RecoveryEffects() { return Mask(Effect::Movement) | Mask(Effect::Spell); }
    NativePermit NativeLifeStatePermit(PlayerbotAI& ai, LifeTransition transition);
    NativePermit NativeCombatStatePermit(PlayerbotAI& ai, bool enterCombat);
    NativePermit NativeCorpseRecoveryPermit(PlayerbotAI& ai, CorpseRecovery step);
    void ReconcileNativeLifeState(PlayerbotAI& ai);
}
#endif
