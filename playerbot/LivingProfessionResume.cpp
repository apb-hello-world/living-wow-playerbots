#include "LivingProfessionResume.h"
#include "LivingActivityJournal.h"
#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace LivingActivity {
bool PrepareInterruptedProfession(const Task& saved,const WorldContext& current,
    const ProfessionHistory& history,const UnsettledClaimBatch& batch,const CraftFrame& frame,
    uint64_t nowMs,const std::string& receipt,ProfessionPreparation& result,std::string& blocker,
    const std::vector<NativeItemStack>& preservedBank) {
    result={};auto reject=[&](const char* why){blocker=why;return false;};
    // Only a restored record can enter this path. Zoning or a live pending cast
    // cannot erase its operation by presenting another map/session generation.
    if(!saved.context.boot.empty() || saved.context.actorGeneration || saved.context.mapGeneration ||
        current.actor!=saved.actor || !IsUuid(current.boot) || !current.actorGeneration ||
        !current.mapGeneration || !current.policyRevision || current.session.size()>120 ||
        (current.session.empty()!=(current.sessionRevision==0)) || !IsUuid(receipt) ||
        nowMs<saved.updatedAtMs || saved.revision>=UINT64_MAX-1)
        return reject("interrupted_craft_restored_context_required");
    if(!history.complete || history.task!=saved.id || history.revision!=saved.revision ||
        !history.unresolvedOperation || !history.interruptedCraft)
        return reject("interrupted_craft_history_incomplete");
    const auto& row=*history.interruptedCraft;InterruptedCraftIntent intent;ProfessionJob job;
    if(!DecodeInterruptedCraftIntent(saved,row,intent,blocker) ||
        !DecodeProfessionJob(saved.checkpoint.data,job,blocker)) return false;
    if(!ValidCraftFrame(frame) || frame.actor!=saved.actor || frame.skill!=intent.skill || frame.money!=intent.money)
        return reject("interrupted_craft_native_state_changed");
    if(!batch.complete || !batch.bookRevision || batch.claims.size()<intent.inputs.size() || batch.claims.size()>16 ||
        preservedBank.size()!=batch.claims.size()-intent.inputs.size())
        return reject("interrupted_craft_claim_batch_changed");
    auto n=[](uint64_t v){return std::to_string(v);};
    std::string guard,frames="{\"skill\":"+n(frame.skill)+",\"money\":"+n(frame.money)+",\"stacks\":[";
    std::set<std::string> ids;
    std::set<uint32_t> bankIds;
    std::map<uint32_t,uint64_t> inputQuantities;
    for(const auto& input:intent.inputs)inputQuantities[input.before.itemGuid]+=input.before.quantity;
    for(const auto& c:batch.claims) {
        const auto use=std::find_if(intent.inputs.begin(),intent.inputs.end(),[&](const auto& v){return v.before.id==c.id;});
        if(!ids.insert(c.id).second || !ValidResourceClaim(c) || c.task!=saved.id || c.actor!=saved.actor ||
            c.state!="held" || c.copper || c.nativeReference)
            return reject("interrupted_craft_claim_changed");
        if(use==intent.inputs.end()) {
            // Dependent capacity work shares this root; banking its surplus
            // does not release the claim before the whole job is settled.
            const auto item=std::find_if(preservedBank.begin(),preservedBank.end(),[&](const auto& v){return v.guid==c.itemGuid;});
            if(c.location!="bank" || item==preservedBank.end() || item->actor!=saved.actor ||
                item->entry!=c.itemEntry || item->count!=c.quantity || !bankIds.insert(item->guid).second ||
                (!item->bagGuid && (item->slot<39 || item->slot>=67)))
                return reject("interrupted_craft_preserved_bank_changed");
            guard+=" AND EXISTS(SELECT 1 FROM character_inventory v JOIN item_instance i ON i.guid=v.item WHERE v.guid="+
                n(saved.actor)+" AND v.item="+n(item->guid)+" AND v.item_template="+n(item->entry)+" AND v.bag="+n(item->bagGuid)+
                " AND v.slot="+n(item->slot)+" AND i.owner_guid="+n(saved.actor)+" AND i.itemEntry="+n(item->entry)+
                " AND i.count="+n(item->count)+')';
            if(item->bagGuid) guard+=" AND EXISTS(SELECT 1 FROM character_inventory b WHERE b.guid="+n(saved.actor)+
                " AND b.item="+n(item->bagGuid)+" AND b.bag=0 AND b.slot>=67 AND b.slot<74)";
        } else {
        if(!SameResourceClaim(c,use->before)) return reject("interrupted_craft_claim_changed");
        const auto item=std::find_if(frame.stacks.begin(),frame.stacks.end(),[&](const auto& v){return v.guid==c.itemGuid;});
        if(item==frame.stacks.end() || item->entry!=c.itemEntry || item->count!=inputQuantities[c.itemGuid])
            return reject("interrupted_craft_native_quantity_changed");
        // Conservative compatibility for intents without an entire before-frame:
        // require one exact, fully reserved native stack per recipe input.
        if(std::count_if(frame.stacks.begin(),frame.stacks.end(),[&](const auto& v){return v.entry==c.itemEntry;})!=1)
            return reject("interrupted_craft_mixed_stack_unsupported");
        }
        guard+=" AND EXISTS(SELECT 1 FROM living_activity_claim c WHERE c.claim_id="+SqlValue(c.id)+
            " AND c.task_id="+SqlValue(saved.id)+" AND c.actor_guid="+n(saved.actor)+" AND c.item_guid="+n(c.itemGuid)+
            " AND c.item_entry="+n(c.itemEntry)+" AND c.quantity="+n(c.quantity)+" AND c.copper=0 AND c.location="+SqlValue(c.location)+
            " AND c.native_reference=0 AND c.state='held' AND c.revision="+n(c.revision)+')';
    }
    for(const auto& item:frame.stacks) {
        if(frames.back()!='[') frames+=',';
        frames+='['+n(item.guid)+','+n(item.entry)+','+n(item.count)+','+n(item.bagGuid)+','+n(item.slot)+']';
        guard+=" AND EXISTS(SELECT 1 FROM character_inventory v JOIN item_instance i ON i.guid=v.item WHERE v.guid="+
            n(saved.actor)+" AND v.item="+n(item.guid)+" AND v.item_template="+n(item.entry)+" AND v.bag="+n(item.bagGuid)+
            " AND v.slot="+n(item.slot)+" AND i.owner_guid="+n(saved.actor)+" AND i.itemEntry="+n(item.entry)+
            " AND i.count="+n(item.count)+')';
    }
    frames+="]}";
    ProfessionPreparation prepared;prepared.task=saved;auto& next=prepared.task;
    next.context=current;++next.revision;next.phase=Phase::Verifying;next.updatedAtMs=nowMs;
    next.checkpoint.step="profession_prepare";next.checkpoint.blocker.clear();
    auto outcome=row.receipt;outcome.state=OperationState::Rejected;
    outcome.evidence="native_craft_intent_not_committed";
    outcome.nativeReference="spell:"+n(job.recipe)+":operation:"+outcome.id;
    const auto after=std::string("{\"recovery\":{\"version\":1,\"basis\":\"atomic_native_save_absent\",\"boot\":\"")+
        current.boot+"\"},\"frame\":"+frames+'}';
    prepared.plan=OperationOutcomeWrite(next,saved.revision,outcome,receipt,after);
    prepared.plan.statements.front()+=" AND phase='executing' AND accepted=1 AND checkpoint="+SqlValue(saved.checkpoint.data)+
        " AND EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.operation_id="+SqlValue(outcome.id)+
        " AND o.state='intent' AND o.before_state="+SqlValue(row.beforeState)+
        " AND o.after_state='{}' AND o.evidence_code='' AND o.native_reference='')"+
        " AND (SELECT COUNT(*) FROM living_activity_operation o JOIN living_activity_task owner ON owner.task_id=o.task_id"
        " WHERE owner.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))=1"+
        " AND (SELECT COUNT(*) FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
        " AND o.kind='profession_craft')="+n(history.attempts.size()+1)+
        " AND (SELECT COUNT(*) FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id"
        " AND c.state NOT IN ('consumed','released'))="+n(batch.claims.size())+
        " AND EXISTS(SELECT 1 FROM characters c WHERE c.guid="+n(saved.actor)+" AND c.money="+n(frame.money)+')'+
        " AND EXISTS(SELECT 1 FROM character_skills s WHERE s.guid="+n(saved.actor)+" AND s.skill="+n(job.skill)+
        " AND s.value="+n(frame.skill)+')'+guard;
    prepared.plan.statements.insert(prepared.plan.statements.begin(),
        "UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+n(saved.actor));
    result=std::move(prepared);blocker.clear();return true;
}
bool PrepareProfessionResumption(const Task& saved,const WorldContext& current,
    const ProfessionSnapshot& snapshot,const UnsettledClaimBatch& batch,
    const std::vector<NativeResourceBalance>& balances,uint64_t nowMs,
    const std::string& receipt,ProfessionPreparation& result,std::string& blocker) {
    result={};auto reject=[&](const char* why){blocker=why;return false;};
    std::string reason;
    if (!Validate(saved,reason) || !ValidateProfessionTask(saved,reason) || !IsProfessionJob(saved) ||
        !saved.accepted || saved.mode!=Mode::Active || saved.root!=saved.id || !saved.parent.empty() ||
        Terminal(saved.phase) || saved.phase==Phase::Executing || saved.phase==Phase::Reconciling ||
        !IsUuid(receipt) || nowMs<saved.updatedAtMs || saved.revision>=UINT64_MAX-1)
        return reject("profession_resume_task_not_reconcilable");
    if (current==saved.context || current.actor!=saved.actor || !current.actorGeneration ||
        !current.mapGeneration || !current.policyRevision || !IsUuid(current.boot) ||
        current.session.size()>120 || (current.session.empty()!=(current.sessionRevision==0)) ||
        (saved.context.boot.empty() && (saved.context.actorGeneration || saved.context.mapGeneration)))
        return reject("profession_resume_context_not_revalidated");
    auto rebound=saved;rebound.context=current;
    const auto decision=NextProfessionStep(rebound,snapshot);
    if (decision.step==ProfessionStep::Reconcile)
        return reject("profession_resume_history_requires_reconciliation");
    if (decision.step==ProfessionStep::Finalize)
        return reject("profession_resume_completed_goal_requires_settlement");
    if (!snapshot.safe) return reject("profession_safety_pause");
    // Rebinding does not erase backoff or extend deadlines/active progress.
    if (!batch.complete || !batch.bookRevision || batch.claims.size()>16 || balances.size()>16)
        return reject("profession_resume_claim_batch_incomplete");
    std::map<uint32_t,NativeResourceBalance> owned;
    for (const auto& b : balances)
        if (!ValidNativeResourceBalance(b) || b.actor!=saved.actor || !owned.emplace(b.itemGuid,b).second)
            return reject("profession_resume_native_balance_invalid");
    auto n=[](uint64_t v){return std::to_string(v);};
    std::map<uint32_t,uint64_t> totals;std::set<std::string> ids;
    std::string guards,fingerprint=saved.checkpoint.data+'|'+n(batch.bookRevision);
    for (const auto& c : batch.claims) {
        if (!ValidResourceClaim(c) || c.actor!=saved.actor || c.task!=saved.id || !ids.insert(c.id).second ||
            (c.state!="held" && c.state!="proposed"))
            return reject("profession_resume_claim_requires_reconciliation");
        if (c.state=="proposed") {
            if (c.itemGuid || c.nativeReference) return reject("profession_resume_proposal_has_native_identity");
        } else {
            const auto found=owned.find(c.itemGuid);
            if (found==owned.end() || found->second.itemEntry!=c.itemEntry || found->second.location!=c.location ||
                found->second.nativeReference!=c.nativeReference)
                return reject("profession_resume_native_stock_missing");
            const uint64_t amount=c.copper ? c.copper : c.quantity;
            const uint64_t available=c.copper ? found->second.copper : found->second.quantity;
            if (amount>available || totals[c.itemGuid]>available-amount)
                return reject("profession_resume_native_stock_changed");
            totals[c.itemGuid]+=amount;
        }
        const auto match="c.claim_id="+SqlValue(c.id)+" AND c.task_id="+SqlValue(c.task)+" AND c.actor_guid="+n(c.actor)+
            " AND c.item_guid="+n(c.itemGuid)+" AND c.item_entry="+n(c.itemEntry)+" AND c.quantity="+n(c.quantity)+
            " AND c.copper="+n(c.copper)+" AND c.location="+SqlValue(c.location)+" AND c.native_reference="+n(c.nativeReference)+
            " AND c.state="+SqlValue(c.state)+" AND c.revision="+n(c.revision);
        guards+=" AND EXISTS(SELECT 1 FROM living_activity_claim c WHERE "+match+')';fingerprint+='|'+match;
    }
    for (const auto& attempt : snapshot.attempts) {
        const auto& r=attempt.receipt;
        const auto match="o.operation_id="+SqlValue(r.id)+" AND o.task_id="+SqlValue(saved.id)+
            " AND o.task_revision="+n(r.taskRevision)+" AND o.kind='profession_craft' AND o.state="+
            SqlValue(r.state==OperationState::Verified ? "verified" : "rejected")+
            " AND o.native_reference="+SqlValue(r.nativeReference)+" AND o.evidence_code="+SqlValue(r.evidence);
        guards+=" AND EXISTS(SELECT 1 FROM living_activity_operation o WHERE "+match+')';fingerprint+='|'+match;
    }
    ProfessionPreparation prepared;prepared.task=rebound;auto& next=prepared.task;
    ++next.revision;next.updatedAtMs=nowMs;next.phase=Phase::Preparing;
    next.checkpoint.step="profession_prepare";next.checkpoint.blocker.clear();
    // Native safety, service availability and usefulness are checked again by
    // the ordinary next-step planner after the exact receipt is acknowledged.
    // In particular, revalidation itself is not progress or an execution grant.
    prepared.plan=Detail::TaskTransitionWrite(next,saved.revision,receipt,"profession_preparation_revalidated",fingerprint);
    prepared.plan.statements.front()+=" AND mode='active' AND accepted=1 AND phase="+SqlValue(Name(saved.phase))+
        " AND checkpoint="+SqlValue(saved.checkpoint.data)+guards+
        " AND NOT EXISTS(SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id "
        "WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))"
        " AND (SELECT COUNT(*) FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id "
        "AND o.kind='profession_craft')="+n(snapshot.attempts.size())+
        " AND (SELECT COUNT(*) FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id "
        "AND c.state NOT IN ('consumed','released'))="+n(batch.claims.size())+
        " AND NOT EXISTS(SELECT 1 FROM living_activity_task child WHERE child.root_task_id=living_activity_task.task_id "
        "AND child.task_id<>living_activity_task.task_id AND child.phase NOT IN ('completed','cancelled','failed'))";
    if (saved.phase==Phase::Verifying)
        prepared.plan.statements.front()+=" AND EXISTS(SELECT 1 FROM living_activity_operation o "
            "WHERE o.task_id=living_activity_task.task_id AND o.state IN ('verified','rejected'))";
    prepared.plan.statements.insert(prepared.plan.statements.begin(),
        "UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+n(saved.actor));
    result=std::move(prepared);blocker.clear();return true;
}
}
