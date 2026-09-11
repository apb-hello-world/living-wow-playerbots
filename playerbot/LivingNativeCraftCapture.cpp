#include "playerbot/playerbot.h"
#include "LivingNativeCraftCapture.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityGameplay.h"
#include "LivingActivityNativeContext.h"
#include "LivingProfessionNative.h"
#include "PlayerbotActionBroker.h"
#include "PlayerbotGuildSupplies.h"
#include "Skills/SkillExtraItems.h"
#include "Spells/Spell.h"
#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace LivingActivity {
    namespace {
        CraftIdentity Identity(const Task& task,const ActionContext& action,const ProfessionJob& job) {
            return {task.id,action.operation,task.revision,action.ownerGeneration,task.context,job.recipe,job.skill};
        }
        std::string FrameJson(const CraftFrame& frame) {
            std::string json="{\"skill\":"+std::to_string(frame.skill)+",\"money\":"+std::to_string(frame.money)+",\"stacks\":[";
            for (const auto& item : frame.stacks) {
                if (json.back()!='[') json+=',';
                json+='['+std::to_string(item.guid)+','+std::to_string(item.entry)+','+std::to_string(item.count)+','+
                    std::to_string(item.bagGuid)+','+std::to_string(item.slot)+']';
            }
            return json+"]}";
        }
    }
    bool ReadNativeCraftFrame(Player& actor,const ProfessionJob& job,CraftFrame& frame,std::string& blocker) {
        frame={};
        auto reject=[&](const char* code){blocker=code;return false;};
        if (!actor.GetPlayerbotAI() || !actor.IsInWorld() || actor.IsBeingTeleported())
            return reject("native_craft_actor_unavailable");
        std::set<uint32_t> relevant{job.outputEntry};
        for (const auto& reagent : job.reagents) relevant.insert(reagent.entry);
        frame.actor=actor.GetGUIDLow();frame.skill=actor.GetSkillValuePure(job.skill);frame.money=actor.GetMoney();
        unsigned inspected=0;
        for (auto* stack : actor.GetPlayerbotAI()->InventoryParseItems("inventory",IterateItemsMask::ITERATE_ITEMS_IN_BAGS)) {
            if (++inspected>256) return reject("native_craft_inventory_snapshot_bound");
            if (!stack || !relevant.count(stack->GetEntry())) continue;
            if (stack->GetOwnerGuid()!=actor.GetObjectGuid()) return reject("native_craft_inventory_owner_mismatch");
            frame.stacks.push_back({actor.GetGUIDLow(),stack->GetGUIDLow(),stack->GetEntry(),stack->GetCount(),
                stack->GetContainer() ? stack->GetContainer()->GetGUIDLow() : 0,stack->GetSlot()});
        }
        if (!ValidCraftFrame(frame)) return reject("native_craft_inventory_snapshot_invalid");
        blocker.clear();return true;
    }
    NativeCraftCast::NativeCraftCast(Task executing,ActionContext action,ProfessionJob job,
        std::vector<ClaimConsumption> consumption,ItemGainSpec output)
        : task(std::move(executing)),action(std::move(action)),job(std::move(job)),consumption(std::move(consumption)),
          output(output),identity(Identity(task,this->action,this->job)),capture(std::make_shared<CraftCapture>(identity)) {
        std::string blocker;
        if (!ValidateProfessionTask(task,blocker) || task.phase!=Phase::Executing || task.mode!=Mode::Active ||
            !task.accepted || !Fresh(task,this->action,task.context) ||
            this->action.revision!=task.revision || EncodeProfessionJob(this->job)!=task.checkpoint.data ||
            (SpellEffectMask(false)&~this->action.permittedEffects) || !ValidItemGainSpec(output) ||
            output.entry!=this->job.outputEntry || this->job.subjectItem ||
            (this->job.operation!=ProfessionOperation::CreateItem && this->job.operation!=ProfessionOperation::TransformMaterial))
            throw std::invalid_argument("native_craft_saved_execution_context_required");
    }
    Player* NativeCraftCast::Actor(Spell& spell) const {
        auto* caster=spell.GetTrueCaster();
        if (!caster || !caster->IsPlayer() || !spell.m_spellInfo || spell.m_spellInfo->Id!=job.recipe ||
            caster->GetGUIDLow()!=task.actor) return nullptr;
        return static_cast<Player*>(caster);
    }
    bool NativeCraftCast::AuthorityAllowed(Player& actor) const {
        auto* ai=actor.GetPlayerbotAI();if (!ai) return false;
        const auto reader=ai->ActivityPermissions();const auto view=reader.Inspect();
        if (!view) return false;
        const auto current=ReadNativeContext(actor,view->current.policyRevision,view->current.boot);
        const uint64_t now=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto safety=ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR));
        // Strict even when the ordinary migration policy is only observing.
        // Being a test/native callback does not manufacture execution authority.
        return reader.Check({SpellEffectMask(false),Lane::Managed,true},current,now,&task,&action,nullptr,safety)==AuthorityCode::Allowed;
    }
    bool NativeCraftCast::InputsAllowed(Player& actor,const CraftFrame& frame,std::string& blocker) const {
        auto reject=[&](const char* code){blocker=code;return false;};
        const auto protectedItems=sLivingActivityCoordinator.ResourceReservations().Inspect();
        if (!protectedItems || !protectedItems->ready) return reject("native_craft_claim_projection_unavailable");
        const auto trade=sPlayerbotActionBroker.ReservedItemsView();
        const auto supply=sGuildSupplies.ReservedItemsView();
        if (consumption.size()!=job.reagents.size()) return reject("native_craft_exact_input_claims_required");
        std::set<std::string> usedClaims;
        for (const auto& reagent : job.reagents) {
            const NativeItemStack* native=nullptr;
            for (const auto& stack : frame.stacks) if (stack.entry==reagent.entry) {
                // Native DestroyItemCount chooses the stack itself. Until the
                // shared splitter is available, never guess its mixed-stack order.
                if (native) return reject("native_craft_input_split_preparation_required");
                native=&stack;
            }
            if (!native || native->count<reagent.perAttempt) return reject("native_craft_material_missing");
            const ClaimConsumption* own=nullptr;
            for (const auto& use : consumption) if (use.before.itemGuid==native->guid) {
                if (own) return reject("native_craft_ambiguous_input_claim");
                own=&use;
            }
            if (!own || own->before.actor!=actor.GetGUIDLow() || own->before.task!=task.root ||
                own->before.itemEntry!=reagent.entry || own->before.state!="held" || own->before.location!="bags" ||
                own->before.quantity<reagent.perAttempt || own->before.quantity>native->count ||
                own->used!=reagent.perAttempt || !usedClaims.insert(own->before.id).second)
                return reject("native_craft_exact_input_claims_required");
            if (trade->Item(native->guid) || supply->Item(native->guid) || supply->Entry(actor.GetGUIDLow(),reagent.entry) ||
                protectedItems->HasUncertainItem(actor.GetGUIDLow(),reagent.entry) ||
                protectedItems->ProtectedItem(native->guid)!=own->before.quantity)
                return reject("native_craft_other_obligation_protects_material");
        }
        blocker.clear();return true;
    }
    bool NativeCraftCast::Attach(Spell& spell,std::string& blocker) {
        auto reject=[&](const char* code){blocker=code;return false;};
        auto* actor=Actor(spell);
        if (!sLivingActivityCoordinator.OnWorldThread() || !actor || !AuthorityAllowed(*actor))
            return reject("native_craft_current_authority_required");
        if (actor->GetTradeData() || actor->GetMap()->IsDungeon() || !actor->IsStopped() ||
            actor->CanNoReagentCast(spell.m_spellInfo)) return reject("native_craft_safe_resource_cast_required");
        if (actor->GetPlayerbotAI()->HasSpellItems(job.recipe,nullptr))
            return reject("native_craft_virtual_reagents_forbidden");
        const auto native=InspectNativeProfessionRecipe(*actor,job);auto currentJob=job;currentJob.initialSkill=native.skillValue;
        if (!MatchNativeProfessionRecipe(currentJob,native,blocker)) return false;
        if (job.purpose==ProfessionPurpose::SkillGain && native.skillValue>=job.targetSkill)
            return reject("native_craft_skill_goal_already_reached");
        unsigned outputs=0;uint32_t quantity=0;
        for (uint8_t i=0;i<MAX_EFFECT_INDEX;++i) {
            const auto* info=spell.m_spellInfo;
            if (!info->Effect[i]) continue;
            if (info->Effect[i]!=SPELL_EFFECT_CREATE_ITEM || info->EffectItemType[i]!=output.entry ||
                info->EffectDieSides[i]>1 || info->EffectDicePerLevel[i]!=0 || spell.GetSpellScript())
                return reject("native_craft_variable_or_scripted_output_unsupported");
            ++outputs;const auto* item=sObjectMgr.GetItemPrototype(output.entry);
            if (!item || !item->Stackable) return reject("native_craft_output_unavailable");
            const auto amount=spell.CalculateSpellEffectValue(SpellEffectIndex(i),actor,true,false);
            quantity=std::min(uint32_t(item->Stackable),uint32_t(std::max(1,amount)));
        }
        float chance=0;uint8_t extras=0;
        if (outputs!=1 || quantity!=output.quantity || canCreateExtraItems(actor,job.recipe,chance,extras))
            return reject("native_craft_exact_output_contract_required");
        ItemPosCountVec destinations;
        if (actor->CanStoreNewItem(NULL_BAG,NULL_SLOT,destinations,output.entry,output.quantity)!=EQUIP_ERR_OK)
            return reject("native_craft_capacity_preparation_required");
        CraftFrame before;
        if (!ReadNativeCraftFrame(*actor,job,before,blocker) || !InputsAllowed(*actor,before,blocker)) return false;
        if (before.stacks.size()>32) return reject("native_craft_frame_proof_bound");
        bool unused=false;
        if (!attached.compare_exchange_strong(unused,true)) return reject("native_craft_binding_already_used");
        if (!capture->Start(identity,std::move(before)) || !spell.SetLivingCraftCast(shared_from_this())) {
            capture->Abandon();return reject("native_craft_binding_failed");
        }
        blocker.clear();return true;
    }
    bool NativeCraftCast::Start(Player& actor,std::string& blocker) {
        if (!sLivingActivityCoordinator.OnWorldThread() || actor.GetGUIDLow()!=task.actor ||
            !actor.HasSpell(job.recipe) || actor.IsNonMeleeSpellCasted(false,true,true)) {
            blocker="native_craft_launch_prerequisite_changed";return false;
        }
        const auto* info=sSpellTemplate.LookupEntry<SpellEntry>(job.recipe);
        if (!info) {blocker="native_craft_recipe_missing";return false;}
        std::unique_ptr<Spell> spell(new Spell(&actor,info,false));
        if (!Attach(*spell,blocker)) return false;
        SpellCastTargets targets;targets.setUnitTarget(&actor);
        // SpellStart installs its native event before PreCastCheck. The event
        // owns deletion even when the cast is rejected or finishes immediately.
        spell.release()->SpellStart(&targets);
        blocker.clear();return true;
    }
    NativeObservation NativeCraftCast::Observe(Player& actor,const CraftCaptureResult& result,
        std::vector<VerifiedItemGain>& gains) const {
        NativeObservation observation;
        observation.nativeReference="spell:"+std::to_string(job.recipe)+":operation:"+action.operation;
        observation.afterState="{\"recipe\":"+std::to_string(job.recipe)+",\"skill_id\":"+std::to_string(job.skill)+
            ",\"effect_entered\":"+(result.effectEntered ? "true" : "false")+
            ",\"native_finished\":"+(result.nativeFinished ? "true" : "false")+
            ",\"native_succeeded\":"+(result.nativeSucceeded ? "true" : "false")+
            ",\"created_calls\":"+std::to_string(result.createdCalls)+",\"created_quantity\":"+std::to_string(result.createdQuantity)+
            ",\"before\":"+FrameJson(result.before)+",\"after\":"+FrameJson(result.after)+'}';
        const auto verified=VerifyCraftCapture(identity,job,result,output);
        observation.evidence=verified.blocker;
        if (verified.result==CraftEvidence::Reconciling) return observation;
        CraftFrame current;std::string blocker;
        if (!ReadNativeCraftFrame(actor,job,current,blocker) || !SameCraftFrame(result.after,current)) {
            observation.evidence="native_craft_changed_before_save";return observation;
        }
        if (verified.result==CraftEvidence::RejectedWithoutEffect) observation.state=OperationState::Rejected;
        else {
            observation.state=OperationState::Verified;gains=verified.gains;
            observation.evidence="native_craft_consumption_output_and_skill_observed";
        }
        return observation;
    }
    std::string NativeCraftCast::PersistedProof(Player& actor,const Task& outcome) const {
        const auto result=capture->ReadFinished();
        if (!result || !result->nativeFinished) throw std::runtime_error("native_craft_saved_frame_missing");
        CraftFrame current;std::string blocker;
        if (!ReadNativeCraftFrame(actor,job,current,blocker) || !SameCraftFrame(result->after,current))
            throw std::runtime_error("native_craft_changed_before_commit");
        std::string proof="SELECT "+SqlValue(outcome.id)+','+std::to_string(outcome.revision)+
            " FROM characters WHERE guid="+std::to_string(actor.GetGUIDLow())+" AND money="+std::to_string(result->after.money)+
            " AND EXISTS (SELECT 1 FROM character_skills WHERE guid="+std::to_string(actor.GetGUIDLow())+
            " AND skill="+std::to_string(job.skill)+" AND value="+std::to_string(result->after.skill)+')';
        std::set<uint32_t> present;
        for (const auto& item : result->after.stacks) {
            present.insert(item.guid);
            proof+=" AND EXISTS (SELECT 1 FROM character_inventory v JOIN item_instance i ON i.guid=v.item WHERE v.guid="+
                std::to_string(item.actor)+" AND v.item="+std::to_string(item.guid)+" AND v.item_template="+std::to_string(item.entry)+
                " AND v.bag="+std::to_string(item.bagGuid)+" AND v.slot="+std::to_string(item.slot)+
                " AND i.owner_guid="+std::to_string(item.actor)+" AND i.itemEntry="+std::to_string(item.entry)+
                " AND i.count="+std::to_string(item.count)+')';
        }
        for (const auto& item : result->before.stacks) if (!present.count(item.guid))
            proof+=" AND NOT EXISTS (SELECT 1 FROM character_inventory WHERE item="+std::to_string(item.guid)+
                ") AND NOT EXISTS (SELECT 1 FROM item_instance WHERE guid="+std::to_string(item.guid)+')';
        return proof;
    }
    uint32_t NativeCraftOperation::OperationEffects() const {return SpellEffectMask(false);}
    bool NativeCraftOperation::ValidateNative(Player& actor,const OperationRequest& request,std::string& blocker) {
        auto reject=[&](const char* code){blocker=code;return false;};
        ProfessionJob job;
        if (!ValidateProfessionTask(request.transition.task,blocker) ||
            !DecodeProfessionJob(request.transition.task.checkpoint.data,job,blocker)) return false;
        if (request.transition.task.actor!=actor.GetGUIDLow() || request.kind!=OperationKind() ||
            request.effects!=OperationEffects() || request.persistence!=PersistencePolicy() ||
            (job.operation!=ProfessionOperation::CreateItem && job.operation!=ProfessionOperation::TransformMaterial) ||
            job.subjectItem || !ValidItemGainSpec(request.itemGain) || request.itemGain.entry!=job.outputEntry ||
            request.consumption.size()!=job.reagents.size()) return reject("native_craft_exact_operation_required");
        if (!actor.GetPlayerbotAI() || !actor.IsInWorld() || actor.IsBeingTeleported() || actor.GetMap()->IsDungeon() ||
            ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) || actor.GetTradeData() ||
            !actor.IsStopped() || actor.IsNonMeleeSpellCasted(false,true,true)) return reject("native_craft_safety_prerequisite");
        const auto native=InspectNativeProfessionRecipe(actor,job);auto current=job;current.initialSkill=native.skillValue;
        if (!MatchNativeProfessionRecipe(current,native,blocker)) return false;
        if (job.purpose==ProfessionPurpose::SkillGain && native.skillValue>=job.targetSkill)
            return reject("native_craft_skill_goal_already_reached");
        CraftFrame frame;
        if (!ReadNativeCraftFrame(actor,job,frame,blocker)) return false;
        if (frame.stacks.size()>32) return reject("native_craft_frame_proof_bound");
        for (const auto& reagent : job.reagents) {
            unsigned stacks=0,claims=0;
            for (const auto& stack : frame.stacks) if (stack.entry==reagent.entry) {
                ++stacks;
                for (const auto& use : request.consumption) if (use.before.itemGuid==stack.guid &&
                    use.before.itemEntry==reagent.entry && use.before.actor==actor.GetGUIDLow() &&
                    use.before.task==request.transition.task.root && use.before.location=="bags" &&
                    use.before.state=="held" && use.used==reagent.perAttempt && use.before.quantity>=use.used &&
                    stack.count>=use.before.quantity) ++claims;
            }
            if (stacks!=1 || claims!=1) return reject("native_craft_exact_input_preparation_required");
        }
        blocker.clear();return true;
    }
    std::shared_ptr<NativeCraftCast> NativeCraftOperation::ReserveNativeCast(const OperationRequest& request,
        const Task& executing,const ActionContext& action) const {
        ProfessionJob job;std::string blocker;
        if (!DecodeProfessionJob(executing.checkpoint.data,job,blocker)) throw std::invalid_argument(blocker);
        return std::make_shared<NativeCraftCast>(executing,action,job,request.consumption,request.itemGain);
    }
    std::unique_ptr<ExecutionScope> NativeCraftCast::EnterEffect(Spell& spell) {
        auto* actor=Actor(spell);std::string blocker;CraftFrame current;
        if (!actor || !attached.load() || !AuthorityAllowed(*actor) || !actor->HasSpell(job.recipe) ||
            actor->GetTradeData() || !actor->IsStopped() ||
            !ReadNativeCraftFrame(*actor,job,current,blocker) || !InputsAllowed(*actor,current,blocker)) return {};
        if (!capture->EnterEffect(identity,current)) return {};
        return std::make_unique<ExecutionScope>(task,action);
    }
    void NativeCraftCast::Created(Spell& spell,uint32_t entry,uint32_t quantity) noexcept {
        try {if (Actor(spell)) capture->Created(identity,entry,quantity);else capture->Abandon();}
        catch (...) {capture->Abandon();}
    }
    void NativeCraftCast::Finished(Spell& spell,bool succeeded) noexcept {
        try {
            auto* actor=Actor(spell);CraftFrame after;std::string blocker;
            if (!actor || !ReadNativeCraftFrame(*actor,job,after,blocker)) {capture->Abandon();return;}
            capture->Finish(identity,succeeded,std::move(after));
        } catch (...) {capture->Abandon();}
    }
    void NativeCraftCast::Abandon() noexcept {capture->Abandon();}
    NativeCraftFinishGuard::~NativeCraftFinishGuard() noexcept {
        if (const auto& managed=spell.GetLivingCraftCast()) managed->Finished(spell,succeeded);
    }
}
