#pragma once
#include "GenericActions.h"
#include "playerbot/LivingTrainingLesson.h"

namespace ai
{
	class TrainerAction : public ChatCommandAction
    {
	public:
		TrainerAction(PlayerbotAI* ai) : ChatCommandAction(ai, "trainer") {}
        virtual bool Execute(Event& event) override;
        LivingActivity::Effects GetActivityEffects() const override {
            using namespace LivingActivity;
            return {Mask(Effect::Spell) | Mask(Effect::Money) | Mask(Effect::Social), Lane::Managed, true};
        }

    private:
        typedef LivingActivity::TrainingLessonOutcome (TrainerAction::*TrainerSpellAction)(uint32, ObjectGuid trainerGuid, uint32 spellId, TrainerSpell const*, std::ostringstream& msg);
        bool Iterate(Player* requester, Creature* creature, TrainerSpellAction action, SpellIds& spells);
        LivingActivity::TrainingLessonOutcome Learn(uint32 cost, ObjectGuid trainerGuid, uint32 spellId, TrainerSpell const* tSpell, std::ostringstream& msg);
        void TellHeader(Player* requester, Creature* creature);
        void TellFooter(Player* requester, uint32 totalCost);
    };
}
