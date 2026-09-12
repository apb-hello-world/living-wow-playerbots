#include "LivingProfessionEvidence.h"
#include "LivingActivityGameplay.h"
#include "LivingActivityOperations.h"
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
    void InputBacking(const ProfessionJob& job,const std::vector<ClaimConsumption>& uses,const CraftFrame& frame) {
        size_t matched=0;
        for (const auto& reagent : job.reagents) {
            unsigned count=0;const NativeItemStack* native=nullptr;
            for (const auto& stack : frame.stacks) if (stack.entry==reagent.entry) {
                ++count;native=&stack;
            }
            Require(count==1,"stored_craft_input_backing_mismatch");
            uint64_t used=0,held=0;
            for (const auto& claim:uses) if(claim.before.itemEntry==reagent.entry) {
                ++matched;used+=claim.used;held+=claim.before.quantity;
                Require(claim.before.itemGuid==native->guid,"stored_craft_input_backing_mismatch");
            }
            Require(used==reagent.perAttempt && held<=native->count,"stored_craft_recipe_claims_mismatch");
        }
        Require(matched==uses.size(),"stored_craft_recipe_claims_mismatch");
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
}
bool DecodeStoredCraftProof(const Task& task,const StoredCraftOperation& row,
    StoredCraftProof& result,std::string& blocker) {
    result={};blocker.clear();
    try {
        const auto& receipt=row.receipt;ProfessionJob job;std::string reason;
        Require(row.acknowledged,"stored_craft_receipt_not_acknowledged");
        Require(task.mode==Mode::Active && task.accepted && IsUuid(task.id) && task.root==task.id && task.actor &&
            ValidateProfessionTask(task,reason) && IsProfessionJob(task) &&
            DecodeProfessionJob(task.checkpoint.data,job,reason),"stored_craft_task_invalid");
        Require(IsUuid(receipt.id) && receipt.task==task.id && receipt.taskRevision && receipt.taskRevision<task.revision &&
            receipt.kind=="profession_craft","stored_craft_receipt_identity_mismatch");
        Require(receipt.state==OperationState::Verified || receipt.state==OperationState::Rejected,
            "stored_craft_operation_unresolved");
        const bool verified=receipt.state==OperationState::Verified;
        if (!verified && receipt.evidence=="native_craft_intent_not_committed") {
            auto predecessor=task;predecessor.revision=receipt.taskRevision;
            predecessor.phase=Phase::Executing;predecessor.checkpoint.step="profession_craft";
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
            InputBacking(job,decoded.inputs,frame);
            for (const auto& use:decoded.inputs) for(const auto& stack:frame.stacks)
                if(stack.guid==use.before.itemGuid) Require(stack.count==use.before.quantity,"stored_craft_recovery_quantity_changed");
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
        const auto& intended=before.get_child("native.native");Object(intended,{"recipe","skill","money"});
        auto inputs=Inputs(task,before.get_child("native.claimed_consumption"));
        const auto& captured=verified ? after.get_child("native.result") : after;
        Object(captured,{"recipe","skill_id","effect_entered","native_finished","native_succeeded",
            "created_calls","created_quantity","before","after"});
        Require(Number(captured.get_child("recipe"))==job.recipe && Number(captured.get_child("skill_id"))==job.skill &&
            Number(intended.get_child("recipe"))==job.recipe,"stored_craft_recipe_identity_mismatch");
        const auto frameBefore=Frame(task.actor,captured.get_child("before"));
        const auto frameAfter=Frame(task.actor,captured.get_child("after"));
        Require(Number(intended.get_child("skill"))==frameBefore.skill && Number(intended.get_child("money"))==frameBefore.money,
            "stored_craft_intent_snapshot_mismatch");
        InputBacking(job,inputs,frameBefore);
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
            task.parent.empty() && task.phase==Phase::Executing && task.checkpoint.step=="profession_craft" &&
            ValidateProfessionTask(task,reason) && IsProfessionJob(task) &&
            DecodeProfessionJob(task.checkpoint.data,job,reason),"interrupted_craft_task_invalid");
        Require(IsUuid(r.id) && r.task==task.id && r.taskRevision==task.revision && r.kind=="profession_craft" &&
            r.state==OperationState::Intent && r.evidence.empty() && r.nativeReference.empty() && row.afterState=="{}",
            "interrupted_craft_not_pristine_intent");
        Require((job.operation==ProfessionOperation::CreateItem || job.operation==ProfessionOperation::TransformMaterial) &&
            !job.subjectItem,"interrupted_craft_operation_unsupported");
        const auto before=Parse(row.beforeState);Object(before,{"effects","persistence","native","item_gain"});
        Require(Number(before.get_child("effects"))==SpellEffectMask(false) &&
            Number(before.get_child("persistence"))==unsigned(NativePersistence::Profession),"interrupted_craft_atomic_contract_required");
        Object(before.get_child("native"),{"native","claimed_consumption"});
        const auto& native=before.get_child("native.native");Object(native,{"recipe","skill","money"});
        InterruptedCraftIntent decoded;decoded.output=Gain(before.get_child("item_gain"));
        decoded.skill=Number<uint16_t>(native.get_child("skill"));decoded.money=Number(native.get_child("money"));
        decoded.inputs=Inputs(task,before.get_child("native.claimed_consumption"));
        Require(Number(native.get_child("recipe"))==job.recipe && decoded.output.entry==job.outputEntry,
            "interrupted_craft_recipe_mismatch");
        size_t matched=0;
        for(const auto& need:job.reagents) {
            uint64_t used=0;uint32_t guid=0;
            for(const auto& use:decoded.inputs) if(use.before.itemEntry==need.entry) {
                Require(!guid || guid==use.before.itemGuid,"interrupted_craft_mixed_stack_unsupported");
                guid=use.before.itemGuid;used+=use.used;++matched;
            }
            Require(used==need.perAttempt,"interrupted_craft_recipe_mismatch");
        }
        Require(matched==decoded.inputs.size(),"interrupted_craft_recipe_mismatch");
        result=std::move(decoded);return true;
    } catch(const std::invalid_argument& error) {blocker=error.what();}
      catch(const std::exception&) {blocker="interrupted_craft_evidence_malformed";}
    return false;
}

std::string ProfessionHistoryQuery(const Task& task) {
    ProfessionJob job;std::string blocker;
    if (task.mode!=Mode::Active || !task.accepted || !task.actor || !IsUuid(task.id) || task.root!=task.id ||
        !task.revision || task.source!="profession_job" || !ValidateProfessionTask(task,blocker) ||
        !DecodeProfessionJob(task.checkpoint.data,job,blocker)) throw std::invalid_argument("profession_history_task_invalid");
    // A single DB statement observes the revision, all unresolved work for this
    // actor (including dependent mail/purchases), and at most limit+1 attempts.
    // The extra row detects overflow; never silently truncate accepted work.
    const auto id=SqlValue(task.id);
    return "SELECT t.actor_guid,t.revision,EXISTS(SELECT 1 FROM living_activity_operation u "
        "JOIN living_activity_task owner ON owner.task_id=u.task_id WHERE owner.actor_guid=t.actor_guid "
        "AND u.state IN ('intent','reconciling')),o.operation_id,o.task_id,o.task_revision,o.kind,o.state,"
        "o.native_reference,o.before_state,o.after_state,o.evidence_code FROM living_activity_task t "
        "LEFT JOIN (SELECT operation_id,task_id,task_revision,kind,state,native_reference,before_state,after_state,evidence_code "
        "FROM living_activity_operation WHERE task_id="+id+" AND (kind='profession_craft' OR "
        "(kind='mail_collect' AND state IN ('intent','reconciling'))) "
        "ORDER BY task_revision,operation_id LIMIT "+std::to_string(job.attemptLimit+2)+") o ON o.task_id=t.task_id "
        "WHERE t.task_id="+id+" AND t.actor_guid="+std::to_string(task.actor)+" AND t.revision="+
        std::to_string(task.revision)+" AND t.root_task_id=t.task_id AND t.mode='active' "
        "AND t.accepted=1 AND t.source='profession_job' AND t.kind='profession' ORDER BY o.task_revision,o.operation_id";
}
bool ProfessionHistoryCursor::Begin(const Task& owner,const std::vector<ProfessionHistoryRow>& rows,std::string& blocker) {
    task={};history={};records.clear();position=0;blocker.clear();
    try {
        // Reuse exactly the query's eligibility check even for test/restore data.
        (void)ProfessionHistoryQuery(owner);
        ProfessionJob job;std::string why;
        Require(DecodeProfessionJob(owner.checkpoint.data,job,why),"profession_history_task_invalid");
        Require(!rows.empty(),"profession_history_task_changed_or_missing");
        Require(rows.size()<=job.attemptLimit+1,"profession_history_attempt_limit_exceeded");
        bool first=true,unresolved=false;uint64_t previous=0;std::set<std::string> ids;
        unsigned crafts=0,mails=0;
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
            Require(IsUuid(receipt.id) && ids.insert(receipt.id).second && receipt.task==owner.id &&
                receipt.taskRevision>previous && receipt.taskRevision<=owner.revision &&
                (receipt.kind=="profession_craft" || receipt.kind=="mail_collect"),
                "profession_history_operation_identity_invalid");
            if(receipt.kind=="profession_craft") Require(++crafts<=job.attemptLimit,"profession_history_attempt_limit_exceeded");
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
            row.acknowledged=true;records.push_back(std::move(row));
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
    if (row.receipt.state==OperationState::Verified || row.receipt.state==OperationState::Rejected) {
        StoredCraftProof proof;
        if (!DecodeStoredCraftProof(task,row,proof,blocker)) {
            // No partial list may reach the next-step planner as a complete read.
            history={};records.clear();task={};return false;
        }
        history.attempts.push_back(std::move(proof.attempt));
    } else {
        history.unresolvedOperation=true;
        if(row.receipt.state==OperationState::Intent && row.receipt.taskRevision==task.revision &&
            task.phase==Phase::Executing) {
            if(row.receipt.kind=="mail_collect") history.interruptedMail=row;
            else history.interruptedCraft=row;
        }
    }
    if (++position==records.size()) {history.complete=true;records.clear();}
    return true;
}
}
