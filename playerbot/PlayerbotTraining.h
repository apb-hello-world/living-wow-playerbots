#pragma once
#include <algorithm>
#include "PlayerbotOrganicEconomy.h"
#include "Entities/Pet.h"
#include "Spells/SpellTargetDefines.h"

// Bot sessions alone receive the trainer subsidy. Human trainer handling stays
// in the core, with its normal prices and all normal spell prerequisites.
inline bool LivingWowFreeBotTraining(Player* player)
{
    return player && player->GetPlayerbotAI() && !player->isRealPlayer();
}

inline bool LivingWowCanTrainSpell(Player* bot, TrainerSpell const* spell, Unit* trainer = nullptr)
{
    if (!bot || !spell) return false;
#ifdef MANGOSBOT_ZERO
    if (!sPlayerbotOrganicEconomy.CanLearnProfessionSpell(bot, spell->learnedSpell)) return false;
#else
    for (uint32 learned : spell->learnedSpell)
        if (learned && !sPlayerbotOrganicEconomy.CanLearnProfessionSpell(bot, learned)) return false;
#endif
    // Inspect native cast mode, not the NPC's label. The pinned core treats
    // LEARN_SPELL-to-pet offers as casts; those require a living eligible pet.
    // A genuine direct owner lesson must not borrow future cast prerequisites.
    const SpellEntry* teaching = sSpellTemplate.LookupEntry<SpellEntry>(spell->spell);
    if (!teaching) return false;
#ifdef MANGOSBOT_ZERO
    const bool trainerCasts = !spell->learnedSpell;
#else
    const bool trainerCasts = spell->IsCastable();
#endif
    for (unsigned effect = 0; effect < 3; ++effect)
    {
        if (!trainerCasts) break;
        if (teaching->Effect[effect] != SPELL_EFFECT_LEARN_PET_SPELL &&
            !(teaching->Effect[effect] == SPELL_EFFECT_LEARN_SPELL &&
              (teaching->EffectImplicitTargetA[effect] == TARGET_UNIT_CASTER_PET ||
               teaching->EffectImplicitTargetB[effect] == TARGET_UNIT_CASTER_PET))) continue;
        Pet* pet = bot->GetPet();
        const uint32 learned = teaching->EffectTriggerSpell[effect];
        const SpellEntry* ability = sSpellTemplate.LookupEntry<SpellEntry>(learned);
        if (!pet || !pet->IsAlive() || !ability || pet->HasSpell(learned) ||
            pet->GetLevel() < ability->spellLevel || !pet->HasTPForSpell(learned) ||
            !pet->CanTakeMoreActiveSpells(learned)) return false;
        for (const auto& known : pet->m_spells)
            if (known.second.state != PETSPELL_REMOVED &&
                sSpellMgr.GetFirstSpellInChain(known.first) == sSpellMgr.GetFirstSpellInChain(learned) &&
                sSpellMgr.GetSpellRank(known.first) >= sSpellMgr.GetSpellRank(learned)) return false;
    }
    uint32 requiredLevel = 0;
    if (!bot->IsSpellFitByClassAndRace(spell->spell, &requiredLevel)) return false;
    requiredLevel = spell->isProvidedReqLevel ? spell->reqLevel : std::max(requiredLevel, spell->reqLevel);
    if (spell->conditionId && !sObjectMgr.IsConditionSatisfied(spell->conditionId,
        bot, bot->GetMap(), trainer ? trainer : bot, CONDITION_FROM_TRAINER)) return false;
    return bot->GetTrainerSpellState(spell, requiredLevel) == TRAINER_SPELL_GREEN;
}

inline bool LivingWowHasClassTraining(Player* bot, int32 entry)
{
    if (!bot || entry <= 0) return false;
    CreatureInfo const* trainer = sObjectMgr.GetCreatureTemplate(entry);
    if (!trainer || trainer->TrainerType != TRAINER_TYPE_CLASS ||
        trainer->TrainerClass != bot->getClass()) return false;
    TrainerSpellData const* lists[] = {
        sObjectMgr.GetNpcTrainerSpells(entry),
        trainer->TrainerTemplateId ? sObjectMgr.GetNpcTrainerTemplateSpells(trainer->TrainerTemplateId) : nullptr
    };
    for (TrainerSpellData const* list : lists)
        if (list)
            for (const auto& spell : list->spellList)
                if (LivingWowCanTrainSpell(bot, &spell.second))
                    return true;
    return false;
}

inline bool LivingWowPartyTrainerMatches(Player* bot,const CreatureInfo* trainer) {
    return bot && trainer && (((trainer->TrainerType==TRAINER_TYPE_CLASS || trainer->TrainerType==TRAINER_TYPE_PETS) &&
        trainer->TrainerClass==bot->getClass()) ||
        trainer->TrainerType==TRAINER_TYPE_TRADESKILLS);
}
inline bool LivingWowExistingSkillTier(Player* bot,const TrainerSpell* offered) {
    if(!bot || !offered)return false;
#ifdef MANGOSBOT_ZERO
    const std::vector<uint32_t> learned={offered->learnedSpell};
#else
    const auto& learned=offered->learnedSpell;
#endif
    for(auto id:learned)if(const auto* skill=sSpellMgr.GetSpellLearnSkill(id))
        if(bot->GetSkillValuePure(skill->skill)>0 && skill->step>bot->GetSkillStep(skill->skill))return true;
    return false;
}
inline bool LivingWowHasPartyTraining(Player* bot,int32 entry) {
    if(!bot || entry<=0)return false;
    const auto* trainer=sObjectMgr.GetCreatureTemplate(entry);
    if(!LivingWowPartyTrainerMatches(bot,trainer))return false;
    if(trainer->TrainerType==TRAINER_TYPE_CLASS)return LivingWowHasClassTraining(bot,entry);
    for(const auto* list:{sObjectMgr.GetNpcTrainerSpells(entry),trainer->TrainerTemplateId?
        sObjectMgr.GetNpcTrainerTemplateSpells(trainer->TrainerTemplateId):nullptr})if(list)
        for(const auto& row:list->spellList)
            if((trainer->TrainerType==TRAINER_TYPE_PETS || LivingWowExistingSkillTier(bot,&row.second)) &&
                LivingWowCanTrainSpell(bot,&row.second))return true;
    return false;
}
