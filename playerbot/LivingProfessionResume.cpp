#include "LivingProfessionResume.h"
#include "LivingActivityJournal.h"
#include <limits>
#include <map>
#include <set>

namespace LivingActivity {
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
