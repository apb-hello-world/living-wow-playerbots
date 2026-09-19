#include "botpch.h"
#include "LivingNativeTraining.h"
#include "LivingActivityCoordinator.h"
#include "PlayerbotTrainingLesson.h"
#include "PlayerbotTraining.h"
#include "Spells/Scripts/SpellScript.h"
#include <stdexcept>

namespace LivingActivity {
namespace {
TrainingLessonState PetTrainingFrame(Player& actor) {
    TrainingLessonState frame;frame.actor=actor.GetGUIDLow();frame.money=actor.GetMoney();
    for(const auto& row:actor.GetSpellMap())
        if(row.second.state!=PLAYERSPELL_REMOVED && !row.second.disabled)frame.playerSpells.insert(row.first);
    if(auto* pet=actor.GetPet()) {
        frame.pet=pet->GetObjectGuid().GetRawValue();
        for(const auto& row:pet->m_spells)if(row.second.state!=PETSPELL_REMOVED)frame.petSpells.insert(row.first);
    }
    return frame;
}
std::string PetTrainingIds(const std::set<uint32_t>& ids) {
    std::string result="[";for(auto id:ids){if(result.size()>1)result+=',';result+=std::to_string(id);}return result+']';
}
}
bool CompleteNativePetTrainingQuote(Player& actor,TrainingLessonQuote& q,std::string& why) {
    auto reject=[&](const char* code){why=code;return false;};
    auto* pet=actor.GetPet();const auto* spell=sSpellTemplate.LookupEntry<SpellEntry>(q.teachingSpell);
    if(!sLivingActivityCoordinator.OnWorldThread() || !q.cast || q.cost || q.petSpells.size()!=1 || !q.playerSpells.empty() ||
        !pet || !pet->IsAlive() || pet->getPetType()!=HUNTER_PET || !pet->GetCharmInfo() ||
        pet->GetOwnerGuid()!=actor.GetObjectGuid() || pet->GetObjectGuid().GetRawValue()!=q.pet)
        return reject("training_exact_living_hunter_pet_required");
    // This finite native operation must finish inside its retained transaction.
    // Timed, scripted, channelled or resource-consuming casts remain deferred.
    if(!spell || SpellScriptMgr::GetSpellScript(q.teachingSpell) || IsChanneledSpell(spell) ||
        GetSpellCastTime(spell,&actor) || spell->speed || spell->manaCost || spell->manaCostPerlevel ||
        spell->ManaCostPercentage || spell->manaPerSecond || spell->manaPerSecondPerLevel)
        return reject("training_instant_pet_cast_required");
    unsigned effects=0;
    for(unsigned i=0;i<MAX_EFFECT_INDEX;++i)if(spell->Effect[i]) {
        if(++effects!=1 || spell->EffectTriggerSpell[i]!=q.petSpells[0] || spell->EffectImplicitTargetB[i] ||
            !((spell->Effect[i]==SPELL_EFFECT_LEARN_SPELL && spell->EffectImplicitTargetA[i]==TARGET_UNIT_CASTER_PET) ||
              (spell->Effect[i]==SPELL_EFFECT_LEARN_PET_SPELL && spell->EffectImplicitTargetA[i]==TARGET_UNIT_CASTER)))
            return reject("training_single_pet_learning_effect_required");
    }
    if(effects!=1)return reject("training_single_pet_learning_effect_required");
    for(unsigned i=0;i<MAX_SPELL_REAGENTS;++i)if(spell->Reagent[i]>0 && spell->ReagentCount[i]>0)
        return reject("training_pet_cast_reagents_unsupported");
    const uint32_t learned=q.petSpells[0];const auto* ability=sSpellTemplate.LookupEntry<SpellEntry>(learned);
    if(!ability || pet->HasSpell(learned) || pet->GetLevel()<ability->spellLevel ||
        !pet->HasTPForSpell(learned) || !pet->CanTakeMoreActiveSpells(learned))return reject("training_pet_native_prerequisite");
    q.petNumber=pet->GetCharmInfo()->GetPetNumber();q.petEntry=pet->GetEntry();q.petLevel=pet->GetLevel();
    q.petPoints=pet->m_TrainingPoints;q.petPointCost=pet->GetTPForSpell(learned);q.petReplacedSpell=0;
    for(const auto& row:pet->m_spells)if(row.second.state!=PETSPELL_REMOVED &&
        sSpellMgr.IsSpellAnotherRankOfSpell(learned,row.first)) {
        if(q.petReplacedSpell || !sSpellMgr.IsSpellHigherRankOfSpell(learned,row.first) || row.second.type==PETSPELL_FAMILY)
            return reject("training_pet_rank_reconciliation_required");
        q.petReplacedSpell=row.first;
    }
    if(!FreePetTrainingQuote(q))return reject("training_pet_quote_invalid");
    why.clear();return true;
}
NativeObservation ExecuteNativePetTraining(Player& actor,const TrainingLessonQuote& q) {
    NativeObservation out;out.nativeReference="trainer_lesson:"+std::to_string(q.lesson);
    auto current=q;std::string why;
    if(!CharacterDatabase.HasOpenTransaction() || !CompleteNativePetTrainingQuote(actor,current,why) || !SameTrainingLessonQuote(q,current)) {
        out.state=OperationState::Rejected;out.evidence="training_pet_transaction_and_fresh_quote_required";return out;
    }
    // Native SavePetToDB removes other unstabled hunter rows. Do not turn that
    // cleanup into unrecorded loss of another owned pet during a trainer task.
    const auto custody=CharacterDatabase.PQuery("SELECT COUNT(*) FROM character_pet WHERE owner=%u AND (slot=0 OR slot>2) AND id<>%u",q.actor,q.petNumber);
    if(!custody || custody->Fetch()[0].GetUInt32()) {
        out.state=OperationState::Rejected;out.evidence="training_pet_custody_requires_reconciliation";return out;
    }
    const auto before=PetTrainingFrame(actor);
    auto primitive=q;primitive.petNumber=primitive.petEntry=primitive.petLevel=primitive.petReplacedSpell=0;
    primitive.petPoints=primitive.petPointCost=0;
    ExecuteNativeTrainingLesson(*actor.GetPlayerbotAI(),primitive);
    const auto after=PetTrainingFrame(actor);auto* pet=actor.GetPet();
    const uint32_t number=pet && pet->GetCharmInfo()?pet->GetCharmInfo()->GetPetNumber():0;
    const int32_t points=pet?pet->m_TrainingPoints:0;
    out.state=VerifyPetTraining(q,before,after,q.petNumber,number,q.petPoints,points,out.evidence);
    if(!pet || pet->GetEntry()!=q.petEntry || pet->GetLevel()!=q.petLevel) {
        out.state=OperationState::Reconciling;out.evidence="training_pet_subject_changed";
    }
    out.afterState="{\"lesson\":"+std::to_string(q.lesson)+",\"actor\":"+std::to_string(actor.GetGUIDLow())+
        ",\"money_before\":"+std::to_string(before.money)+",\"money_after\":"+std::to_string(after.money)+
        ",\"pet_number\":"+std::to_string(number)+",\"points_before\":"+std::to_string(q.petPoints)+",\"points_after\":"+std::to_string(points)+
        ",\"owner_spells_before\":"+PetTrainingIds(before.playerSpells)+",\"owner_spells_after\":"+PetTrainingIds(after.playerSpells)+
        ",\"pet_spells_before\":"+PetTrainingIds(before.petSpells)+",\"pet_spells_after\":"+PetTrainingIds(after.petSpells)+'}';
    return out;
}
std::string PersistedPetTrainingProof(Player& actor,const TrainingLessonQuote& q,const Task& after) {
    const auto* pet=actor.GetPet();
    if(!pet || !pet->GetCharmInfo() || pet->GetCharmInfo()->GetPetNumber()!=q.petNumber ||
        pet->GetEntry()!=q.petEntry || pet->GetLevel()!=q.petLevel)throw std::runtime_error("training_pet_save_subject_changed");
    std::string proof="SELECT "+SqlValue(after.id)+','+std::to_string(after.revision)+
        " FROM characters c JOIN character_pet p ON p.owner=c.guid WHERE c.guid="+std::to_string(q.actor)+
        " AND c.money="+std::to_string(actor.GetMoney())+" AND p.id="+std::to_string(q.petNumber)+
        " AND p.entry="+std::to_string(q.petEntry)+" AND p.level="+std::to_string(q.petLevel)+
        " AND p.trainpoint="+std::to_string(pet->m_TrainingPoints)+" AND p.PetType=1";
    size_t count=0;
    for(const auto& row:pet->m_spells)if(row.second.state!=PETSPELL_REMOVED && row.second.type!=PETSPELL_FAMILY) {
        ++count;proof+=" AND EXISTS(SELECT 1 FROM pet_spell s WHERE s.guid=p.id AND s.spell="+
            std::to_string(row.first)+" AND s.active="+std::to_string(uint32_t(row.second.active))+')';
    }
    proof+=" AND (SELECT COUNT(*) FROM pet_spell s WHERE s.guid=p.id)="+std::to_string(count);
    return proof;
}
}
