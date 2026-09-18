
#include "playerbot/playerbot.h"
#include "playerbot/LivingActivityCoordinator.h"
#include "playerbot/PlayerbotServiceTracking.h"
#include "TrainerAction.h"
#include "playerbot/PlayerbotTraining.h"
#include "playerbot/PlayerbotTrainingLesson.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/values/BudgetValues.h"

using namespace ai;

LivingActivity::TrainingLessonOutcome TrainerAction::Learn(uint32, ObjectGuid trainerGuid, uint32 spellId, TrainerSpell const*, std::ostringstream& msg)
{
    const auto result=LivingActivity::ExecuteNativeTrainingLesson(*ai,trainerGuid,spellId);
    if(result.outcome==LivingActivity::TrainingLessonOutcome::Verified)msg << " - learned";
    else if(result.reason=="training_unreserved_money_shortfall")msg << " - too expensive";
    else if(result.outcome==LivingActivity::TrainingLessonOutcome::Uncertain)msg << " - learning outcome not yet verified";
    else msg << " - cannot learn right now";
    return result.outcome;
}

bool TrainerAction::Iterate(Player* requester, Creature* creature, TrainerSpellAction action, SpellIds& spells)
{
    bool hasHeader = false;    
    bool hasTrainable = false;
    bool learnedAny = false;
    bool uncertain = false;

    TrainerSpellData const* cSpells = creature->GetTrainerSpells();
    TrainerSpellData const* tSpells = creature->GetTrainerTemplateSpells();
    float fDiscountMod =  bot->GetReputationPriceDiscount(creature);
    uint32 totalCost = 0;

    TrainerSpellMap trainer_spells;
    if (cSpells)
        trainer_spells.insert(cSpells->spellList.begin(), cSpells->spellList.end());
    if (tSpells)
        trainer_spells.insert(tSpells->spellList.begin(), tSpells->spellList.end());

    for (TrainerSpellMap::const_iterator itr =  trainer_spells.begin(); itr !=  trainer_spells.end(); ++itr)
    {
        TrainerSpell const* tSpell = &itr->second;

        if (!tSpell)
            continue;

        if (!LivingWowCanTrainSpell(bot, tSpell, creature))
            continue;

        uint32 spellId = tSpell->spell;
        const SpellEntry *const pSpellInfo =  sServerFacade.LookupSpellInfo(spellId);
        if (!pSpellInfo)
            continue;

#ifdef MANGOSBOT_ZERO
        if (tSpell->learnedSpell)
        {
            bool learned = true;
            if (bot->HasSpell(tSpell->learnedSpell))
            {
                learned = false;
            }
            else
            {
                for (int j = 0; j < 3; ++j)
                {
                    if (pSpellInfo->Effect[j] == SPELL_EFFECT_LEARN_SPELL)
                    {
                        learned = false;
                        uint32 learnedSpell = pSpellInfo->EffectTriggerSpell[j];

                        if (!bot->HasSpell(learnedSpell))
                        {
                            learned = true;
                            hasTrainable = true;
                            break;
                        }
                    }
                }
            }

            if (!learned)
                continue;
        }
#else
        if (!tSpell->learnedSpell.empty())
        {
            bool anySpellLearned = false;
            for (auto& learnedSpell : tSpell->learnedSpell)
            {
                bool learned = true;
                if (bot->HasSpell(learnedSpell))
                {
                    learned = false;
                }
                else
                {
                    for (int j = 0; j < 3; ++j)
                    {
                        if (pSpellInfo->Effect[j] == SPELL_EFFECT_LEARN_SPELL)
                        {
                            learned = false;
                            uint32 learnedSpell = pSpellInfo->EffectTriggerSpell[j];

                            if (!bot->HasSpell(learnedSpell))
                            {
                                learned = true;
                                hasTrainable = true;
                                break;
                            }
                        }
                    }
                }
                if (learned)
                    anySpellLearned = true;
            }
            if (!anySpellLearned)
                continue;
        }
#endif

        if (!spells.empty() && spells.find(tSpell->spell) == spells.end())
            continue;

        hasTrainable = true; // Directly taught spells also count as eligible.
        uint32 cost = LivingWowFreeBotTraining(bot) ? 0 : uint32(floor(tSpell->spellCost * fDiscountMod));
        totalCost += cost;

        std::ostringstream out;
        out << chat->formatSpell(pSpellInfo) << chat->formatMoney(cost);

        if (action)
        {
            const auto outcome=(this->*action)(cost, creature->GetObjectGuid(), itr->first, tSpell, out);
            learnedAny |= outcome==LivingActivity::TrainingLessonOutcome::Verified;
            uncertain = outcome==LivingActivity::TrainingLessonOutcome::Uncertain;
        }

        if (!hasHeader)
        {
            TellHeader(requester, creature);
            hasHeader = true;
        }
        ai->TellPlayer(requester, out, PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
        if(uncertain)break; // Do not repeat a partially observed native lesson.
    }

    if(hasHeader)
        TellFooter(requester, totalCost);
    else if (!ai->GetMaster() || sServerFacade.GetDistance2d(bot, ai->GetMaster()) < sPlayerbotAIConfig.reactDistance || ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT))
        ai->TellPlayerNoFacing(requester, "No spells can be learned from this trainer");

    return action ? learnedAny && !uncertain : hasTrainable;
}

bool TrainerAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    std::string text = event.getParam();
    const bool partyTraining = event.getSource() == "living party training";
    Creature* creature = nullptr;

    if (event.getSource() == "rpg action" || partyTraining)
    {
        ObjectGuid guid = event.getObject();
        creature = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_TRAINER);
    }
    else
    {
        if (requester)
            creature = ai->GetCreature(requester->GetSelectionGuid());
        else
            return false;
    }

#ifdef MANGOS
    if (!creature || !creature->IsTrainer())
#endif
#ifdef CMANGOS
    if (!creature || !creature->isTrainer())
#endif
        return false;       
            
    if (partyTraining && (creature->GetCreatureInfo()->TrainerType != TRAINER_TYPE_CLASS ||
        creature->GetCreatureInfo()->TrainerClass != bot->getClass())) return false;

    if (!creature->IsTrainerOf(bot, false))
    {
        if (!ai->GetMaster() || sServerFacade.GetDistance2d(bot, ai->GetMaster()) < sPlayerbotAIConfig.reactDistance || ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT))
            ai->TellPlayerNoFacing(requester, "This trainer cannot teach me");
        return false;
    }

    // check present spell in trainer spell list
    TrainerSpellData const* cSpells = creature->GetTrainerSpells();
    TrainerSpellData const* tSpells = creature->GetTrainerTemplateSpells();
    if (!cSpells && !tSpells)
    {
        if (!ai->GetMaster() || sServerFacade.GetDistance2d(bot, ai->GetMaster()) < sPlayerbotAIConfig.reactDistance || ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT))
            ai->TellPlayerNoFacing(requester, "No spells can be learned from this trainer");
        return false;
    }

    uint32 spell = chat->parseSpell(text);
    SpellIds spells;
    if (spell)
        spells.insert(spell);

    if (partyTraining || text.find("learn") != std::string::npos || sRandomPlayerbotMgr.IsFreeBot(bot) || (sPlayerbotAIConfig.autoTrainSpells != "no" && (creature->GetCreatureInfo()->TrainerType != TRAINER_TYPE_TRADESKILLS || !ai->HasActivePlayerMaster()))) //Todo rewrite to only exclude start primary profession skills and make config dependent.
    {
        // Older ranks can unlock newer ones whose IDs sort earlier. Complete
        // the eligible chain during this visit, without bypassing prerequisites.
        uint32 moneyBefore = bot->GetMoney();
        std::set<uint32> petKnownBefore;
        Pet* trainingPet = bot->GetPet();
        const ObjectGuid trainingPetGuid = trainingPet ? trainingPet->GetObjectGuid() : ObjectGuid();
        if (trainingPet)
            for (const auto& spell : trainingPet->m_spells)
                if (spell.second.state != PETSPELL_REMOVED) petKnownBefore.insert(spell.first);
        std::set<uint32> knownBefore;
        for (const auto& spell : bot->GetSpellMap())
            if (spell.second.state != PLAYERSPELL_REMOVED && !spell.second.disabled)
                knownBefore.insert(spell.first);
        for (uint32 pass = 0; pass < (partyTraining ? 20u : 1u); ++pass)
        {
            if (!Iterate(requester, creature, &TrainerAction::Learn, spells)) break;
            if (partyTraining && !LivingWowHasClassTraining(bot, creature->GetEntry())) break;
        }
        uint32 learnedCount = 0;
        for (const auto& spell : bot->GetSpellMap())
            if (spell.second.state != PLAYERSPELL_REMOVED && !spell.second.disabled && !knownBefore.count(spell.first))
            {
                ++learnedCount;
                sLog.outString("Living WoW training event=spell_learned bot=%u trainer=%u spell=%u level=%u money_before=%u money_after=%u",
                    bot->GetGUIDLow(), creature->GetEntry(), spell.first, bot->GetLevel(), moneyBefore, bot->GetMoney());
            }
        uint32 petLearnedCount = 0;
        trainingPet = bot->GetPet();
        if (trainingPet && trainingPet->GetObjectGuid() == trainingPetGuid)
            for (const auto& spell : trainingPet->m_spells)
                if (spell.second.state != PETSPELL_REMOVED && !petKnownBefore.count(spell.first))
                {
                    ++petLearnedCount;
                    sLog.outString("Living WoW training event=pet_spell_learned bot=%u pet=%u trainer=%u spell=%u",
                        bot->GetGUIDLow(), trainingPet->GetGUIDLow(), creature->GetEntry(), spell.first);
                }
        const auto trainerType = creature->GetCreatureInfo()->TrainerType;
        PlayerbotServiceTracking::Result(bot, trainerType == TRAINER_TYPE_CLASS ? "class_training" :
            trainerType == TRAINER_TYPE_TRADESKILLS ? "profession_training" :
            trainerType == TRAINER_TYPE_PETS ? "pet_training" : "other_training",
            creature->GetEntry(), 0, trainerType == TRAINER_TYPE_PETS ? "newly_learned_pet_spells" : "newly_learned_spells",
            0, trainerType == TRAINER_TYPE_PETS ? petLearnedCount : learnedCount);
        context->ClearValues("item usage");
        context->ClearValues("trainable spells");
        context->ClearValues("available trainers");
        context->ClearValues("train cost");
    }
    else
        Iterate(requester, creature, NULL, spells);

    return true;
}

void TrainerAction::TellHeader(Player* requester, Creature* creature)
{
    std::ostringstream out; out << "--- Can learn from " << creature->GetName() << " ---";
    ai->TellPlayer(requester, out, PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
}

void TrainerAction::TellFooter(Player* requester, uint32 totalCost)
{
    if (totalCost)
    {
        std::ostringstream out; out << "Total cost: " << chat->formatMoney(totalCost);
        ai->TellPlayer(requester, out, PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
    }
}
