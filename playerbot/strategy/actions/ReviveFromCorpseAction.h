#pragma once
#include "MovementActions.h"
#include "playerbot/LivingActivityRecovery.h"

namespace ai
{
	class ReviveFromCorpseAction : public MovementAction 
    {
	public:
		ReviveFromCorpseAction(PlayerbotAI* ai) : MovementAction(ai, "revive from corpse") {}
        virtual bool Execute(Event& event) override;
        LivingActivity::Effects GetActivityEffects() const override { return {LivingActivity::RecoveryEffects(), LivingActivity::Lane::Safety, true}; }
        LivingActivity::NativePermit GetNativeActivityPermit(Event&) override { return LivingActivity::NativeCorpseRecoveryPermit(*ai, LivingActivity::CorpseRecovery::Reclaim); }
    };

    class FindCorpseAction : public MovementAction 
    {
    public:
        FindCorpseAction(PlayerbotAI* ai) : MovementAction(ai, "find corpse") {}
        virtual bool Execute(Event& event) override;
        virtual bool isUseful() override;
        LivingActivity::Effects GetActivityEffects() const override { return {LivingActivity::Mask(LivingActivity::Effect::Movement), LivingActivity::Lane::Safety, true}; }
        LivingActivity::NativePermit GetNativeActivityPermit(Event&) override { return LivingActivity::NativeCorpseRecoveryPermit(*ai, LivingActivity::CorpseRecovery::Find); }
    };

	class SpiritHealerAction : public MovementAction 
    {
	public:
	    SpiritHealerAction(PlayerbotAI* ai, std::string name = "spirit healer") : MovementAction(ai,name) {}
        virtual bool Execute(Event& event) override;
        virtual bool isUseful() override;
    };
}
