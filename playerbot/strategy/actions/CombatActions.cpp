
#include "playerbot/playerbot.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/strategy/values/LastMovementValue.h"
#include "CombatActions.h"
#include "playerbot/LivingActivityCoordinator.h"
#include "playerbot/LivingActivityScope.h"

using namespace ai;

LivingActivity::NativePermit SwitchToMeleeAction::GetNativeActivityPermit(Event&)
{
    return LivingActivity::NativeEngagedAttackPermit(*ai, AI_VALUE(Unit*, "current target"));
}

LivingActivity::NativePermit SwitchToRangedAction::GetNativeActivityPermit(Event&)
{
    return LivingActivity::NativeEngagedAttackPermit(*ai, AI_VALUE(Unit*, "current target"));
}

bool SwitchToMeleeAction::isUseful()
{
    return ai->HasStrategy("ranged", BotState::BOT_STATE_COMBAT);
}

bool SwitchToMeleeAction::Execute(Event&)
{
    if (Unit* target = AI_VALUE(Unit*, "current target"))
    {
        const auto permit = LivingActivity::NativeEngagedAttackPermit(*ai, target);
        std::unique_ptr<LivingActivity::ExecutionScope> nativeScope;
        if (permit.validated) nativeScope.reset(new LivingActivity::ExecutionScope(permit));
        const LivingActivity::Effects effects{LivingActivity::AttackEffectMask(),
            permit.validated ? permit.lane : LivingActivity::Lane::Managed, true};
        if (!sLivingActivityCoordinator.PermitEffects(*ai, effects, "native melee posture")) return false;
        bot->MeleeAttackStart(target);
        // Only this typed posture. Do not let an event parameter turn an
        // attack permit into arbitrary strategy changes or database writes.
        ai->ChangeStrategy("-ranged,+close", BotState::BOT_STATE_COMBAT);
        return true;
    }
    return false;
}

bool SwitchToRangedAction::isUseful()
{

    return ai->HasStrategy("close", BotState::BOT_STATE_COMBAT);
}

bool SwitchToRangedAction::Execute(Event&)
{
    if (Unit* target = AI_VALUE(Unit*, "current target"))
    {
        const auto permit = LivingActivity::NativeEngagedAttackPermit(*ai, target);
        std::unique_ptr<LivingActivity::ExecutionScope> nativeScope;
        if (permit.validated) nativeScope.reset(new LivingActivity::ExecutionScope(permit));
        const LivingActivity::Effects effects{LivingActivity::AttackEffectMask(),
            permit.validated ? permit.lane : LivingActivity::Lane::Managed, true};
        if (!sLivingActivityCoordinator.PermitEffects(*ai, effects, "native ranged posture")) return false;
        bot->MeleeAttackStop(target);
        ai->ChangeStrategy("-close,+ranged", BotState::BOT_STATE_COMBAT);
        return true;
    }
    return false;
}
