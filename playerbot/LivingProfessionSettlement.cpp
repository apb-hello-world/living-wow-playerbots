#include "LivingProfessionSettlement.h"
#include "LivingActivityJournal.h"
#include "LivingPersonalResourceSettlement.h"
#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace LivingActivity {
namespace {
    std::string N(uint64_t value) {return std::to_string(value);}
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
    const bool enchant=job.operation==ProfessionOperation::EnchantItem;
    if (job.purpose!=ProfessionPurpose::SkillGain ||
        (!enchant && job.operation!=ProfessionOperation::CreateItem && job.operation!=ProfessionOperation::TransformMaterial))
        return reject("profession_output_handoff_validator_required");
    PersonalResourceSettlement resources;
    if (!PreparePersonalResourceSettlement(before,batch,balances,resources,blocker,"profession_settlement_")) return false;
    auto fingerprint=resources.fingerprint;
    ProfessionSettlement prepared;
    prepared.claims=resources.claims;
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
    const std::string path=enchant?"$.native.":"$.native.result.";
    const std::string evidence=enchant?"native_enchant_consumption_subject_and_skill_observed":"native_craft_consumption_output_and_skill_observed";
    update+=" AND mode='active' AND accepted=1 AND phase='verifying' AND checkpoint="+SqlValue(before.checkpoint.data)+
        " AND NOT EXISTS (SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id "
        "WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))"+
        proofGuards+" AND (SELECT COUNT(*) FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id "
        "AND o.kind='profession_craft')="+N(snapshot.attempts.size())+
        " AND EXISTS (SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id "
        "AND o.kind='profession_craft' AND o.state='verified' "
        "AND o.evidence_code="+SqlValue(evidence)+
        " AND JSON_UNQUOTE(JSON_EXTRACT(o.after_state,"+SqlValue(path+"recipe")+"))="+SqlValue(N(job.recipe))+
        " AND JSON_UNQUOTE(JSON_EXTRACT(o.after_state,"+SqlValue(path+"skill_id")+"))="+SqlValue(N(job.skill))+
        " AND CAST(JSON_UNQUOTE(JSON_EXTRACT(o.after_state,"+SqlValue(path+"after.skill")+")) AS UNSIGNED)>="+N(job.targetSkill)+
        " AND CAST(JSON_UNQUOTE(JSON_EXTRACT(o.after_state,"+SqlValue(path+"after.skill")+")) AS UNSIGNED)>"
        "CAST(JSON_UNQUOTE(JSON_EXTRACT(o.after_state,"+SqlValue(path+"before.skill")+")) AS UNSIGNED))"+
        " AND NOT EXISTS (SELECT 1 FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id "
        "AND c.state IN ('in_transfer','reconciling'))"+
        " AND NOT EXISTS (SELECT 1 FROM living_activity_task child WHERE child.root_task_id=living_activity_task.task_id "
        "AND child.task_id<>living_activity_task.task_id AND child.phase NOT IN ('completed','cancelled','failed'))";
    update+=resources.guards;
    // Serialize with other accepted work for this actor, using existing rows.
    prepared.plan.statements.insert(prepared.plan.statements.begin(),
        "UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+N(before.actor));
    AppendPersonalResourceSettlement(prepared.plan,resources,batch,nowMs,receipt);
    result=std::move(prepared);blocker.clear();return true;
}
bool PrepareProfessionRestartSettlement(const Task& saved,const WorldContext& current,
    const ProfessionSnapshot& snapshot,const UnsettledClaimBatch& batch,
    const std::vector<NativeResourceBalance>& balances,uint64_t nowMs,const std::string& receipt,
    ProfessionSettlement& result,std::string& blocker) {
    result={};
    if (saved.context.actor!=saved.actor || current.actor!=saved.actor ||
        saved.context==current || !IsUuid(current.boot) || current.session.size()>120 ||
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
