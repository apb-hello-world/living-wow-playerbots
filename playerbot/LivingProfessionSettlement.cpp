#include "LivingProfessionSettlement.h"
#include "LivingActivityJournal.h"
#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace LivingActivity {
namespace {
    std::string N(uint64_t value) {return std::to_string(value);}
    std::string ClaimWhere(const ResourceClaim& c) {
        return "c.claim_id="+SqlValue(c.id)+" AND c.task_id="+SqlValue(c.task)+" AND c.actor_guid="+N(c.actor)+
            " AND c.item_guid="+N(c.itemGuid)+" AND c.item_entry="+N(c.itemEntry)+" AND c.quantity="+N(c.quantity)+
            " AND c.copper="+N(c.copper)+" AND c.location="+SqlValue(c.location)+" AND c.native_reference="+
            N(c.nativeReference)+" AND c.state="+SqlValue(c.state)+" AND c.revision="+N(c.revision);
    }
}
bool PrepareProfessionSettlement(const Task& before,const ProfessionSnapshot& snapshot,
    const UnsettledClaimBatch& batch,const std::vector<NativeResourceBalance>& balances,
    uint64_t nowMs,const std::string& receipt,ProfessionSettlement& result,std::string& blocker) {
    result={};auto reject=[&](const char* why){blocker=why;return false;};
    ProfessionJob job;std::string reason;
    if (!Validate(before,reason) || before.mode!=Mode::Active || before.phase!=Phase::Verifying ||
        before.source!="profession_job" || before.root!=before.id || !before.parent.empty() ||
        !DecodeProfessionJob(before.checkpoint.data,job,reason) || !IsUuid(receipt) || nowMs<before.updatedAtMs ||
        before.revision>=std::numeric_limits<uint64_t>::max()-1)
        return reject("profession_settlement_context_invalid");
    if (NextProfessionStep(before,snapshot).step!=ProfessionStep::Finalize)
        return reject("profession_settlement_goal_not_verified");
    // Do not quietly release an item promised to a requester or another job.
    if (job.purpose!=ProfessionPurpose::SkillGain ||
        (job.operation!=ProfessionOperation::CreateItem && job.operation!=ProfessionOperation::TransformMaterial))
        return reject("profession_output_handoff_validator_required");
    if (!batch.bookRevision || batch.claims.size()>16 || balances.size()>16 || (!batch.complete && batch.claims.empty()))
        return reject("profession_settlement_claim_batch_invalid");
    std::map<uint32_t,NativeResourceBalance> owned;
    for (const auto& native : balances) {
        if (native.actor!=before.actor || !owned.emplace(native.itemGuid,native).second ||
            (native.location=="money" ? (native.itemGuid || native.itemEntry || native.quantity) :
             (!native.itemGuid || !native.itemEntry || native.copper ||
              (native.location!="bags" && native.location!="bank"))))
            return reject("profession_settlement_native_balance_invalid");
    }
    std::set<std::string> ids;std::map<uint32_t,uint64_t> claimed;
    std::string excluded,fingerprint=before.checkpoint.data+'|'+N(batch.bookRevision)+'|'+(batch.complete ? "complete" : "batch");
    ProfessionSettlement prepared;
    for (const auto& c : batch.claims) {
        if (!ValidResourceClaim(c) || c.task!=before.id || c.actor!=before.actor || !ids.insert(c.id).second ||
            c.revision>=std::numeric_limits<uint64_t>::max()-1 ||
            (c.state!="held" && c.state!="proposed") || c.nativeReference)
            return reject("profession_settlement_claim_requires_reconciliation");
        if (c.state=="proposed") {
            if (c.itemGuid) return reject("profession_settlement_proposal_has_native_identity");
        } else {
            const auto found=owned.find(c.itemGuid);
            if (found==owned.end() || found->second.location!=c.location || found->second.itemEntry!=c.itemEntry)
                return reject("profession_settlement_native_stock_missing");
            claimed[c.itemGuid]+=c.quantity+c.copper;
            if (claimed[c.itemGuid]>(c.copper ? found->second.copper : found->second.quantity))
                return reject("profession_settlement_native_stock_changed");
        }
        if (!excluded.empty()) excluded+=',';
        excluded+=SqlValue(c.id);fingerprint+='|'+ClaimWhere(c);
        auto released=c;released.state="released";++released.revision;
        prepared.claims.push_back({released,c.revision});
    }
    prepared.task=before;auto& next=prepared.task;++next.revision;next.updatedAtMs=nowMs;
    next.phase=batch.complete ? Phase::Completed : Phase::Verifying;
    next.checkpoint.step=batch.complete ? "profession_completed" : "profession_settling";
    next.checkpoint.blocker=batch.complete ? "" : "profession_claim_settlement_pending";
    next.checkpoint.lastProgressAtMs=nowMs;next.retryAtMs=0;
    // Proof values are internal, but the SQL must independently require the
    // actual saved native skill increase, not merely a supplied 'verified' bit.
    // All saved attempts are immutable once verified/rejected. Bind their IDs,
    // revisions, outcomes and exact native references; reject a shortened list.
    std::string proofGuards;
    for (const auto& attempt : snapshot.attempts) {
        const auto& r=attempt.receipt;
        const auto state=r.state==OperationState::Verified ? "verified" : "rejected";
        const std::string match="o.operation_id="+SqlValue(r.id)+" AND o.task_id="+SqlValue(before.id)+
            " AND o.task_revision="+N(r.taskRevision)+" AND o.kind='profession_craft' AND o.state="+SqlValue(state)+
            " AND o.native_reference="+SqlValue(r.nativeReference)+" AND o.evidence_code="+SqlValue(r.evidence);
        fingerprint+='|'+match;proofGuards+=" AND EXISTS (SELECT 1 FROM living_activity_operation o WHERE "+match+')';
    }
    prepared.plan=Detail::TaskTransitionWrite(next,before.revision,receipt,
        batch.complete ? "profession_completed" : "profession_claims_settled",fingerprint);
    auto& update=prepared.plan.statements.front();
    update+=" AND mode='active' AND accepted=1 AND phase='verifying' AND checkpoint="+SqlValue(before.checkpoint.data)+
        " AND NOT EXISTS (SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id "
        "WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))"+
        proofGuards+" AND (SELECT COUNT(*) FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id "
        "AND o.kind='profession_craft')="+N(snapshot.attempts.size())+
        " AND EXISTS (SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id "
        "AND o.kind='profession_craft' AND o.state='verified' "
        "AND o.evidence_code='native_craft_consumption_output_and_skill_observed' "
        "AND JSON_UNQUOTE(JSON_EXTRACT(o.after_state,'$.native.result.recipe'))="+SqlValue(N(job.recipe))+
        " AND JSON_UNQUOTE(JSON_EXTRACT(o.after_state,'$.native.result.skill_id'))="+SqlValue(N(job.skill))+
        " AND CAST(JSON_UNQUOTE(JSON_EXTRACT(o.after_state,'$.native.result.after.skill')) AS UNSIGNED)>="+N(job.targetSkill)+
        " AND CAST(JSON_UNQUOTE(JSON_EXTRACT(o.after_state,'$.native.result.after.skill')) AS UNSIGNED)>"
        "CAST(JSON_UNQUOTE(JSON_EXTRACT(o.after_state,'$.native.result.before.skill')) AS UNSIGNED))"+
        " AND NOT EXISTS (SELECT 1 FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id "
        "AND c.state IN ('in_transfer','reconciling'))"+
        " AND NOT EXISTS (SELECT 1 FROM living_activity_task child WHERE child.root_task_id=living_activity_task.task_id "
        "AND child.task_id<>living_activity_task.task_id AND child.phase NOT IN ('completed','cancelled','failed'))";
    for (const auto& c : batch.claims)
        update+=" AND EXISTS (SELECT 1 FROM living_activity_claim c WHERE "+ClaimWhere(c)+')';
    if (batch.complete)
        update+=" AND NOT EXISTS (SELECT 1 FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id "
            "AND c.state NOT IN ('consumed','released')"+(excluded.empty() ? "" : " AND c.claim_id NOT IN ("+excluded+')')+')';
    // Serialize with other accepted work for this actor, using existing rows.
    prepared.plan.statements.insert(prepared.plan.statements.begin(),
        "UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+N(before.actor));
    const auto accepted=prepared.plan.receiptQuery;
    for (size_t i=0;i<batch.claims.size();++i) {
        const auto& c=batch.claims[i];const auto& after=prepared.claims[i].after;
        prepared.plan.statements.push_back("UPDATE living_activity_claim c JOIN living_activity_task t ON t.task_id=c.task_id "
            "SET c.state='released',c.revision="+N(after.revision)+",c.updated_at_ms="+N(nowMs)+" WHERE "+ClaimWhere(c)+
            " AND t.last_receipt_id="+SqlValue(receipt)+" AND EXISTS ("+accepted+')');
        prepared.plan.receiptQuery+=" AND EXISTS (SELECT 1 FROM living_activity_claim c WHERE "+ClaimWhere(after)+')';
    }
    result=std::move(prepared);blocker.clear();return true;
}
bool PrepareProfessionRestartSettlement(const Task& saved,const WorldContext& current,
    const ProfessionSnapshot& snapshot,const UnsettledClaimBatch& batch,
    const std::vector<NativeResourceBalance>& balances,uint64_t nowMs,const std::string& receipt,
    ProfessionSettlement& result,std::string& blocker) {
    result={};
    if (saved.context.actor!=saved.actor || current.actor!=saved.actor ||
        saved.context.boot==current.boot || !IsUuid(current.boot) ||
        (saved.context.boot.empty() && (saved.context.actorGeneration || saved.context.mapGeneration)) ||
        !current.actorGeneration || !current.mapGeneration || !current.policyRevision ||
        (current.session.empty() ? current.sessionRevision!=0 : current.sessionRevision==0)) {
        blocker="profession_restart_context_not_revalidated";return false;
    }
    // The loader deliberately restores no boot or native epochs. Their absence
    // is expected and never itself grants authority. Only the native context
    // changes; accepted intent, task ID, revision and
    // exact saved operation history stay unchanged. Settlement still performs
    // compare-and-swap against the original persisted revision and checkpoint.
    auto rebound=saved;rebound.context=current;
    return PrepareProfessionSettlement(rebound,snapshot,batch,balances,nowMs,receipt,result,blocker);
}
}
