#pragma once

#include "playerbot/strategy/Action.h"
#include "playerbot/LivingActivityRecovery.h"

namespace ai
{
    class SetCombatStateAction : public Action 
    {
    public:
        SetCombatStateAction(PlayerbotAI* ai, std::string name = "set combat state") : Action(ai, name) {}
        bool Execute(Event& event) override;
        LivingActivity::Effects GetActivityEffects() const override { return {LivingActivity::Mask(LivingActivity::Effect::Movement), LivingActivity::Lane::Combat, true}; }
        LivingActivity::NativePermit GetNativeActivityPermit(Event&) override { return LivingActivity::NativeCombatStatePermit(*ai, true); }
    };

    class SetNonCombatStateAction : public Action
    {
    public:
        SetNonCombatStateAction(PlayerbotAI* ai, std::string name = "set non combat state") : Action(ai, name) {}
        bool Execute(Event& event) override;
        LivingActivity::Effects GetActivityEffects() const override { return {LivingActivity::Mask(LivingActivity::Effect::Movement), LivingActivity::Lane::Combat, true}; }
        LivingActivity::NativePermit GetNativeActivityPermit(Event&) override {
            if (ai->IsStateActive(BotState::BOT_STATE_DEAD))
                return LivingActivity::NativeLifeStatePermit(*ai, LivingActivity::LifeTransition::Resurrected);
            return LivingActivity::NativeCombatStatePermit(*ai, false);
        }
    };

    class SetDeadStateAction : public Action
    {
    public:
        SetDeadStateAction(PlayerbotAI* ai, std::string name = "set dead state") : Action(ai, name) {}
        bool Execute(Event& event) override;
        LivingActivity::Effects GetActivityEffects() const override { return {LivingActivity::Mask(LivingActivity::Effect::Movement), LivingActivity::Lane::Safety, true}; }
        LivingActivity::NativePermit GetNativeActivityPermit(Event&) override { return LivingActivity::NativeLifeStatePermit(*ai, LivingActivity::LifeTransition::Died); }
    };
}
