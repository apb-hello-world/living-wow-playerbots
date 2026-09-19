#include "botpch.h"
#include "PlayerbotTrainingLesson.h"
#include "PlayerbotTraining.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityNativeContext.h"
#include "LivingServiceExecution.h"
#include "strategy/values/BudgetValues.h"

namespace LivingActivity {
namespace {
TrainingLessonState ReadLessonState(Player& actor) {
    TrainingLessonState result;result.actor=actor.GetGUIDLow();result.money=actor.GetMoney();
    for(const auto& spell:actor.GetSpellMap())
        if(spell.second.state!=PLAYERSPELL_REMOVED && !spell.second.disabled)result.playerSpells.insert(spell.first);
    if(const auto* pet=actor.GetPet()) {
        result.pet=pet->GetObjectGuid().GetRawValue();
        for(const auto& spell:pet->m_spells)
            if(spell.second.state!=PETSPELL_REMOVED)result.petSpells.insert(spell.first);
    }
    return result;
}
}
bool PlanNativeTrainingLesson(PlayerbotAI& ai,const ObjectGuid& guid,uint32_t lesson,TrainingLessonQuote& output,std::string& reason) {
    output={};auto reject=[&](const char* why){reason=why;return false;};
    auto* bot=ai.GetBot();
    if(!bot || !bot->GetSession() || !bot->GetMap() || !bot->IsStopped() || bot->GetMap()->IsDungeon() ||
        ReadNativeSafety(*bot,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) ||
        LivingServiceExecution::Busy(bot))return reject("training_safety_pause");
    auto* trainer=bot->GetNPCIfCanInteractWith(guid,UNIT_NPC_FLAG_TRAINER);
    if(!trainer || !trainer->IsTrainerOf(bot,false))return reject("training_interacting_trainer_required");
    // Same precedence as the legacy merged list: creature-specific lessons win.
    const TrainerSpell* offered=nullptr;
    for(const auto* list:{trainer->GetTrainerSpells(),trainer->GetTrainerTemplateSpells()})
        if(list) {const auto it=list->spellList.find(lesson);if(it!=list->spellList.end()){offered=&it->second;break;}}
    if(!offered || !LivingWowCanTrainSpell(bot,offered,trainer))return reject("training_lesson_not_eligible");
    const auto* teaching=sSpellTemplate.LookupEntry<SpellEntry>(offered->spell);
    if(!teaching)return reject("training_teaching_spell_missing");
    TrainingLessonQuote quote;quote.actor=bot->GetGUIDLow();quote.trainer=guid.GetRawValue();quote.lesson=lesson;
    quote.teachingSpell=offered->spell;
    quote.money=bot->GetMoney();
    const bool free=LivingWowFreeBotTraining(bot) || sPlayerbotAIConfig.autoTrainSpells=="free" || ai.HasCheat(BotCheatMask::gold);
    quote.cost=free?0:uint32(floor(offered->spellCost*bot->GetReputationPriceDiscount(trainer)));
    if(quote.cost && ai.GetAiObjectContext()->GetValue<uint32>("free money for",std::to_string(uint32(ai::NeedMoneyFor::spells)))->Get()<quote.cost)
        return reject("training_unreserved_money_shortfall");
#ifdef MANGOSBOT_ZERO
    quote.cast= !offered->learnedSpell;
#else
    quote.cast=offered->IsCastable();
#endif
    if(quote.cast) {
        std::set<uint32_t> player,pets;
        for(unsigned i=0;i<3;++i) {
            if(teaching->Effect[i]!=SPELL_EFFECT_LEARN_SPELL && teaching->Effect[i]!=SPELL_EFFECT_LEARN_PET_SPELL)continue;
            const uint32_t learned=teaching->EffectTriggerSpell[i];
            if(!learned || !sSpellTemplate.LookupEntry<SpellEntry>(learned))return reject("training_target_spell_missing");
            const bool pet=teaching->Effect[i]==SPELL_EFFECT_LEARN_PET_SPELL ||
                teaching->EffectImplicitTargetA[i]==TARGET_UNIT_CASTER_PET || teaching->EffectImplicitTargetB[i]==TARGET_UNIT_CASTER_PET;
            (pet?pets:player).insert(learned);
        }
        quote.playerSpells.assign(player.begin(),player.end());quote.petSpells.assign(pets.begin(),pets.end());
    } else {
#ifdef MANGOSBOT_ZERO
        std::set<uint32_t> spells;
        for(unsigned i=0;i<3;++i)if(teaching->Effect[i]==SPELL_EFFECT_LEARN_SPELL)spells.insert(teaching->EffectTriggerSpell[i]);
        if(spells.empty())spells.insert(offered->learnedSpell);
        quote.playerSpells.assign(spells.begin(),spells.end());
#else
        quote.playerSpells.push_back(lesson);
#endif
    }
    const auto before=ReadLessonState(*bot);quote.pet=before.pet;
    if(!TrainingLessonReady(quote,before))return reject("training_exact_lesson_unavailable");
    output=std::move(quote);reason.clear();return true;
}
NativeTrainingLessonResult ExecuteNativeTrainingLesson(PlayerbotAI& ai,const TrainingLessonQuote& quote) {
    auto reject=[](const char* reason){return NativeTrainingLessonResult{TrainingLessonOutcome::Rejected,reason};};
    if(!ValidTrainingLesson(quote))return reject("training_invalid_quote");
    const ObjectGuid guid(quote.trainer);TrainingLessonQuote current;std::string reason;
    if(!PlanNativeTrainingLesson(ai,guid,quote.lesson,current,reason))return reject(reason.c_str());
    if(!SameTrainingLessonQuote(quote,current))return reject("training_quote_changed");
    const Effects effects{Mask(Effect::Spell)|Mask(Effect::Social)|(quote.cost?Mask(Effect::Money):0),Lane::Managed,true};
    if(!sLivingActivityCoordinator.PermitEffects(ai,effects,"native trainer lesson"))return reject("training_authority_denied");
    auto* bot=ai.GetBot();
    const auto before=ReadLessonState(*bot);
    if(!TrainingLessonReady(quote,before))return reject("training_exact_lesson_unavailable");
#ifndef MANGOSBOT_ZERO
    bot->GetSession()->SendPlaySpellVisual(guid,0xB3);
    WorldPacket impact(SMSG_PLAY_SPELL_IMPACT,8+4);
    impact << bot->GetObjectGuid() << uint32(0x016A);
    bot->GetSession()->SendPacket(impact);
#endif
    // Never charge before validating the spell, recipient and native offer.
    if(quote.cost)bot->ModifyMoney(-int32(quote.cost));
    if(quote.cast)bot->CastSpell(bot,quote.teachingSpell,TRIGGERED_OLD_TRIGGERED);
    else for(const auto spell:quote.playerSpells)bot->learnSpell(spell,false);
    const auto after=ReadLessonState(*bot);
    const auto outcome=ObserveTrainingLesson(quote,before,after);
    sLog.outString("Living WoW training event=lesson_observed bot=%u trainer=%u lesson=%u outcome=%s money_before=%u money_after=%u",
        quote.actor,guid.GetEntry(),quote.lesson,outcome==TrainingLessonOutcome::Verified?"verified":
        outcome==TrainingLessonOutcome::Rejected?"rejected":"uncertain",before.money,after.money);
    if(outcome==TrainingLessonOutcome::Verified) {
        const auto* teaching=sSpellTemplate.LookupEntry<SpellEntry>(quote.teachingSpell);
        if(teaching)sPlayerbotAIConfig.logEvent(&ai,"TrainerAction",teaching->SpellName[0],std::to_string(teaching->Id));
    }
    return {outcome,outcome==TrainingLessonOutcome::Verified?"native_exact_trainer_lesson_observed":
        outcome==TrainingLessonOutcome::Rejected?"native_training_rejected_without_effect":"native_training_requires_reconciliation"};
}
NativeTrainingLessonResult ExecuteNativeTrainingLesson(PlayerbotAI& ai,const ObjectGuid& guid,uint32_t lesson) {
    TrainingLessonQuote quote;std::string reason;
    if(!PlanNativeTrainingLesson(ai,guid,lesson,quote,reason))return {TrainingLessonOutcome::Rejected,reason};
    return ExecuteNativeTrainingLesson(ai,quote);
}
}
