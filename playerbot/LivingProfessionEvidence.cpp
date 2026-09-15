#include "LivingProfessionEvidence.h"
#include "LivingCommissionJob.h"
#include "LivingGuildProcurement.h"
#include "LivingActivityGameplay.h"
#include "LivingActivityOperations.h"
#include "LivingEnchantIntent.h"
#include "LivingProfessionConsumption.h"
#include <boost/property_tree/json_parser.hpp>
#include <algorithm>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace LivingActivity {
namespace {
    using Tree=boost::property_tree::ptree;
    void Require(bool value,const char* code) {if (!value) throw std::invalid_argument(code);}
    void Object(const Tree& value,std::initializer_list<const char*> names) {
        Require(value.data().empty() && value.size()==names.size(),"stored_craft_object_shape_invalid");
        std::set<std::string> seen;
        for (const auto& field : value) {
            Require(seen.insert(field.first).second &&
                std::find_if(names.begin(),names.end(),[&](const char* name){return field.first==name;})!=names.end(),
                "stored_craft_object_field_invalid");
        }
    }
    std::string Scalar(const Tree& value) {
        Require(value.empty(),"stored_craft_scalar_required");return value.data();
    }
    template<class T=uint32_t> T Number(const Tree& value) {
        const auto text=Scalar(value);
        Require(!text.empty() && text.size()<=20 && (text.size()==1 || text[0]!='0'),"stored_craft_number_invalid");
        uint64_t number=0;
        for (char c : text) {
            Require(c>='0' && c<='9',"stored_craft_number_invalid");
            const auto digit=unsigned(c-'0');
            Require(number<=(std::numeric_limits<T>::max()-digit)/10,"stored_craft_number_overflow");
            number=number*10+digit;
        }
        return T(number);
    }
    bool Flag(const Tree& value) {
        const auto text=Scalar(value);Require(text=="true" || text=="false","stored_craft_flag_invalid");return text=="true";
    }
    Tree Parse(const std::string& json) {
        const auto start=json.find_first_not_of(" \t\r\n");
        Require(json.size()<=8192 && start!=std::string::npos && json[start]=='{',"stored_craft_evidence_bound");
        Tree tree;std::istringstream input(json);boost::property_tree::read_json(input,tree);return tree;
    }
    ItemGainSpec Gain(const Tree& value) {
        Object(value,{"entry","quantity"});ItemGainSpec gain{Number(value.get_child("entry")),Number(value.get_child("quantity"))};
        Require(ValidItemGainSpec(gain),"stored_craft_output_spec_invalid");return gain;
    }
    CraftFrame Frame(uint32_t actor,const Tree& value) {
        Object(value,{"skill","money","stacks"});CraftFrame frame;
        frame.actor=actor;frame.skill=Number<uint16_t>(value.get_child("skill"));frame.money=Number(value.get_child("money"));
        const auto& stacks=value.get_child("stacks");
        Require(stacks.data().empty() && stacks.size()<=32,"stored_craft_stack_bound");
        for (const auto& row : stacks) {
            Require(row.first.empty() && row.second.data().empty() && row.second.size()==5,"stored_craft_stack_shape_invalid");
            uint32_t fields[5];unsigned i=0;
            for (const auto& field : row.second) {
                Require(field.first.empty(),"stored_craft_stack_shape_invalid");fields[i++]=Number(field.second);
            }
            Require(fields[4]<=255,"stored_craft_slot_invalid");
            frame.stacks.push_back({actor,fields[0],fields[1],fields[2],fields[3],uint8_t(fields[4])});
        }
        Require(ValidCraftFrame(frame),"stored_craft_frame_invalid");return frame;
    }
    std::vector<ClaimConsumption> Inputs(const Task& task,const Tree& value) {
        Require(value.data().empty() && !value.empty() && value.size()<=16,"stored_craft_claim_bound");
        std::vector<ClaimConsumption> uses;std::string previous;
        for (const auto& row : value) {
            Require(row.first.empty(),"stored_craft_claim_array_required");const auto& p=row.second;
            Object(p,{"claim","task","actor","revision","item_guid","item_entry","quantity","copper","location","used"});
            ClaimConsumption use;auto& c=use.before;c.state="held";
            c.id=Scalar(p.get_child("claim"));c.task=Scalar(p.get_child("task"));c.actor=Number(p.get_child("actor"));
            c.revision=Number<uint64_t>(p.get_child("revision"));c.itemGuid=Number(p.get_child("item_guid"));
            c.itemEntry=Number(p.get_child("item_entry"));c.quantity=Number<uint64_t>(p.get_child("quantity"));
            c.copper=Number<uint64_t>(p.get_child("copper"));c.location=Scalar(p.get_child("location"));use.used=Number(p.get_child("used"));
            Require(ValidResourceClaim(c) && c.id>previous && c.actor==task.actor && c.task==task.root &&
                !c.copper && c.location=="bags" && c.quantity && use.used && use.used<=c.quantity &&
                c.revision<std::numeric_limits<uint64_t>::max()-1,
                "stored_craft_input_claim_invalid");
            previous=c.id;uses.push_back(std::move(use));
        }
        return uses;
    }
    void SameInputs(const std::vector<ClaimConsumption>& a,const std::vector<ClaimConsumption>& b) {
        Require(a.size()==b.size(),"stored_craft_claims_changed");
        for (size_t i=0;i<a.size();++i)
            Require(a[i].used==b[i].used && SameResourceClaim(a[i].before,b[i].before),"stored_craft_claims_changed");
    }
    void InputBacking(const Task& task,const ProfessionJob& job,const std::vector<ClaimConsumption>& uses,const CraftFrame& frame) {
        std::string blocker;
        Require(MatchProfessionInputClaims(task,job,frame,uses,blocker),"stored_craft_input_backing_mismatch");
    }
    void GainedBacking(const Tree& rows,const std::vector<VerifiedItemGain>& gains) {
        Require(rows.data().empty() && rows.size()==gains.size(),"stored_craft_gains_mismatch");std::set<uint32_t> seen;
        for (const auto& row : rows) {
            Require(row.first.empty(),"stored_craft_gain_array_required");const auto& p=row.second;
            Object(p,{"guid","bag","slot","before","after","added"});const auto guid=Number(p.get_child("guid"));
            const auto gain=std::find_if(gains.begin(),gains.end(),[&](const auto& item){return item.after.guid==guid;});
            Require(seen.insert(guid).second && gain!=gains.end(),"stored_craft_gain_identity_mismatch");
            Require(Number(p.get_child("bag"))==gain->after.bagGuid && Number(p.get_child("slot"))==gain->after.slot &&
                Number(p.get_child("before"))==gain->before.count && Number(p.get_child("after"))==gain->after.count &&
                Number(p.get_child("added"))==gain->added,"stored_craft_gain_quantity_mismatch");
        }
    }
    bool DecodeStoredEnchantProof(const Task& task,const StoredCraftOperation& row,const ProfessionJob& job,
        StoredCraftProof& result,std::string& blocker) {
        const auto& receipt=row.receipt;const bool verified=receipt.state==OperationState::Verified;
        if (!verified && receipt.evidence=="native_craft_intent_not_committed") {
            const auto recovered=Parse(row.afterState);
            const bool priorCapture=recovered.count("prior_observation")!=0;
            auto predecessor=task;predecessor.revision=receipt.taskRevision;predecessor.phase=Phase::Executing;
            predecessor.checkpoint.data=EncodeProfessionJob(job); // Read-only historical step projection.
            // The original typed root/history was validated above. This local
            // decoder view is never persisted or passed to an executor.
            predecessor.source="profession_job";predecessor.kind=Kind::Profession;
            predecessor.checkpoint.step="profession_craft";auto original=row;original.receipt.state=OperationState::Intent;
            original.receipt.evidence.clear();original.receipt.nativeReference.clear();original.afterState="{}";
            if(priorCapture) {
                ++predecessor.revision;predecessor.phase=Phase::Reconciling;
                predecessor.checkpoint.blocker="native_save_capture_requires_reconciliation";
                original.receipt.state=OperationState::Reconciling;original.receipt.evidence=predecessor.checkpoint.blocker;
                original.receipt.nativeReference=receipt.nativeReference;
                original.afterState=EnchantCodec::Json(recovered.get_child("prior_observation"));
            }
            InterruptedCraftIntent intent;
            if (!DecodeInterruptedCraftIntent(predecessor,original,intent,blocker) || !intent.enchant) return false;
            if(priorCapture) Object(recovered,{"recovery","frame","subject","prior_observation"});
            else Object(recovered,{"recovery","frame","subject"});
            const auto& basis=recovered.get_child("recovery");Object(basis,{"version","basis","boot"});
            const auto frame=Frame(task.actor,recovered.get_child("frame"));
            Require(Number(basis.get_child("version"))==1 && Scalar(basis.get_child("basis"))=="atomic_native_save_absent" &&
                IsUuid(Scalar(basis.get_child("boot"))) && receipt.nativeReference=="spell:"+std::to_string(job.recipe)+":operation:"+receipt.id &&
                frame.skill==intent.skill && frame.money==intent.money &&
                SameEnchantSubject(EnchantCodec::Subject(recovered.get_child("subject")),intent.enchant->before),
                "stored_enchant_recovery_changed");
            InputBacking(task,job,intent.inputs,frame);
            Require(MatchesInterruptedCraftInventory(intent,frame),"stored_enchant_recovery_quantity_changed");
            StoredCraftProof parsed;parsed.inputs=std::move(intent.inputs);parsed.attempt.recipe=job.recipe;
            parsed.attempt.subjectItem=job.subjectItem;parsed.attempt.skillBefore=parsed.attempt.skillAfter=frame.skill;
            parsed.attempt.receipt=receipt;parsed.attempt.committed=true;result=std::move(parsed);return true;
        }
        Require(receipt.nativeReference=="spell:"+std::to_string(job.recipe)+":operation:"+receipt.id &&
            receipt.evidence==(verified?"native_enchant_consumption_subject_and_skill_observed":"native_cast_cancelled_without_effect"),
            "stored_enchant_native_reference_mismatch");
        const auto before=Parse(row.beforeState),after=Parse(row.afterState);
        Object(before,{"effects","persistence","native"});Object(before.get_child("native"),{"native","claimed_consumption"});
        Require(Number(before.get_child("effects"))==SpellEffectMask(false) &&
            Number(before.get_child("persistence"))==unsigned(NativePersistence::Profession),"stored_enchant_persistence_mismatch");
        EnchantIntent intent;
        if (!DecodeEnchantIntent(task,job,EnchantCodec::Json(before.get_child("native.native")),intent,blocker)) return false;
        const auto inputs=Inputs(task,before.get_child("native.claimed_consumption"));
        const auto& captured=verified?after.get_child("native"):after;
        Object(captured,{"recipe","skill_id","effect_entered","native_finished","native_succeeded","created_calls","created_quantity",
            "before","after","enchantment","subject_claim","subject_before","subject_after"});
        const auto frameBefore=Frame(task.actor,captured.get_child("before")),frameAfter=Frame(task.actor,captured.get_child("after"));
        const auto subjectBefore=EnchantCodec::Subject(captured.get_child("subject_before"));
        const auto subjectAfter=EnchantCodec::Subject(captured.get_child("subject_after"));ResourceClaim subject;
        Require(Number(captured.get_child("recipe"))==job.recipe && Number(captured.get_child("skill_id"))==job.skill &&
            Number(captured.get_child("enchantment"))==intent.spec.id && frameBefore.skill==intent.skill && frameBefore.money==intent.money &&
            SameEnchantSubject(subjectBefore,intent.before) &&
            (!intent.inventoryBefore || SameCraftFrame(*intent.inventoryBefore,frameBefore)) &&
            DecodeClaimProjection(EnchantCodec::Json(captured.get_child("subject_claim")),subject,blocker) &&
            SameResourceClaim(subject,intent.claim),"stored_enchant_intent_changed");
        InputBacking(task,job,inputs,frameBefore);
        Require(Flag(captured.get_child("native_finished")) && !Number(captured.get_child("created_calls")) &&
            !Number(captured.get_child("created_quantity")),"stored_enchant_finish_or_creation_mismatch");
        StoredCraftProof parsed;
        if (verified) {
            Object(after,{"native","claimed_consumption"});SameInputs(inputs,Inputs(task,after.get_child("claimed_consumption")));
            Require(Flag(captured.get_child("native_succeeded")) && Flag(captured.get_child("effect_entered")),"stored_enchant_effect_missing");
            auto physical=VerifyEnchantResources(task.actor,job,intent.spec,frameBefore,frameAfter,subjectBefore,subjectAfter);
            if (physical.result!=CraftEvidence::Verified) {blocker=physical.blocker;return false;}
            parsed.attempt=std::move(physical.attempt);parsed.attempt.nativeEffectVerified=true;
        } else {
            Require(!Flag(captured.get_child("native_succeeded")) && !Flag(captured.get_child("effect_entered")) &&
                SameCraftFrame(frameBefore,frameAfter) && SameEnchantSubject(subjectBefore,subjectAfter),"stored_enchant_rejection_has_effect");
            parsed.attempt.recipe=job.recipe;parsed.attempt.subjectItem=job.subjectItem;
            parsed.attempt.skillBefore=frameBefore.skill;parsed.attempt.skillAfter=frameAfter.skill;
        }
        parsed.inputs=inputs;parsed.attempt.receipt=receipt;parsed.attempt.committed=true;
        result=std::move(parsed);blocker.clear();return true;
    }
}
static bool DecodeStoredCraftProofBody(const Task& task,const StoredCraftOperation& row,
    StoredCraftProof& result,std::string& blocker) {
    result={};blocker.clear();
    try {
        const auto& receipt=row.receipt;ProfessionJob job;std::string reason;bool current=false;
        Require(row.acknowledged,"stored_craft_receipt_not_acknowledged");
        Require(task.mode==Mode::Active && task.accepted && IsUuid(task.id) && task.root==task.id && task.actor &&
            ValidateProfessionTask(task,reason) && IsProfessionJob(task) &&
            ProfessionJobAtRevision(task,receipt.taskRevision,job,current,reason),"stored_craft_task_invalid");
        Require(IsUuid(receipt.id) && receipt.task==task.id && receipt.taskRevision && receipt.taskRevision<task.revision &&
            receipt.kind=="profession_craft","stored_craft_receipt_identity_mismatch");
        Require(receipt.state==OperationState::Verified || receipt.state==OperationState::Rejected,
            "stored_craft_operation_unresolved");
        if (job.operation==ProfessionOperation::EnchantItem)
            return DecodeStoredEnchantProof(task,row,job,result,blocker);
        const bool verified=receipt.state==OperationState::Verified;
        if (!verified && receipt.evidence=="native_craft_intent_not_committed") {
            auto predecessor=task;predecessor.revision=receipt.taskRevision;
            predecessor.phase=Phase::Executing;predecessor.checkpoint.step="profession_craft";
            predecessor.checkpoint.data=EncodeProfessionJob(job); // Read-only historical step projection.
            predecessor.source="profession_job";predecessor.kind=Kind::Profession; // Decoder-only, never written.
            auto intent=row;intent.receipt.state=OperationState::Intent;
            intent.receipt.nativeReference.clear();intent.receipt.evidence.clear();intent.afterState="{}";
            InterruptedCraftIntent decoded;
            if (!DecodeInterruptedCraftIntent(predecessor,intent,decoded,blocker)) return false;
            Require(receipt.nativeReference=="spell:"+std::to_string(job.recipe)+":operation:"+receipt.id,
                "stored_craft_native_reference_mismatch");
            const auto recovered=Parse(row.afterState);
            Object(recovered,{"recovery","frame"});const auto& basis=recovered.get_child("recovery");
            Object(basis,{"version","basis","boot"});
            Require(Number(basis.get_child("version"))==1 &&
                Scalar(basis.get_child("basis"))=="atomic_native_save_absent" &&
                IsUuid(Scalar(basis.get_child("boot"))),"stored_craft_recovery_basis_invalid");
            const auto frame=Frame(task.actor,recovered.get_child("frame"));
            Require(frame.skill==decoded.skill && frame.money==decoded.money,"stored_craft_recovery_state_changed");
            InputBacking(task,job,decoded.inputs,frame);
            Require(MatchesInterruptedCraftInventory(decoded,frame),"stored_craft_recovery_quantity_changed");
            StoredCraftProof parsed;parsed.inputs=std::move(decoded.inputs);
            parsed.attempt.recipe=job.recipe;parsed.attempt.skillBefore=parsed.attempt.skillAfter=frame.skill;
            parsed.attempt.receipt=receipt;parsed.attempt.committed=true;
            // A reconciled non-commit is NOT a native callback or skill credit.
            result=std::move(parsed);return true;
        }
        Require(receipt.nativeReference=="spell:"+std::to_string(job.recipe)+":operation:"+receipt.id &&
            receipt.evidence==(verified ? "native_craft_consumption_output_and_skill_observed" : "native_cast_cancelled_without_effect"),
            "stored_craft_native_reference_mismatch");
        Require((job.operation==ProfessionOperation::CreateItem || job.operation==ProfessionOperation::TransformMaterial) &&
            !job.subjectItem,"stored_craft_operation_unsupported");
        const auto before=Parse(row.beforeState);const auto after=Parse(row.afterState);
        Object(before,{"effects","persistence","native","item_gain"});
        Require(Number(before.get_child("effects"))==SpellEffectMask(false) &&
            Number(before.get_child("persistence"))==unsigned(NativePersistence::Profession),"stored_craft_native_persistence_mismatch");
        const auto output=Gain(before.get_child("item_gain"));
        Require(output.entry==job.outputEntry,"stored_craft_recipe_output_mismatch");
        Object(before.get_child("native"),{"native","claimed_consumption"});
        const auto& intended=before.get_child("native.native");ProfessionCastIntent intent;
        Require(DecodeCraftIntent(task.actor,job,EnchantCodec::Json(intended),intent,blocker),"stored_craft_intent_invalid");
        auto inputs=Inputs(task,before.get_child("native.claimed_consumption"));
        const auto& captured=verified ? after.get_child("native.result") : after;
        Object(captured,{"recipe","skill_id","effect_entered","native_finished","native_succeeded",
            "created_calls","created_quantity","before","after"});
        Require(Number(captured.get_child("recipe"))==job.recipe && Number(captured.get_child("skill_id"))==job.skill &&
            Number(intended.get_child("recipe"))==job.recipe,"stored_craft_recipe_identity_mismatch");
        const auto frameBefore=Frame(task.actor,captured.get_child("before"));
        const auto frameAfter=Frame(task.actor,captured.get_child("after"));
        Require(intent.skill==frameBefore.skill && intent.money==frameBefore.money &&
            (!intent.inventoryBefore || SameCraftFrame(*intent.inventoryBefore,frameBefore)),
            "stored_craft_intent_snapshot_mismatch");
        InputBacking(task,job,inputs,frameBefore);
        Require(Flag(captured.get_child("native_finished")),"stored_craft_native_finish_missing");
        StoredCraftProof parsed;
        if (verified) {
            Object(after,{"native","claimed_consumption"});Object(after.get_child("native"),{"result","item_gain","stacks"});
            SameInputs(inputs,Inputs(task,after.get_child("claimed_consumption")));
            const auto afterGain=Gain(after.get_child("native.item_gain"));
            Require(afterGain.entry==output.entry && afterGain.quantity==output.quantity,"stored_craft_output_contract_changed");
            Require(Flag(captured.get_child("native_succeeded")) && Flag(captured.get_child("effect_entered")) &&
                Number(captured.get_child("created_calls"))==1 && Number(captured.get_child("created_quantity"))==output.quantity,
                "stored_craft_creation_proof_mismatch");
            // Historical proof has no current lease/map generation. Reuse only
            // the physical validator; never fabricate a live CraftIdentity.
            auto physical=VerifyCraftResources(task.actor,job,frameBefore,frameAfter,output);
            if (physical.result!=CraftEvidence::Verified) {blocker=physical.blocker;return false;}
            GainedBacking(after.get_child("native.stacks"),physical.gains);
            parsed.gains=std::move(physical.gains);parsed.attempt=std::move(physical.attempt);
            parsed.attempt.nativeEffectVerified=true;
        } else {
            Require(!Flag(captured.get_child("native_succeeded")) && !Flag(captured.get_child("effect_entered")) &&
                !Number(captured.get_child("created_calls")) && !Number(captured.get_child("created_quantity")) &&
                SameCraftFrame(frameBefore,frameAfter),"stored_craft_rejection_has_possible_effect");
            parsed.attempt.recipe=job.recipe;parsed.attempt.skillBefore=frameBefore.skill;parsed.attempt.skillAfter=frameAfter.skill;
        }
        parsed.inputs=std::move(inputs);parsed.attempt.receipt=receipt;parsed.attempt.committed=true;
        result=std::move(parsed);return true;
    } catch (const std::invalid_argument& error) {blocker=error.what();}
      catch (const std::exception&) {blocker="stored_craft_evidence_malformed";}
    return false;
}

bool DecodeInterruptedCraftIntent(const Task& task,const StoredCraftOperation& row,
    InterruptedCraftIntent& result,std::string& blocker) {
    result={};blocker.clear();
    try {
        ProfessionJob job;std::string reason;const auto& r=row.receipt;
        Require(row.acknowledged && task.mode==Mode::Active && task.accepted && task.root==task.id &&
            task.parent.empty() && (task.phase==Phase::Executing || task.phase==Phase::Reconciling) && task.checkpoint.step=="profession_craft" &&
            ValidateProfessionTask(task,reason) && IsProfessionJob(task) &&
            DecodeProfessionJob(task.checkpoint.data,job,reason),"interrupted_craft_task_invalid");
        const bool captured=job.operation==ProfessionOperation::EnchantItem && task.phase==Phase::Reconciling &&
            r.state==OperationState::Reconciling && r.taskRevision<UINT64_MAX && r.taskRevision+1==task.revision &&
            r.evidence=="native_save_capture_requires_reconciliation" && task.checkpoint.blocker==r.evidence &&
            r.nativeReference=="spell:"+std::to_string(job.recipe)+":operation:"+r.id;
        const bool emptyAdapterFailure=task.phase==Phase::Reconciling && r.state==OperationState::Reconciling &&
            r.taskRevision<UINT64_MAX && r.taskRevision+1==task.revision &&
            r.evidence=="native_adapter_exception" && task.checkpoint.blocker==r.evidence &&
            r.nativeReference.empty() && row.afterState=="{}";
        Require(IsUuid(r.id) && r.task==task.id && r.kind=="profession_craft" &&
            (captured || emptyAdapterFailure || (task.phase==Phase::Executing && r.taskRevision==task.revision &&
             r.state==OperationState::Intent && r.evidence.empty() && r.nativeReference.empty() && row.afterState=="{}")),
            "interrupted_craft_not_pristine_intent");
        const bool enchant=job.operation==ProfessionOperation::EnchantItem;
        Require(enchant || ((job.operation==ProfessionOperation::CreateItem || job.operation==ProfessionOperation::TransformMaterial) &&
            !job.subjectItem),"interrupted_craft_operation_unsupported");
        const auto before=Parse(row.beforeState);
        if (enchant) Object(before,{"effects","persistence","native"});
        else Object(before,{"effects","persistence","native","item_gain"});
        Require(Number(before.get_child("effects"))==SpellEffectMask(false) &&
            Number(before.get_child("persistence"))==unsigned(NativePersistence::Profession),"interrupted_craft_atomic_contract_required");
        Object(before.get_child("native"),{"native","claimed_consumption"});
        const auto& native=before.get_child("native.native");InterruptedCraftIntent decoded;
        if (enchant) {
            EnchantIntent target;
            if (!DecodeEnchantIntent(task,job,EnchantCodec::Json(native),target,blocker)) return false;
            decoded.inventoryBefore=target.inventoryBefore;decoded.enchant=std::move(target);
        } else {
            ProfessionCastIntent cast;
            Require(DecodeCraftIntent(task.actor,job,EnchantCodec::Json(native),cast,blocker),"interrupted_craft_intent_invalid");
            decoded.inventoryBefore=std::move(cast.inventoryBefore);decoded.output=Gain(before.get_child("item_gain"));
        }
        decoded.skill=Number<uint16_t>(native.get_child("skill"));decoded.money=Number(native.get_child("money"));
        decoded.inputs=Inputs(task,before.get_child("native.claimed_consumption"));
        // An exception is not proof of non-execution. Recovery is possible only
        // after restart, under the atomic save contract above, with a complete
        // saved before-frame and unchanged native inventory/skill/money/claims.
        // Older aggregate-only intents and any possible after-effect stay held.
        Require(!emptyAdapterFailure || decoded.inventoryBefore.has_value(),"interrupted_craft_full_before_frame_required");
        if(captured) {
            // A failed native callback is not success. Only this exact
            // pre-effect capture gap is eligible for restored-state comparison;
            // possible effects or changed native possessions remain unresolved.
            const auto observed=Parse(row.afterState);
            Object(observed,{"recipe","skill_id","effect_entered","native_finished","native_succeeded","created_calls","created_quantity",
                "before","after","enchantment","subject_claim","subject_before","subject_after"});
            const auto original=Frame(task.actor,observed.get_child("before"));ResourceClaim held;
            Require(Number(observed.get_child("recipe"))==job.recipe && Number(observed.get_child("skill_id"))==job.skill &&
                !Flag(observed.get_child("effect_entered")) && !Flag(observed.get_child("native_finished")) &&
                !Flag(observed.get_child("native_succeeded")) && !Number(observed.get_child("created_calls")) &&
                !Number(observed.get_child("created_quantity")) && original.money==decoded.money && original.skill==decoded.skill &&
                Number(observed.get_child("enchantment"))==decoded.enchant->spec.id &&
                SameEnchantSubject(EnchantCodec::Subject(observed.get_child("subject_before")),decoded.enchant->before) &&
                DecodeClaimProjection(EnchantCodec::Json(observed.get_child("subject_claim")),held,blocker) &&
                SameResourceClaim(held,decoded.enchant->claim),"interrupted_enchant_capture_has_possible_effect");
            InputBacking(task,job,decoded.inputs,original);
            Require(!decoded.inventoryBefore || SameCraftFrame(*decoded.inventoryBefore,original),"interrupted_enchant_capture_before_changed");
            const auto& empty=observed.get_child("after");Object(empty,{"skill","money","stacks"});
            Require(!Number(empty.get_child("skill")) && !Number(empty.get_child("money")) && empty.get_child("stacks").empty(),
                "interrupted_enchant_capture_has_after_state");
            const auto& target=observed.get_child("subject_after");
            Object(target,{"actor","guid","entry","count","bag","slot","enchantments"});
            for(const auto* key:{"actor","guid","entry","count","bag","slot"})
                Require(!Number(target.get_child(key)),"interrupted_enchant_capture_has_after_subject");
            Require(target.get_child("enchantments").empty(),"interrupted_enchant_capture_has_after_subject");
        }
        Require(Number(native.get_child("recipe"))==job.recipe && (enchant || decoded.output.entry==job.outputEntry),
            "interrupted_craft_recipe_mismatch");
        size_t matched=0;
        for(const auto& need:job.reagents) {
            uint64_t used=0;
            for(const auto& use:decoded.inputs) if(use.before.itemEntry==need.entry) {
                used+=use.used;++matched;
            }
            Require(used==need.perAttempt,"interrupted_craft_recipe_mismatch");
        }
        Require(matched==decoded.inputs.size(),"interrupted_craft_recipe_mismatch");
        if(decoded.inventoryBefore)InputBacking(task,job,decoded.inputs,*decoded.inventoryBefore);
        result=std::move(decoded);return true;
    } catch(const std::invalid_argument& error) {blocker=error.what();}
      catch(const std::exception&) {blocker="interrupted_craft_evidence_malformed";}
    return false;
}

bool MatchesInterruptedCraftInventory(const InterruptedCraftIntent& intent,const CraftFrame& current) {
    if(!ValidCraftFrame(current) || current.skill!=intent.skill || current.money!=intent.money)return false;
    if(intent.inventoryBefore)return SameCraftFrame(*intent.inventoryBefore,current);
    std::map<uint32_t,uint64_t> quantities;
    for(const auto& use:intent.inputs)quantities[use.before.itemGuid]+=use.before.quantity;
    for(const auto& stack:current.stacks)if(quantities.count(stack.guid)) {
        if(stack.count!=quantities.at(stack.guid))return false;
        quantities.erase(stack.guid);
    }
    return quantities.empty();
}

bool DecodeStoredCraftProof(const Task& task,const StoredCraftOperation& row,StoredCraftProof& result,std::string& blocker) {
    if(!row.journalDigest.empty() && (row.journalDigest.size()!=64 ||
        row.journalDigest.find_first_not_of("0123456789abcdef")!=std::string::npos)) {
        result={};blocker="profession_history_digest_invalid";return false;
    }
    if(!DecodeStoredCraftProofBody(task,row,result,blocker))return false;
    for(const auto& gain:result.gains)result.attempt.gainedItems[gain.after.guid]+=gain.added;
    result.attempt.journalDigest=row.journalDigest;return true;
}
bool IsGatheringRecoveryTask(const Task& task) {
    GuildProcurementJob job;std::string why;
    return IsGuildProcurementTask(task) && task.phase==Phase::Reconciling &&
        task.checkpoint.step=="guild_requested_gather" &&
        task.checkpoint.blocker=="native_gather_outcome_uncertain" &&
        DecodeGuildProcurementJob(task.checkpoint.data,job,why) && job.craft.empty();
}
bool DecodeInterruptedCapacitySale(const Task& task,const StoredCraftOperation& row,InterruptedCapacitySale& out,std::string& why) {
    out={};why="capacity_restart_exact_intent_required";
    try {
        const auto& r=row.receipt;
        Require(IsProfessionJob(task) && task.accepted && task.mode==Mode::Active && task.root==task.id &&
            task.phase==Phase::Executing && task.checkpoint.step=="profession_capacity_sale" &&
            row.acknowledged && IsUuid(r.id) && r.task==task.id && r.taskRevision==task.revision &&
            r.kind=="capacity_vendor_sale" && r.state==OperationState::Intent && r.evidence.empty() &&
            r.nativeReference.empty() && row.afterState=="{}" && row.journalDigest.size()==64 &&
            row.journalDigest.find_first_not_of("0123456789abcdef")==std::string::npos,
            "capacity_restart_exact_intent_required");
        const auto before=Parse(row.beforeState);
        Object(before,{"effects","persistence","native"});Object(before.get_child("native"),{"native","claimed_consumption"});
        Require(Number(before.get_child("effects"))==(Mask(Effect::Inventory)|Mask(Effect::Money)) &&
            Number(before.get_child("persistence"))==unsigned(NativePersistence::Inventory),"capacity_restart_atomic_save_required");
        const auto uses=Inputs(task,before.get_child("native.claimed_consumption"));
        Require(uses.size()==1 && uses[0].used==uses[0].before.quantity,"capacity_restart_whole_claim_required");
        const auto& q=before.get_child("native.native");
        Object(q,{"actor","guid","entry","quantity","unit_copper","money","copper","count_before","vendor","vendor_entry","from","capacity_entry","capacity_quantity"});
        const auto& c=uses[0].before;
        const uint64_t price=uint64_t(Number(q.get_child("unit_copper")))*Number(q.get_child("quantity"));
        out.claim=c;out.money=Number(q.get_child("money"));out.entryCount=Number(q.get_child("count_before"));
        out.position=Number<uint16_t>(q.get_child("from"));
        Require(Number(q.get_child("actor"))==task.actor && Number(q.get_child("guid"))==c.itemGuid &&
            Number(q.get_child("entry"))==c.itemEntry && Number(q.get_child("quantity"))==c.quantity &&
            price && price==Number(q.get_child("copper")) && price+out.money<=uint64_t(INT32_MAX) &&
            out.entryCount>=c.quantity && Number<uint64_t>(q.get_child("vendor")) && Number(q.get_child("vendor_entry")) &&
            Number(q.get_child("capacity_entry")) && Number(q.get_child("capacity_quantity")),"capacity_restart_quote_invalid");
        why.clear();return true;
    } catch(const std::exception& e){out={};why=e.what();return false;}
}
std::string ProfessionHistoryQuery(const Task& task) {
    ProfessionJob job;std::string blocker;
    const bool gathering=IsGatheringRecoveryTask(task);
    if (task.mode!=Mode::Active || !task.accepted || !task.actor || !IsUuid(task.id) || task.root!=task.id ||
        !task.revision || (gathering ? !ValidateGuildProcurementTask(task,blocker) :
        ((task.source!="profession_job" && !IsGuildCraftTask(task) && !IsCommissionJob(task)) || !ValidateProfessionTask(task,blocker) ||
        !DecodeProfessionJob(task.checkpoint.data,job,blocker)))) throw std::invalid_argument("profession_history_task_invalid");
    // A single DB statement observes the revision, all unresolved work for this
    // actor (including dependent mail/purchases), and at most limit+1 attempts.
    // The extra row detects overflow; never silently truncate accepted work.
    const auto id=SqlValue(task.id);
    return "SELECT t.actor_guid,t.revision,EXISTS(SELECT 1 FROM living_activity_operation u "
        "JOIN living_activity_task owner ON owner.task_id=u.task_id WHERE owner.actor_guid=t.actor_guid "
        "AND u.state IN ('intent','reconciling')),o.operation_id,o.task_id,o.task_revision,o.kind,o.state,"
        "o.native_reference,o.before_state,o.after_state,o.evidence_code,SHA2(CONCAT(o.before_state,'|',o.after_state),256) FROM living_activity_task t "
        "LEFT JOIN (SELECT operation_id,task_id,task_revision,kind,state,native_reference,before_state,after_state,evidence_code "
        "FROM living_activity_operation WHERE task_id="+id+" AND "+
        (gathering ? "kind='gather_open' AND state IN ('intent','reconciling') " :
            std::string("(kind='profession_craft' OR (kind IN ('mail_collect','capacity_vendor_sale') AND state IN ('intent','reconciling'))")+
            (IsCommissionJob(task)?" OR (kind IN ('commission_mail_send','commission_trade','commission_output_partition') AND state<>'rejected') OR (kind='commission_trade_offer' AND state IN ('intent','reconciling')) OR kind IN ('commission_customer_received','commission_fee_collected','commission_parcel_returned')":"")+") ")+
        "ORDER BY task_revision,operation_id LIMIT "+std::to_string(gathering?2:ProfessionWorkflowAttemptLimit(task.checkpoint.data)+(IsCommissionJob(task)?18:3))+") o ON o.task_id=t.task_id "
        "WHERE t.task_id="+id+" AND t.actor_guid="+std::to_string(task.actor)+" AND t.revision="+
        std::to_string(task.revision)+" AND t.root_task_id=t.task_id AND t.mode='active' "
        "AND t.accepted=1 AND t.source="+SqlValue(task.source)+" AND t.kind="+SqlValue(Name(task.kind))+" ORDER BY o.task_revision,o.operation_id";
}
bool ProfessionHistoryCursor::Begin(const Task& owner,const std::vector<ProfessionHistoryRow>& rows,std::string& blocker) {
    task={};history={};records.clear();position=0;blocker.clear();
    try {
        // Reuse exactly the query's eligibility check even for test/restore data.
        (void)ProfessionHistoryQuery(owner);
        ProfessionJob job;std::string why;
        const bool gathering=IsGatheringRecoveryTask(owner);
        Require(gathering || DecodeProfessionJob(owner.checkpoint.data,job,why),"profession_history_task_invalid");
        Require(!rows.empty(),"profession_history_task_changed_or_missing");
        const auto limit=gathering?0u:ProfessionWorkflowAttemptLimit(owner.checkpoint.data);
        Require(rows.size()<=limit+(IsCommissionJob(owner)?17:2),"profession_history_attempt_limit_exceeded");
        bool first=true,unresolved=false;uint64_t previous=0;std::set<std::string> ids,commissionKinds;
        unsigned crafts=0,mails=0,capacity=0;
        for (const auto& fields : rows) {
            auto number=[&](size_t i){Tree scalar;scalar.data()=fields[i];return Number<uint64_t>(scalar);};
            Require(number(0)==owner.actor && number(1)==owner.revision,"profession_history_read_identity_changed");
            Require(fields[2]=="0" || fields[2]=="1","profession_history_unresolved_flag_invalid");
            if (first) {unresolved=fields[2]=="1";first=false;}
            else Require(unresolved==(fields[2]=="1"),"profession_history_read_inconsistent");
            if (fields[3].empty()) {
                Require(rows.size()==1 && std::all_of(fields.begin()+4,fields.end(),[](const auto& s){return s.empty();}),
                    "profession_history_empty_row_invalid");
                continue;
            }
            StoredCraftOperation row;auto& receipt=row.receipt;
            receipt.id=fields[3];receipt.task=fields[4];receipt.taskRevision=number(5);receipt.kind=fields[6];
            const bool commission=IsCommissionJob(owner) && (receipt.kind=="commission_mail_send" || receipt.kind=="commission_trade" || receipt.kind=="commission_trade_offer" || receipt.kind=="commission_output_partition" ||
                receipt.kind=="commission_customer_received" || receipt.kind=="commission_fee_collected" || receipt.kind=="commission_parcel_returned");
            Require(IsUuid(receipt.id) && ids.insert(receipt.id).second && receipt.task==owner.id &&
                receipt.taskRevision && (receipt.taskRevision>previous || (commission && receipt.taskRevision==previous)) && receipt.taskRevision<=owner.revision &&
                (gathering ? receipt.kind=="gather_open" : (commission || receipt.kind=="profession_craft" || receipt.kind=="mail_collect" || receipt.kind=="capacity_vendor_sale")),
                "profession_history_operation_identity_invalid");
            if(gathering) Require(++crafts==1 && owner.revision>1 && receipt.taskRevision==owner.revision-1 &&
                fields[7]=="reconciling","gather_history_operation_identity_invalid");
            else if(commission) Require((commissionKinds.insert(receipt.kind).second || receipt.kind=="commission_customer_received" || receipt.kind=="commission_output_partition") &&
                (receipt.kind=="commission_mail_send" || receipt.kind=="commission_trade" || receipt.kind=="commission_trade_offer" || receipt.kind=="commission_output_partition" || fields[7]=="verified"),"commission_history_duplicate_or_unverified_receipt");
            else if(receipt.kind=="profession_craft") Require(++crafts<=limit,"profession_history_attempt_limit_exceeded");
            else if(receipt.kind=="capacity_vendor_sale") Require(++capacity==1 && receipt.taskRevision==owner.revision &&
                (fields[7]=="intent" || fields[7]=="reconciling"),"capacity_history_operation_identity_invalid");
            else Require(++mails==1 && receipt.taskRevision==owner.revision &&
                (fields[7]=="intent" || fields[7]=="reconciling"),"profession_history_mail_identity_invalid");
            previous=receipt.taskRevision;
            if (fields[7]=="verified") receipt.state=OperationState::Verified;
            else if (fields[7]=="rejected") receipt.state=OperationState::Rejected;
            else if (fields[7]=="intent" || fields[7]=="reconciling") {
                Require(unresolved,"profession_history_unresolved_flag_missing");
                receipt.state=fields[7]=="intent" ? OperationState::Intent : OperationState::Reconciling;
            } else throw std::invalid_argument("profession_history_operation_state_invalid");
            Require(fields[8].size()<=160 && fields[9].size()<=8192 && fields[10].size()<=8192 && fields[11].size()<=64,
                "profession_history_evidence_bound");
            receipt.nativeReference=fields[8];row.beforeState=fields[9];row.afterState=fields[10];receipt.evidence=fields[11];
            row.journalDigest=fields[12];row.acknowledged=true;records.push_back(std::move(row));
        }
        task=owner;history.task=owner.id;history.revision=owner.revision;
        history.unresolvedOperation=unresolved;history.complete=records.empty();return true;
    } catch (const std::invalid_argument& error) {blocker=error.what();}
      catch (const std::exception&) {blocker="profession_history_envelope_malformed";}
    history={};records.clear();task={};return false;
}
bool ProfessionHistoryCursor::Advance(std::string& blocker) {
    blocker.clear();
    if (history.complete) return true;
    if (task.id.empty() || position>=records.size()) {blocker="profession_history_read_not_started";return false;}
    const auto& row=records[position];
    if(IsCommissionJob(task) && row.receipt.kind.compare(0,11,"commission_")==0) {
        if(row.receipt.kind=="commission_output_partition")history.commissionPartitions.push_back(row);
        else if(row.receipt.kind=="commission_trade_offer")history.interruptedCommissionOffer=row;
        else if(row.receipt.kind=="commission_trade")history.commissionTrade.push_back(row);
        else history.commissionMail.push_back(row); // Domain decoder validates the exact native contract before use.
    } else if (row.receipt.state==OperationState::Verified || row.receipt.state==OperationState::Rejected) {
        StoredCraftProof proof;
        if (!DecodeStoredCraftProof(task,row,proof,blocker)) {
            // No partial list may reach the next-step planner as a complete read.
            history={};records.clear();task={};return false;
        }
        history.attempts.push_back(std::move(proof.attempt));
    } else {
        history.unresolvedOperation=true;
        if(row.receipt.kind=="gather_open" && IsGatheringRecoveryTask(task)) history.interruptedGather=row;
        else if(row.receipt.state==OperationState::Intent && row.receipt.taskRevision==task.revision &&
            task.phase==Phase::Executing) {
            if(row.receipt.kind=="mail_collect") history.interruptedMail=row;
            else if(row.receipt.kind=="capacity_vendor_sale") history.interruptedCapacitySale=row;
            else history.interruptedCraft=row;
        } else if(row.receipt.kind=="profession_craft" && task.phase==Phase::Reconciling) {
            InterruptedCraftIntent intent;std::string reason;
            if(DecodeInterruptedCraftIntent(task,row,intent,reason)) history.interruptedCraft=row;
        }
    }
    if (++position==records.size()) {history.complete=true;records.clear();}
    return true;
}
}
