#pragma once

#include "playerbot/strategy/Action.h"
#include "ChangeStrategyAction.h"
#include "playerbot/LivingActivityGameplay.h"

namespace ai
{
    class SwitchToMeleeAction : public ChangeCombatStrategyAction
    {
    public:
        SwitchToMeleeAction(PlayerbotAI* ai) : ChangeCombatStrategyAction(ai, "-ranged,+close") {}
        LivingActivity::Effects GetActivityEffects() const override { return {LivingActivity::AttackEffectMask(), LivingActivity::Lane::Managed, true}; }
        LivingActivity::NativePermit GetNativeActivityPermit(Event& event) override;
        bool isUseful() override;
        bool Execute(Event& event) override;
    };

    class SwitchToRangedAction : public ChangeCombatStrategyAction
    {
    public:
        SwitchToRangedAction(PlayerbotAI* ai) : ChangeCombatStrategyAction(ai, "-close,+ranged") {}
        LivingActivity::Effects GetActivityEffects() const override { return {LivingActivity::AttackEffectMask(), LivingActivity::Lane::Managed, true}; }
        LivingActivity::NativePermit GetNativeActivityPermit(Event& event) override;
        bool isUseful() override;
        bool Execute(Event& event) override;
    };
}
