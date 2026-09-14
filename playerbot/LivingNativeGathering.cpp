#include "botpch.h"
#include "LivingNativeGathering.h"
#include "LivingNativeSkinning.h"
#include "LivingNativeLootCollection.h"
#include "LivingGathering.h"
#include "LivingGuildProcurement.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityNativeContext.h"
#include "LivingActivityGameplay.h"
#include "LivingProfessionDemand.h"
#include "LivingServiceExecution.h"
#include "LootObjectStack.h"
#include "TravelMgr.h"
#include "ServerFacade.h"
#include "strategy/values/SharedValueContext.h"
#include "strategy/values/ItemUsageValue.h"
#include "Spells/Spell.h"
#include "Spells/Scripts/SpellScript.h"
#include <chrono>
#include <mutex>

namespace LivingActivity {
namespace {
bool GatherLock(uint32_t entry,uint32_t& skill,uint32_t& required) {
    skill=required=0;const auto* info=sObjectMgr.GetGameObjectInfo(entry);
    if(!info || info->type!=GAMEOBJECT_TYPE_CHEST || sObjectMgr.IsGameObjectForQuests(entry))return false;
    const auto* lock=sLockStore.LookupEntry(info->GetLockId());if(!lock)return false;
    for(unsigned i=0;i<8;++i)if(lock->Type[i]==LOCK_KEY_SKILL) {
        const auto candidate=SkillByLockType(LockType(lock->Index[i]));
        if(candidate!=SKILL_MINING && candidate!=SKILL_HERBALISM)continue;
        if(skill)return false;
        skill=candidate;required=std::max(1u,lock->Skill[i]);
    }
    return skill!=0;
}
bool DirectDrop(uint32_t node,uint32_t entry) {
    const auto* loot=ai::DropMapValue::GetLootTemplate(ObjectGuid(HIGHGUID_GAMEOBJECT,node,uint32_t(1)),LOOT_CORPSE);
    if(!loot)return false;
    // Do not mistake an item inside a container or a reference-only/scripted
    // table for a directly supported node yield.
    for(const auto& item:loot->Entries)if(item.itemid==entry && item.chance>0)return true;
    for(const auto& group:loot->Groups) {
        for(const auto& item:group.ExplicitlyChanced)if(item.itemid==entry && item.chance>0)return true;
        for(const auto& item:group.EqualChanced)if(item.itemid==entry)return true;
    }
    return false;
}
bool LocalGatherQuote(Player& actor,uint64_t source,uint32_t entry,NativeGatherQuote& q,std::string& why,bool effect=false) {
    q={};auto reject=[&](const char* s){why=s;return false;};
    auto* ai=actor.GetPlayerbotAI();const ObjectGuid guid(source);
    if(!ai || !actor.GetMap() || !actor.IsInWorld() || actor.GetGroup() || actor.GetMap()->IsDungeon() ||
        actor.GetTradeData() || !actor.IsStopped() ||
        ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)))return reject("gather_safety_pause");
    const bool skinning=guid.IsCreature();
    if(skinning) {
        if(!LocalNativeSkinningQuote(actor,source,entry,q,why))return false;
        if(ai::ItemUsageValue::IsNeededForQuest(&actor,entry,true))return reject("gather_native_permission_or_tools_missing");
        const auto* info=sSpellTemplate.LookupEntry<SpellEntry>(8613);
        if(!info || info->Id!=8613 || IsChanneledSpell(info) || info->Effect[0]!=SPELL_EFFECT_SKINNING ||
            info->Effect[1] || info->Effect[2])return reject("skinning_spell_effect_unsupported");
        for(unsigned i=0;i<MAX_SPELL_REAGENTS;++i)if(info->Reagent[i]>0 && info->ReagentCount[i]>0)
            return reject("gather_consumable_tool_adapter_required");
        if(!effect && actor.IsNonMeleeSpellCasted(false,true,true))return reject("gather_native_cast_unavailable");
        why.clear();return true;
    }
    auto* node=guid.IsGameObject()?ai->GetGameObject(guid):nullptr;uint32_t skill=0,required=0;
    if(!node || !sServerFacade.isSpawned(node) || node->IsInUse() || node->m_loot ||
        !actor.IsWithinDistInMap(node,INTERACTION_DISTANCE) || !GatherLock(node->GetEntry(),skill,required) ||
        !DirectDrop(node->GetEntry(),entry))return reject("gather_node_changed_or_unavailable");
    const uint32_t spell=skill==SKILL_MINING?2575:2366;
    if(!actor.HasSpell(spell) || CheckGatheringSkill(actor.GetSkillValue(skill),actor.GetSkillMax(skill),required,false,
        GatheringIntent::RequestedMaterials)!=GatheringSkillResult::Eligible)return reject("gather_skill_or_spell_unavailable");
    ai::LootObject target(&actor,guid);
    if(target.IsEmpty() || target.skillId!=skill || !target.IsLootPossible(&actor) ||
        ai::ItemUsageValue::IsNeededForQuest(&actor,entry,true))return reject("gather_native_permission_or_tools_missing");
    const auto* info=sSpellTemplate.LookupEntry<SpellEntry>(spell);
    if(!info || info->Id!=spell || IsChanneledSpell(info))return reject("gather_spell_unsupported");
    // These two fixed native base spells are not generic scripted recipes.
    // Pinned TBC mining/herbalism both use OPEN_LOCK + SKILL (the latter is a
    // native no-op) and GameobjectCallForHelpOnUsage. Preserve that ordinary
    // aggro script: if it starts combat, the effect callback's safety check
    // pauses this attempt. Do not suppress the script or invent a skill gain.
    if(info->Effect[0]!=SPELL_EFFECT_OPEN_LOCK || info->Effect[1]!=SPELL_EFFECT_SKILL || info->Effect[2] ||
        SkillByLockType(LockType(info->EffectMiscValue[0]))!=skill || uint32_t(info->EffectMiscValue[1])!=skill)
        return reject("gather_spell_effect_unsupported");
    for(unsigned i=0;i<MAX_SPELL_REAGENTS;++i)if(info->Reagent[i]>0 && info->ReagentCount[i]>0)
        return reject("gather_consumable_tool_adapter_required");
    if(!effect && actor.IsNonMeleeSpellCasted(false,true,true))return reject("gather_native_cast_unavailable");
    q={actor.GetGUIDLow(),entry,skill,spell,required,actor.GetSkillValuePure(skill),actor.GetSkillMaxPure(skill),
        actor.GetSkillValue(skill),actor.GetMoney(),actor.GetItemCount(entry,false),source};
    if(!ValidNativeGatherQuote(q))return reject("gather_native_quote_invalid");
    why.clear();return true;
}
uint64_t LootGeneration(const Loot& loot) {
    const auto value=std::chrono::duration_cast<std::chrono::microseconds>(loot.GetCreateTime().time_since_epoch()).count();
    return value>0?uint64_t(value):0;
}
bool ReadGatherAfter(Player& actor,NativeGatherResult& r) {
    if(actor.GetGUIDLow()!=r.before.actor || !actor.IsInWorld() || actor.IsBeingTeleported())return false;
    r.value=actor.GetSkillValuePure(r.before.skill);r.maximum=actor.GetSkillMaxPure(r.before.skill);
    r.money=actor.GetMoney();r.bagCount=actor.GetItemCount(r.before.entry,false);r.generation=0;r.owned=false;
    auto* ai=actor.GetPlayerbotAI();const ObjectGuid guid(r.before.source);Loot* loot=nullptr;
    if(r.before.skill==393) {auto* corpse=ai->GetCreature(guid);if(corpse)loot=corpse->m_loot;}
    else {auto* node=ai->GetGameObject(guid);if(node)loot=node->m_loot;}
    if(loot) {
        r.generation=LootGeneration(*loot);
        r.owned=loot->GetLootGuid().GetRawValue()==r.before.source &&
            loot->GetLootType()==(r.before.skill==393?LOOT_SKINNING:LOOT_CORPSE) &&
            loot->GetOwnerSet().count(actor.GetObjectGuid())!=0;
    }
    return true;
}
class NativeGatherCast final : public NativeCraftCast {
public:
    NativeGatherCast(Task task,ActionContext action,NativeGatherQuote quote):task(std::move(task)),action(std::move(action)),quote(quote) {
        result.before=quote;
        if(!ValidNativeGatherQuote(quote) || this->task.actor!=quote.actor || this->task.phase!=Phase::Executing ||
            !IsGuildProcurementTask(this->task) || !Fresh(this->task,this->action,this->task.context) ||
            this->action.revision!=this->task.revision || (SpellEffectMask(false)&~this->action.permittedEffects))
            throw std::invalid_argument("gather_saved_binding_required");
    }
    bool Start(Player& actor,std::string& why) override {
        NativeGatherQuote current;
        if(!sLivingActivityCoordinator.OnWorldThread() || !Authority(actor) ||
            !LocalGatherQuote(actor,result.before.source,result.before.entry,current,why) ||
            EncodeNativeGatherQuote(current)!=EncodeNativeGatherQuote(result.before))return false;
        const ObjectGuid guid(current.source);
        auto* node=current.skill==393?nullptr:actor.GetPlayerbotAI()->GetGameObject(guid);
        auto* corpse=current.skill==393?actor.GetPlayerbotAI()->GetCreature(guid):nullptr;
        const auto* info=sSpellTemplate.LookupEntry<SpellEntry>(current.spell);
        if((!node && !corpse) || !info){why="gather_source_disappeared";return false;}
        std::unique_ptr<Spell> spell(new Spell(&actor,info,false));spell->m_clientCast=true;
        if(!spell->SetLivingCraftCast(shared_from_this())){why="gather_cast_binding_failed";return false;}
        {std::lock_guard<std::mutex> lock(mutex);if(result.started)return false;result.started=true;}
        SpellCastTargets targets;
        if(corpse)targets.setUnitTarget(corpse);else targets.setGOTarget(node);
        spell.release()->SpellStart(&targets);why.clear();return true;
    }
    bool Ready() const override {const auto r=Snapshot();return r.finished || r.uncertain;}
    NativeObservation Observe(Player& actor,std::vector<VerifiedItemGain>& gains) const override {
        gains.clear();auto r=Snapshot(),current=r;NativeObservation out;
        out.nativeReference="gather:"+std::to_string(r.before.source)+":generation:"+std::to_string(r.generation);
        out.state=VerifyNativeGatherResult(r,out.evidence);
        if(!ReadGatherAfter(actor,current) || EncodeNativeGatherResult(current)!=EncodeNativeGatherResult(r)) {
            out.state=OperationState::Reconciling;out.evidence="gather_changed_before_save";
        }
        out.afterState=EncodeNativeGatherResult(r);return out;
    }
    std::string PersistedProof(Player& actor,const Task& outcome) const override {
        auto r=Snapshot(),current=r;
        if(!r.finished || r.uncertain || !ReadGatherAfter(actor,current) || EncodeNativeGatherResult(current)!=EncodeNativeGatherResult(r))
            throw std::runtime_error("gather_native_proof_missing");
        return "SELECT "+SqlValue(outcome.id)+','+std::to_string(outcome.revision)+" FROM characters WHERE guid="+
            std::to_string(r.before.actor)+" AND money="+std::to_string(r.money)+
            " AND EXISTS(SELECT 1 FROM character_skills WHERE guid="+std::to_string(r.before.actor)+
            " AND skill="+std::to_string(r.before.skill)+" AND value="+std::to_string(r.value)+" AND max="+std::to_string(r.maximum)+')';
    }
    std::unique_ptr<ExecutionScope> EnterEffect(Spell& spell) override {
        auto r=Snapshot();auto* actor=Actor(spell);NativeGatherQuote current;std::string why;
        if(!actor || !r.started || r.effect || r.finished || r.uncertain || !Authority(*actor) ||
            !LocalGatherQuote(*actor,r.before.source,r.before.entry,current,why,true) ||
            EncodeNativeGatherQuote(current)!=EncodeNativeGatherQuote(r.before))return {};
        {std::lock_guard<std::mutex> lock(mutex);if(result.effect || result.finished || result.uncertain)return {};result.effect=true;}
        return std::make_unique<ExecutionScope>(task,action);
    }
    void Created(Spell&,uint32_t,uint32_t) noexcept override {std::lock_guard<std::mutex> lock(mutex);result.uncertain=true;}
    void Finished(Spell& spell,bool succeeded) noexcept override {
        try {
            auto r=Snapshot();if(r.finished)return;auto* actor=Actor(spell);
            if(!actor || !ReadGatherAfter(*actor,r))r.uncertain=true;
            r.finished=true;r.succeeded=succeeded;
            std::lock_guard<std::mutex> lock(mutex);r.uncertain|=result.uncertain;result=r;
        } catch(...) {std::lock_guard<std::mutex> lock(mutex);result.uncertain=true;}
    }
    void Abandon() noexcept override {std::lock_guard<std::mutex> lock(mutex);if(result.started && !result.finished)result.uncertain=true;}
private:
    NativeGatherResult Snapshot() const {std::lock_guard<std::mutex> lock(mutex);return result;}
    Player* Actor(Spell& spell) const {
        auto* caster=spell.GetTrueCaster();return caster && caster->IsPlayer() && caster->GetGUIDLow()==task.actor &&
            spell.m_spellInfo && spell.m_spellInfo->Id==quote.spell?static_cast<Player*>(caster):nullptr;
    }
    bool Authority(Player& actor) const {
        auto* ai=actor.GetPlayerbotAI();if(!ai)return false;const auto reader=ai->ActivityPermissions();const auto view=reader.Inspect();
        if(!view)return false;
        const auto current=ReadNativeContext(actor,view->current.policyRevision,view->current.boot);
        const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        return reader.Check({SpellEffectMask(false),Lane::Managed,true},current,now,&task,&action,nullptr,
            ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)))==AuthorityCode::Allowed;
    }
    const Task task;const ActionContext action;const NativeGatherQuote quote;mutable std::mutex mutex;NativeGatherResult result;
};
}
bool NativeGatherSources(Player& actor,uint32_t entry,std::vector<int32_t>& out,uint32_t& purpose,std::string& why) {
    out.clear();purpose=0;auto reject=[&](const char* code){why=code;return false;};
    if(!sLivingActivityCoordinator.OnWorldThread() || !actor.GetPlayerbotAI() || !actor.IsInWorld() || !actor.GetMap() ||
        actor.GetGroup() || actor.GetMap()->IsDungeon())return reject("gather_solo_outdoor_source_required");
    const auto* item=sObjectMgr.GetItemPrototype(entry);
    if(!item || item->Bonding!=NO_BIND)return reject("gather_tradeable_request_item_required");
    if(ai::ItemUsageValue::IsNeededForQuest(&actor,entry,true))return reject("gather_requested_item_protected_for_quest");
    const auto entries=GAI_VALUE2(std::list<int32>,"item drop list",entry);
    if(entries.size()>4096)return reject("gather_source_catalog_bound");
    for(const auto candidate:entries) {
        if(candidate>=0 || candidate==INT32_MIN)continue;
        uint32_t skill=0,required=0;
        if(!GatherLock(uint32_t(-candidate),skill,required) || !DirectDrop(uint32_t(-candidate),entry) ||
            !actor.HasSpell(skill==SKILL_MINING?2575:2366) ||
            CheckGatheringSkill(actor.GetSkillValue(skill),actor.GetSkillMax(skill),required,false,
                GatheringIntent::RequestedMaterials)!=GatheringSkillResult::Eligible)continue;
        const auto next=uint32_t(skill==SKILL_MINING?ai::TravelDestinationPurpose::GatherMining:ai::TravelDestinationPurpose::GatherHerbalism);
        // One concrete profession per trip; stable table order, no mixed mask.
        if(!purpose)purpose=next;
        if(purpose==next)out.push_back(candidate);
    }
    std::sort(out.begin(),out.end());out.erase(std::unique(out.begin(),out.end()),out.end());
    if(out.empty()) {
        NativeSkinningSources(actor,entry,out);
        if(!out.empty())purpose=uint32_t(ai::TravelDestinationPurpose::GatherSkinning);
    }
    if(out.empty())return reject("gather_no_supported_native_source");
    why.clear();return true;
}
WorldObject* NativeGatherNode(Player& actor,uint32_t entry,const std::vector<int32_t>& sources) {
    if(!sources.empty() && sources.front()>0)return NativeSkinningNode(actor,entry,sources);
    auto* ai=actor.GetPlayerbotAI();if(!ai)return nullptr;GameObject* best=nullptr;float distance=FLT_MAX;unsigned scanned=0;
    for(const auto guid:ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("nearest game objects no los")->Get()) {
        if(++scanned>256)break;
        auto* node=ai->GetGameObject(guid);if(!node || !sServerFacade.isSpawned(node) || node->IsInUse() || node->m_loot ||
            !std::binary_search(sources.begin(),sources.end(),-int32_t(node->GetEntry())) || !DirectDrop(node->GetEntry(),entry))continue;
        if(ai->ShouldAvoidDeathArea(ai::WorldPosition(node)))continue;
        ai::LootObject loot(&actor,guid);if(loot.IsEmpty() || !loot.IsLootPossible(&actor))continue;
        const auto d=actor.GetDistance(node);
        if(d<distance || (d==distance && best && node->GetObjectGuid()<best->GetObjectGuid())){best=node;distance=d;}
    }
    return best;
}
bool InspectNativeGatherQuote(Player& actor,uint64_t source,uint32_t entry,NativeGatherQuote& q,std::string& why) {
    if(!sLivingActivityCoordinator.OnWorldThread()){why="world_thread_required";return false;}
    return LocalGatherQuote(actor,source,entry,q,why);
}
bool FindNativeRequestedLoot(Player& actor,uint32_t entry,NativeLootQuote& q,std::string& why) {
    auto* loot=sLootMgr.GetLoot(&actor);
    if(!loot){why="requested_native_loot_not_open";return false;}
    for(unsigned slot=0;slot<256;++slot) {
        const auto* item=loot->GetLootItemInSlot(slot);
        if(item && item->itemId==entry && item->IsAllowed(&actor,loot))
            return InspectNativeLootQuote(actor,loot->GetLootGuid().GetRawValue(),slot,q,why);
    }
    why="requested_native_loot_absent";return false;
}
bool HoldsManagedGatherLoot(PlayerbotAI& ai,uint64_t source) {
    const auto view=ai.ActivityPermissions().Inspect();if(!view || view->compatibility || !view->lease.generation)return false;
    const auto& task=view->root;GuildProcurementJob job;std::string why;const ObjectGuid guid(source);uint32_t skill=0,required=0;
    return task.mode==Mode::Active && task.accepted && !Terminal(task.phase) && IsGuildProcurementTask(task) &&
        DecodeGuildProcurementJob(task.checkpoint.data,job,why) && job.craft.empty() &&
        ((guid.IsGameObject() && GatherLock(guid.GetEntry(),skill,required) && DirectDrop(guid.GetEntry(),job.entry)) ||
         HoldsNativeSkinningLoot(ai,source,job.entry));
}
uint32_t NativeGatherOperation::OperationEffects() const {return SpellEffectMask(false);}
bool NativeGatherOperation::ValidateNative(Player& actor,const OperationRequest& request,std::string& why) {
    NativeGatherQuote quote,current;GuildProcurementJob job;
    if(request.kind!=OperationKind() || request.effects!=OperationEffects() || request.persistence!=PersistencePolicy() ||
        !request.itemGain.Empty() || !request.mailGain.Empty() || !request.itemTransfer.id.empty() || !request.consumption.empty() ||
        !DecodeNativeGatherQuote(request.beforeState,quote) || quote.actor!=actor.GetGUIDLow() ||
        !DecodeGuildProcurementJob(request.transition.task.checkpoint.data,job,why) || !job.craft.empty() || job.entry!=quote.entry) {
        why="gather_exact_saved_operation_required";return false;
    }
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(request.transition.task.id);
    if(!saved || saved->checkpoint.data!=request.transition.task.checkpoint.data ||
        (saved->revision!=request.transition.expectedRevision && saved->revision!=request.transition.task.revision) ||
        !sLivingActivityCoordinator.ValidateGuildProcurementDemand(*saved,why))return false;
    NativeProfessionDemand demand;if(!InspectNativeProfessionDemand(actor,*saved,demand)){why=demand.blocker;return false;}
    if(demand.stock.size()!=1 || demand.stock.front().entry!=job.entry ||
        uint64_t(demand.stock.front().bag)+demand.stock.front().bank+demand.stock.front().delivered+demand.stock.front().paidInTransit>=job.quantity) {
        why="gather_demand_already_covered";return false;
    }
    if(!InspectNativeGatherQuote(actor,quote.source,quote.entry,current,why))return false;
    if(EncodeNativeGatherQuote(current)!=request.beforeState){why="gather_native_quote_changed";return false;}
    why.clear();return true;
}
std::shared_ptr<NativeCraftCast> NativeGatherOperation::ReserveNativeCast(const OperationRequest& request,const Task& task,const ActionContext& action) const {
    NativeGatherQuote quote;if(!DecodeNativeGatherQuote(request.beforeState,quote))throw std::invalid_argument("gather_quote_invalid");
    return std::make_shared<NativeGatherCast>(task,action,quote);
}
}
