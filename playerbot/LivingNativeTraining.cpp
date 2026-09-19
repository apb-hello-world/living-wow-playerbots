#include "botpch.h"
#include "LivingNativeTraining.h"
#include "PlayerbotTraining.h"
#include "PlayerbotTrainingLesson.h"
#include "LivingActivityCoordinator.h"
namespace LivingActivity {
bool PlanNativePartyTraining(Player& actor,const ObjectGuid& guid,PartyTrainingJob& out,std::string& why) {
    out={};auto reject=[&](const char* code){why=code;return false;};
    if(!sLivingActivityCoordinator.OnWorldThread() || !LivingWowFreeBotTraining(&actor))return reject("party_training_bot_required");
    auto* trainer=actor.GetNPCIfCanInteractWith(guid,UNIT_NPC_FLAG_TRAINER);
    if(!trainer || !trainer->IsTrainerOf(&actor,false) || !LivingWowPartyTrainerMatches(&actor,trainer->GetCreatureInfo()))
        return reject("party_training_class_trainer_required"); // Historical retry code, same saved trainer route.
    std::set<uint32_t> lessons;bool cast=false;
    for(const auto* list:{trainer->GetTrainerSpells(),trainer->GetTrainerTemplateSpells()})if(list)
        for(const auto& row:list->spellList) {
            if(trainer->GetCreatureInfo()->TrainerType==TRAINER_TYPE_TRADESKILLS && !LivingWowExistingSkillTier(&actor,&row.second))continue;
            TrainingLessonQuote q;std::string blocker;
            if(!PlanNativeTrainingLesson(*actor.GetPlayerbotAI(),guid,row.first,q,blocker))continue;
            if(!CompleteNativeTrainingSkillQuote(actor,q,blocker)){cast=true;continue;}
            if(!DirectFreeTrainingQuote(q) && !DirectSkillTrainingQuote(q) && !FreePetTrainingQuote(q) && !SupportedNativeTrainingCast(q,blocker)){cast=true;continue;}
            lessons.insert(row.first);
        }
    if(lessons.empty())return reject(cast?"party_training_cast_capture_required":"party_training_no_direct_lesson");
    out.trainer=trainer->GetEntry();
    for(auto lesson:lessons){out.lessons.push_back(lesson);if(out.lessons.size()==64)break;}
    why.clear();return true;
}
bool QuoteNativePartyTraining(Player& actor,const Task& task,TrainingLessonQuote& quote,std::string& why) {
    quote={};PartyTrainingJob job;
    if(!sLivingActivityCoordinator.OnWorldThread() || !LivingWowFreeBotTraining(&actor) || actor.GetGUIDLow()!=task.actor ||
        !ValidatePartyTrainingTask(task,why) || !IsPartyTrainingTask(task) ||
        !DecodePartyTrainingJob(task.checkpoint.data,job) || job.next>=job.lessons.size()) {
        why="party_training_saved_lesson_required";return false;
    }
    why="party_training_class_trainer_required";
    for(auto guid:actor.GetPlayerbotAI()->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("nearest npcs no los")->Get()) {
        if(guid.GetEntry()!=job.trainer)continue;
        auto* trainer=actor.GetNPCIfCanInteractWith(guid,UNIT_NPC_FLAG_TRAINER);
        if(!trainer || !LivingWowPartyTrainerMatches(&actor,trainer->GetCreatureInfo()))continue;
        if(PlanNativeTrainingLesson(*actor.GetPlayerbotAI(),guid,job.lessons[job.next],quote,why)) {
            if(!CompleteNativeTrainingSkillQuote(actor,quote,why))return false;
            if(trainer->GetCreatureInfo()->TrainerType==TRAINER_TYPE_TRADESKILLS &&
                (!quote.skill.id || !quote.skill.before.value || quote.skill.after.step<=quote.skill.before.step)) {
                why="party_training_existing_profession_tier_required";return false;
            }
            if(PartyTrainingQuoteMatches(task,quote) &&
                (DirectFreeTrainingQuote(quote) || DirectSkillTrainingQuote(quote) || FreePetTrainingQuote(quote) || SupportedNativeTrainingCast(quote,why)))return true;
            why="party_training_native_quote_changed";
        }
    }
    return false;
}
bool NativePartyTraining::ValidateNative(Player& actor,const OperationRequest& r,std::string& why) {
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(r.transition.task.id);TrainingLessonQuote current;
    if(!saved || r.beforeState!=EncodePartyTrainingQuote(quote) || !r.consumption.empty() ||
        !r.itemGain.Empty() || !r.mailGain.Empty() || !r.itemTransfer.id.empty() ||
        !QuoteNativePartyTraining(actor,*saved,current,why) || !SameTrainingLessonQuote(quote,current)) {
        if(why.empty())why="party_training_native_quote_changed";return false;
    }
    why.clear();return true;
}
NativeObservation NativePartyTraining::ExecuteNative(Player& actor,const OperationRequest& r) {
    NativeObservation out;std::string why;
    if(quote.cast && quote.petSpells.empty()){out.state=OperationState::Rejected;out.evidence="training_cast_requires_deferred_dispatch";return out;}
    if(!ValidateNative(actor,r,why)){out.state=OperationState::Rejected;out.evidence=why;return out;}
    if(!quote.petSpells.empty())return ExecuteNativePetTraining(actor,quote);
    if(quote.skill.id)return ExecuteNativeDirectTrainingSkill(actor,quote);
    const auto result=ExecuteNativeTrainingLesson(*actor.GetPlayerbotAI(),quote);
    const auto it=actor.GetSpellMap().find(quote.lesson);
    const bool known=it!=actor.GetSpellMap().end() && it->second.state!=PLAYERSPELL_REMOVED && !it->second.disabled;
    out.nativeReference="trainer_lesson:"+std::to_string(quote.lesson);
    out.afterState="{\"actor\":"+std::to_string(actor.GetGUIDLow())+",\"lesson\":"+std::to_string(quote.lesson)+
        ",\"money\":"+std::to_string(actor.GetMoney())+",\"known\":"+(known?"true":"false")+'}';
    if(result.outcome==TrainingLessonOutcome::Verified && known && actor.GetMoney()==quote.money) {
        out.state=OperationState::Verified;out.evidence="native_training_exact_spellbook_and_unchanged_money";
    } else if(result.outcome==TrainingLessonOutcome::Rejected && !known && actor.GetMoney()==quote.money) {
        out.state=OperationState::Rejected;out.evidence=result.reason;
    } else out.evidence="native_training_requires_reconciliation";
    return out;
}
std::string NativePartyTraining::PersistedNativeProof(Player& actor,const OperationRequest&,const Task& after) const {
    if(!quote.petSpells.empty())return PersistedPetTrainingProof(actor,quote,after);
    const auto it=actor.GetSpellMap().find(quote.lesson);
    const bool known=it!=actor.GetSpellMap().end() && it->second.state!=PLAYERSPELL_REMOVED && !it->second.disabled;
    std::string proof="SELECT "+SqlValue(after.id)+','+std::to_string(after.revision)+" FROM characters c WHERE c.guid="+
        std::to_string(actor.GetGUIDLow())+" AND c.money="+std::to_string(actor.GetMoney())+
        (known?" AND EXISTS(":" AND NOT EXISTS(")+"SELECT 1 FROM character_spell s WHERE s.guid=c.guid AND s.spell="+
        std::to_string(quote.lesson)+" AND s.disabled=0)";
    if(quote.skill.id)proof+=" AND EXISTS(SELECT 1 FROM character_skills s WHERE s.guid=c.guid AND s.skill="+
        std::to_string(quote.skill.id)+" AND s.value="+std::to_string(actor.GetSkillValuePure(quote.skill.id))+
        " AND s.max="+std::to_string(actor.GetSkillMaxPure(quote.skill.id))+')';
    return proof;
}
}
