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
    std::set<uint32_t> targets;unsigned skillEffects=0;
    for(unsigned i=0;i<MAX_EFFECT_INDEX;++i)if(spell->Effect[i]) {
        if((spell->EffectImplicitTargetA[i] && spell->EffectImplicitTargetA[i]!=TARGET_UNIT_CASTER) || spell->EffectImplicitTargetB[i])
            return reject("training_exact_player_learning_effect_required");
        if(spell->Effect[i]==SPELL_EFFECT_SKILL_STEP) {
            if(++skillEffects!=1 || !q.skill.id || spell->EffectMiscValue[i]!=q.skill.id ||
                spell->EffectDieSides[i]>1 || spell->EffectDicePerLevel[i] || spell->EffectRealPointsPerLevel[i])
                return reject("training_exact_skill_step_required");
            continue;
        }
        if(spell->Effect[i]!=SPELL_EFFECT_LEARN_SPELL || !spell->EffectTriggerSpell[i] ||
            !sSpellTemplate.LookupEntry<SpellEntry>(spell->EffectTriggerSpell[i]))
            return reject("training_exact_player_learning_effect_required");
        targets.insert(spell->EffectTriggerSpell[i]);
    }
    for(unsigned i=0;i<MAX_SPELL_REAGENTS;++i)
        if(spell->Reagent[i]>0 && spell->ReagentCount[i]>0)return reject("training_cast_extra_resources_unsupported");
    if(bool(skillEffects)!=bool(q.skill.id) || std::vector<uint32_t>(targets.begin(),targets.end())!=q.playerSpells)
        return reject("training_cast_target_manifest_changed");
    why.clear();return true;
}
bool CompleteNativeTrainingSkillQuote(Player& actor,TrainingLessonQuote& quote,std::string& why) {
    quote.skill={};
    if(!quote.cast) {
        const auto* trainer=actor.GetMap()?actor.GetMap()->GetCreature(ObjectGuid(quote.trainer)):nullptr;
        if(!trainer || trainer->GetCreatureInfo()->TrainerType!=TRAINER_TYPE_TRADESKILLS)return true;
        const auto* learned=sSpellMgr.GetSpellLearnSkill(quote.lesson);
        if(!learned)return true;
        why="training_existing_skill_tier_required";
        if(!actor.HasSkill(learned->skill) || !learned->step || learned->step>MAX_SKILL_STEP)return false;
        auto& skill=quote.skill;skill.id=learned->skill;
        skill.before={actor.GetSkillValuePure(skill.id),actor.GetSkillMaxPure(skill.id),actor.GetSkillStep(skill.id)};
        uint16_t maximum=0;
        const auto* native=actor.GetSkillInfo(skill.id,[&](const SkillRaceClassInfoEntry& entry) {
            const auto* tiers=sSkillTiersStore.LookupEntry(entry.skillTierId);
            if(!tiers || !tiers->maxSkillValue[learned->step-1])return false;
            maximum=uint16_t(tiers->maxSkillValue[learned->step-1]);return true;
        });
        // Player::SetSkillStep uses the same DBC maximum and retains the real
        // earned value. Maximized skills need their separate cast contract.
        if(!native || !maximum || (native->flags&SKILL_FLAG_MAXIMIZED))return false;
        skill.after={skill.before.value,maximum,learned->step};
        if(!DirectSkillTrainingQuote(quote))return false;
        why.clear();return true;
    }
    auto reject=[&](const char* code){why=code;return false;};
    const auto* info=sSpellTemplate.LookupEntry<SpellEntry>(quote.teachingSpell);
    if(!info || SpellScriptMgr::GetSpellScript(quote.teachingSpell) || IsChanneledSpell(info))
        return reject("training_scripted_or_channelled_cast_unsupported");
    for(unsigned i=0;i<MAX_EFFECT_INDEX;++i)if(info->Effect[i]==SPELL_EFFECT_SKILL_STEP) {
        if(quote.skill.id || info->EffectMiscValue[i]<=0 || info->EffectMiscValue[i]>65535 ||
            info->EffectDieSides[i]>1 || info->EffectDicePerLevel[i] || info->EffectRealPointsPerLevel[i])
            return reject("training_exact_skill_step_required");
        // Same native calculation as EffectLearnSkill, with no cast/effect.
        Spell inspect(&actor,info,TRIGGERED_OLD_TRIGGERED);
        const auto step=inspect.CalculateSpellEffectValue(SpellEffectIndex(i),&actor,true,false);
        if(step<=0 || step>MAX_SKILL_STEP)return reject("training_skill_step_unavailable");
        auto& skill=quote.skill;skill.id=uint16_t(info->EffectMiscValue[i]);
        skill.before={actor.GetSkillValuePure(skill.id),actor.GetSkillMaxPure(skill.id),actor.GetSkillStep(skill.id)};
        uint16_t maximum=0;
        const auto* native=actor.GetSkillInfo(skill.id,[&](const SkillRaceClassInfoEntry& entry) {
            const auto* tiers=sSkillTiersStore.LookupEntry(entry.skillTierId);
            if(!tiers || !tiers->maxSkillValue[step-1])return false;
            maximum=uint16_t(tiers->maxSkillValue[step-1]);return true;
        });
        if(!native || !maximum)return reject("training_native_skill_tier_missing");
        skill.after={uint16_t(native->flags&SKILL_FLAG_MAXIMIZED?maximum:std::max(uint16_t(1),skill.before.value)),
            maximum,uint16_t(step)};
        if(skill.before.step>skill.after.step || skill.before.maximum>maximum || skill.after.value>maximum)
            return reject("training_skill_downgrade_not_allowed");
    }
    return SupportedNativeTrainingCast(quote,why);
}
namespace {
bool TrainingSavedTargets(Player& actor,const TrainingLessonQuote& q,std::set<uint32_t>& saved,
    std::set<uint32_t>& dependent) {
    std::set<uint32_t> present;dependent.clear();std::vector<std::pair<uint32_t,uint32_t>> edges;
    for(auto id:q.playerSpells) {
        const auto found=actor.GetSpellMap().find(id);
        if(found==actor.GetSpellMap().end() || found->second.state==PLAYERSPELL_REMOVED || found->second.disabled)continue;
        present.insert(id);if(found->second.dependent)dependent.insert(id);
    }
    for(auto parent:present) {
        const auto bounds=sSpellMgr.GetSpellLearnSpellMapBounds(parent);
        for(auto it=bounds.first;it!=bounds.second;++it)
            if(!it->second.autoLearned && present.count(it->second.spell))edges.emplace_back(parent,it->second.spell);
    }
    return TrainingPersistenceTargets(present,dependent,edges,saved);
}
std::string TrainingIdsJson(const std::set<uint32_t>& ids) {
    std::string out="[";for(auto id:ids){if(out.size()>1)out+=',';out+=std::to_string(id);}return out+']';
}
std::map<uint16_t,TrainingSkillState> ReadTrainingSkills(Player& actor) {
    std::map<uint16_t,TrainingSkillState> out;
    for(unsigned i=0;i<PLAYER_MAX_SKILLS;++i) {
        const auto id=uint16_t(actor.GetUInt32Value(PLAYER_SKILL_INFO_1_1+3*i));
        if(id && actor.HasSkill(id))out.emplace(id,TrainingSkillState{
            actor.GetSkillValuePure(id),actor.GetSkillMaxPure(id),actor.GetSkillStep(id)});
    }
    return out;
}
std::string TrainingSkillsJson(const std::map<uint16_t,TrainingSkillState>& skills) {
    std::string out="[";
    for(const auto& row:skills) {
        if(out.size()>1)out+=',';
        out+='['+std::to_string(row.first)+','+std::to_string(row.second.value)+','+
            std::to_string(row.second.maximum)+','+std::to_string(row.second.step)+']';
    }
    return out+']';
}
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
    if(!trainer || !trainer->IsTrainerOf(&actor,false) || !LivingWowPartyTrainerMatches(&actor,trainer->GetCreatureInfo()))return false;
    if(trainer->GetCreatureInfo()->TrainerType==TRAINER_TYPE_TRADESKILLS &&
        (!quote.skill.id || !quote.skill.before.value || quote.skill.after.step<=quote.skill.before.step))return false;
    for(const auto* list:{trainer->GetTrainerSpells(),trainer->GetTrainerTemplateSpells()})if(list) {
        const auto found=list->spellList.find(quote.lesson);if(found==list->spellList.end())continue;
        auto current=quote;
        return found->second.spell==quote.teachingSpell && LivingWowCanTrainSpell(&actor,&found->second,trainer) &&
            CompleteNativeTrainingSkillQuote(actor,current,why) && SameTrainingLessonQuote(quote,current) &&
            TrainingLessonReady(quote,ReadTrainingFrame(actor));
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
            !QuoteNativePartyTraining(actor,task,current,why) ||
            !SameTrainingLessonQuote(quote,current) || !TrainingCastOfferReady(actor,quote,why))return false;
        result.before=ReadTrainingFrame(actor);
        result.skillsBefore=ReadTrainingSkills(actor);
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
        std::set<uint32_t> saved,dependent;const bool persistenceReady=TrainingSavedTargets(actor,quote,saved,dependent);
        out.afterState="{\"lesson\":"+std::to_string(quote.lesson)+",\"effect_entered\":"+(result.effect?"true":"false")+
            ",\"native_finished\":"+(result.finished?"true":"false")+",\"native_succeeded\":"+(result.succeeded?"true":"false")+
            ",\"before\":"+TrainingFrameJson(result.before,quote)+",\"after\":"+TrainingFrameJson(result.after,quote)+
            ",\"skills_before\":"+TrainingSkillsJson(result.skillsBefore)+",\"skills_after\":"+TrainingSkillsJson(result.skillsAfter)+
            ",\"saved_targets\":"+TrainingIdsJson(saved)+",\"dependent_targets\":"+TrainingIdsJson(dependent)+'}';
        out.state=VerifyTrainingCast(quote,result,out.evidence);
        if(!SameTrainingState(ReadTrainingFrame(actor),result.after) || ReadTrainingSkills(actor)!=result.skillsAfter) {
            out.state=OperationState::Reconciling;out.evidence="training_cast_changed_before_save";
        }
        if(!persistenceReady){out.state=OperationState::Reconciling;out.evidence="training_dependency_save_proof_unavailable";}
        return out;
    }
    std::string PersistedProof(Player& actor,const Task& after) const override {
        if(!result.finished || result.uncertain || !SameTrainingState(ReadTrainingFrame(actor),result.after) || ReadTrainingSkills(actor)!=result.skillsAfter)
            throw std::runtime_error("training_cast_native_proof_missing");
        std::string proof="SELECT "+SqlValue(after.id)+','+std::to_string(after.revision)+" FROM characters c WHERE c.guid="+
            std::to_string(task.actor)+" AND c.money="+std::to_string(result.after.money);
        // Login-provided default spells need not have character_spell rows.
        // Full runtime spellbook conservation was checked above; the durable
        // predicate proves each promised target, not an invalid global count.
        std::set<uint32_t> saved,dependent;
        if(!TrainingSavedTargets(actor,quote,saved,dependent))throw std::runtime_error("training_dependency_save_proof_unavailable");
        for(auto id:quote.playerSpells)if(!dependent.count(id))proof+=(result.after.playerSpells.count(id)?" AND EXISTS(":" AND NOT EXISTS(")+
            std::string("SELECT 1 FROM character_spell s WHERE s.guid=c.guid AND s.spell=")+std::to_string(id)+" AND s.disabled=0)";
        if(quote.skill.id) {
            const auto found=result.skillsAfter.find(quote.skill.id);
            const bool exists=found!=result.skillsAfter.end();
            proof+=(exists?" AND EXISTS(":" AND NOT EXISTS(")+std::string("SELECT 1 FROM character_skills s WHERE s.guid=c.guid AND s.skill=")+
                std::to_string(quote.skill.id);
            if(exists)proof+=" AND s.value="+std::to_string(found->second.value)+" AND s.max="+std::to_string(found->second.maximum);
            proof+=')'; // Native login reconstructs the step from the saved max and DBC tier.
        }
        return proof;
    }
    std::unique_ptr<ExecutionScope> EnterEffect(Spell& spell) override {
        auto* actor=Actor(spell);std::string why;
        if(!actor || !result.started || result.effect || result.finished || result.uncertain || !Authority(*actor) ||
            !TrainingCastOfferReady(*actor,quote,why) || !SameTrainingState(ReadTrainingFrame(*actor),result.before) ||
            ReadTrainingSkills(*actor)!=result.skillsBefore)return {};
        result.effect=true;return std::make_unique<ExecutionScope>(task,action);
    }
    void Created(Spell&,uint32_t,uint32_t) noexcept override{result.uncertain=true;}
    void Finished(Spell& spell,bool success) noexcept override {
        try {
            if(result.finished)return;
            auto* actor=Actor(spell);if(!actor)result.uncertain=true;
            else {result.after=ReadTrainingFrame(*actor);result.skillsAfter=ReadTrainingSkills(*actor);}
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
NativeObservation ExecuteNativeDirectTrainingSkill(Player& actor,const TrainingLessonQuote& quote) {
    NativeObservation out;out.nativeReference="trainer_lesson:"+std::to_string(quote.lesson);
    if(!sLivingActivityCoordinator.OnWorldThread() || !DirectSkillTrainingQuote(quote)) {
        out.state=OperationState::Rejected;out.evidence="training_direct_skill_quote_required";return out;
    }
    const auto before=ReadTrainingFrame(actor);const auto skillsBefore=ReadTrainingSkills(actor);
    // The enclosing adapter just revalidated the complete skill quote. The
    // shared legacy primitive re-quotes native offer/subject/fee itself; its
    // quote has no managed skill projection. Execution is synchronous under
    // the same world-thread operation and save transaction.
    auto nativeQuote=quote;nativeQuote.skill={};
    ExecuteNativeTrainingLesson(*actor.GetPlayerbotAI(),nativeQuote);
    const auto after=ReadTrainingFrame(actor);const auto skillsAfter=ReadTrainingSkills(actor);
    out.state=VerifyDirectTrainingSkill(quote,before,after,skillsBefore,skillsAfter,out.evidence);
    out.afterState="{\"lesson\":"+std::to_string(quote.lesson)+",\"before\":"+TrainingFrameJson(before,quote)+
        ",\"after\":"+TrainingFrameJson(after,quote)+",\"skills_before\":"+TrainingSkillsJson(skillsBefore)+
        ",\"skills_after\":"+TrainingSkillsJson(skillsAfter)+'}';
    return out;
}
}
