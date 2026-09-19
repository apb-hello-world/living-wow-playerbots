#include "botpch.h"
#include "LivingNativeTraining.h"
#include "LivingNativeCraftCapture.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityNativeContext.h"
#include "PlayerbotTraining.h"
#include "PlayerbotTrainingLesson.h"
#include "Spells/Spell.h"
#include "Spells/Scripts/SpellScript.h"
#include <chrono>
#include <stdexcept>

namespace LivingActivity {
bool SupportedNativeTrainingCast(const TrainingLessonQuote& q,std::string& why) {
    auto reject=[&](const char* code){why=code;return false;};
    if(!FreePlayerTrainingCastQuote(q))return reject("training_free_player_cast_required");
    const auto* spell=sSpellTemplate.LookupEntry<SpellEntry>(q.teachingSpell);
    if(!spell || SpellScriptMgr::GetSpellScript(q.teachingSpell) || IsChanneledSpell(spell))
        return reject("training_scripted_or_channelled_cast_unsupported");
    if(spell->manaCost || spell->manaCostPerlevel || spell->ManaCostPercentage ||
        spell->manaPerSecond || spell->manaPerSecondPerLevel)
        return reject("training_cast_extra_resources_unsupported");
    std::set<uint32_t> targets;
    for(unsigned i=0;i<MAX_EFFECT_INDEX;++i)if(spell->Effect[i]) {
        if(spell->Effect[i]!=SPELL_EFFECT_LEARN_SPELL || !spell->EffectTriggerSpell[i] ||
            (spell->EffectImplicitTargetA[i] && spell->EffectImplicitTargetA[i]!=TARGET_UNIT_CASTER) || spell->EffectImplicitTargetB[i] ||
            !sSpellTemplate.LookupEntry<SpellEntry>(spell->EffectTriggerSpell[i]))
            return reject("training_exact_player_learning_effect_required");
        targets.insert(spell->EffectTriggerSpell[i]);
    }
    for(unsigned i=0;i<MAX_SPELL_REAGENTS;++i)
        if(spell->Reagent[i]>0 && spell->ReagentCount[i]>0)return reject("training_cast_extra_resources_unsupported");
    if(std::vector<uint32_t>(targets.begin(),targets.end())!=q.playerSpells)
        return reject("training_cast_target_manifest_changed");
    why.clear();return true;
}
namespace {
TrainingLessonState ReadTrainingFrame(Player& actor) {
    TrainingLessonState out;out.actor=actor.GetGUIDLow();out.money=actor.GetMoney();
    for(const auto& row:actor.GetSpellMap())
        if(row.second.state!=PLAYERSPELL_REMOVED && !row.second.disabled)out.playerSpells.insert(row.first);
    if(const auto* pet=actor.GetPet()) {
        out.pet=pet->GetObjectGuid().GetRawValue();
        for(const auto& row:pet->m_spells)if(row.second.state!=PETSPELL_REMOVED)out.petSpells.insert(row.first);
    }
    return out;
}
std::string TrainingFrameJson(const TrainingLessonState& frame,const TrainingLessonQuote& quote) {
    std::string out="{\"actor\":"+std::to_string(frame.actor)+",\"money\":"+std::to_string(frame.money)+
        ",\"pet\":"+std::to_string(frame.pet)+",\"spell_count\":"+std::to_string(frame.playerSpells.size())+",\"known_targets\":[";
    bool first=true;for(auto id:quote.playerSpells)if(frame.playerSpells.count(id)) {
        if(!first)out+=',';first=false;out+=std::to_string(id);
    }
    return out+"]}";
}
// Rechecks the same native offer inside the cast callback without treating
// this already-authorized teaching cast as a competing generic spell.
bool TrainingCastOfferReady(Player& actor,const TrainingLessonQuote& quote,std::string& why) {
    why="training_cast_native_offer_changed";
    if(!LivingWowFreeBotTraining(&actor) || !actor.IsInWorld() || !actor.GetMap() || actor.GetMap()->IsDungeon() ||
        !actor.IsStopped() || actor.GetTradeData() || actor.GetVictim() || !actor.getAttackers().empty() ||
        ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)))return false;
    auto* trainer=actor.GetNPCIfCanInteractWith(ObjectGuid(quote.trainer),UNIT_NPC_FLAG_TRAINER);
    if(!trainer || !trainer->IsTrainerOf(&actor,false) || trainer->GetCreatureInfo()->TrainerType!=TRAINER_TYPE_CLASS ||
        trainer->GetCreatureInfo()->TrainerClass!=actor.getClass())return false;
    for(const auto* list:{trainer->GetTrainerSpells(),trainer->GetTrainerTemplateSpells()})if(list) {
        const auto found=list->spellList.find(quote.lesson);if(found==list->spellList.end())continue;
        return found->second.spell==quote.teachingSpell && LivingWowCanTrainSpell(&actor,&found->second,trainer) &&
            SupportedNativeTrainingCast(quote,why) && TrainingLessonReady(quote,ReadTrainingFrame(actor));
    }
    return false;
}
class NativeTrainingCast final:public NativeCraftCast {
public:
    NativeTrainingCast(Task saved,ActionContext allowed,TrainingLessonQuote quoted)
        :task(std::move(saved)),action(std::move(allowed)),quote(std::move(quoted)) {
        if(task.phase!=Phase::Executing || task.mode!=Mode::Active || !task.accepted ||
            !PartyTrainingQuoteMatches(task,quote) || !FreePlayerTrainingCastQuote(quote) ||
            !Fresh(task,action,task.context) || action.revision!=task.revision ||
            ((Mask(Effect::Spell)|Mask(Effect::Social))&~action.permittedEffects))
            throw std::invalid_argument("training_cast_saved_binding_required");
    }
    bool Start(Player& actor,std::string& why) override {
        why="training_cast_launch_prerequisite";TrainingLessonQuote current;
        if(result.started || !sLivingActivityCoordinator.OnWorldThread() || actor.GetGUIDLow()!=task.actor ||
            !Authority(actor) || actor.IsNonMeleeSpellCasted(false,true,true) ||
            !PlanNativeTrainingLesson(*actor.GetPlayerbotAI(),ObjectGuid(quote.trainer),quote.lesson,current,why) ||
            !SameTrainingLessonQuote(quote,current) || !TrainingCastOfferReady(actor,quote,why))return false;
        result.before=ReadTrainingFrame(actor);
        const auto* info=sSpellTemplate.LookupEntry<SpellEntry>(quote.teachingSpell);
        // Preserve the native trainer's triggered teaching semantics, while
        // attaching the existing pre-effect and completion/save fence.
        std::unique_ptr<Spell> spell(new Spell(&actor,info,TRIGGERED_OLD_TRIGGERED));
        if(!spell->SetLivingCraftCast(shared_from_this())){why="training_cast_binding_failed";return false;}
        result.started=true;SpellCastTargets targets;targets.setUnitTarget(&actor);
        spell.release()->SpellStart(&targets);why.clear();return true;
    }
    bool Ready() const override{return result.finished || result.uncertain;}
    NativeObservation Observe(Player& actor,std::vector<VerifiedItemGain>& gains) const override {
        gains.clear();NativeObservation out;out.nativeReference="trainer_lesson:"+std::to_string(quote.lesson);
        out.afterState="{\"lesson\":"+std::to_string(quote.lesson)+",\"effect_entered\":"+(result.effect?"true":"false")+
            ",\"native_finished\":"+(result.finished?"true":"false")+",\"native_succeeded\":"+(result.succeeded?"true":"false")+
            ",\"before\":"+TrainingFrameJson(result.before,quote)+",\"after\":"+TrainingFrameJson(result.after,quote)+'}';
        out.state=VerifyTrainingCast(quote,result,out.evidence);
        if(!SameTrainingState(ReadTrainingFrame(actor),result.after)) {
            out.state=OperationState::Reconciling;out.evidence="training_cast_changed_before_save";
        }
        return out;
    }
    std::string PersistedProof(Player& actor,const Task& after) const override {
        if(!result.finished || result.uncertain || !SameTrainingState(ReadTrainingFrame(actor),result.after))
            throw std::runtime_error("training_cast_native_proof_missing");
        std::string proof="SELECT "+SqlValue(after.id)+','+std::to_string(after.revision)+" FROM characters c WHERE c.guid="+
            std::to_string(task.actor)+" AND c.money="+std::to_string(result.after.money)+
            " AND (SELECT COUNT(*) FROM character_spell s WHERE s.guid=c.guid AND s.disabled=0)="+
            std::to_string(result.after.playerSpells.size());
        for(auto id:quote.playerSpells)proof+=(result.after.playerSpells.count(id)?" AND EXISTS(":" AND NOT EXISTS(")+
            std::string("SELECT 1 FROM character_spell s WHERE s.guid=c.guid AND s.spell=")+std::to_string(id)+" AND s.disabled=0)";
        return proof;
    }
    std::unique_ptr<ExecutionScope> EnterEffect(Spell& spell) override {
        auto* actor=Actor(spell);std::string why;
        if(!actor || !result.started || result.effect || result.finished || result.uncertain || !Authority(*actor) ||
            !TrainingCastOfferReady(*actor,quote,why) || !SameTrainingState(ReadTrainingFrame(*actor),result.before))return {};
        result.effect=true;return std::make_unique<ExecutionScope>(task,action);
    }
    void Created(Spell&,uint32_t,uint32_t) noexcept override{result.uncertain=true;}
    void Finished(Spell& spell,bool success) noexcept override {
        try {
            if(result.finished)return;
            auto* actor=Actor(spell);if(!actor)result.uncertain=true;else result.after=ReadTrainingFrame(*actor);
            result.finished=true;result.succeeded=success;
        }catch(...){result.uncertain=true;}
    }
    void Abandon() noexcept override{if(result.started && !result.finished)result.uncertain=true;}
private:
    Player* Actor(Spell& spell) const {
        auto* caster=spell.GetTrueCaster();
        return caster && caster->IsPlayer() && caster->GetGUIDLow()==task.actor && spell.m_spellInfo &&
            spell.m_spellInfo->Id==quote.teachingSpell?static_cast<Player*>(caster):nullptr;
    }
    bool Authority(Player& actor) const {
        auto* ai=actor.GetPlayerbotAI();if(!ai)return false;
        const auto reader=ai->ActivityPermissions();const auto view=reader.Inspect();if(!view)return false;
        const auto current=ReadNativeContext(actor,view->current.policyRevision,view->current.boot);
        const uint64_t now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        return reader.Check({Mask(Effect::Spell)|Mask(Effect::Social),Lane::Managed,true},current,now,&task,&action,nullptr,
            ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)))==AuthorityCode::Allowed;
    }
    const Task task;const ActionContext action;const TrainingLessonQuote quote;TrainingCastResult result;
};
}
std::shared_ptr<NativeCraftCast> NativePartyTraining::ReserveNativeCast(const OperationRequest& request,
    const Task& executing,const ActionContext& action) const {
    if(request.kind!=OperationKind() || request.beforeState!=EncodePartyTrainingQuote(quote))
        throw std::invalid_argument("training_cast_exact_operation_required");
    return std::make_shared<NativeTrainingCast>(executing,action,quote);
}
}
