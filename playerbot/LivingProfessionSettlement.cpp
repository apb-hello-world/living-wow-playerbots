#include "LivingProfessionSettlement.h"
#include "LivingActivityJournal.h"
#include "LivingPersonalResourceSettlement.h"
#include "LivingActivityItemGain.h"
#include "LivingGuildProcurement.h"
#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace LivingActivity {
namespace {
    std::string N(uint64_t value) {return std::to_string(value);}
    std::string ProofMatch(const Task& task,const ProfessionCraftProof& attempt) {
        const auto& r=attempt.receipt;
        auto match="o.operation_id="+SqlValue(r.id)+" AND o.task_id="+SqlValue(task.id)+
            " AND o.task_revision="+N(r.taskRevision)+" AND o.kind='profession_craft' AND o.state="+
            SqlValue(r.state==OperationState::Verified?"verified":"rejected")+
            " AND o.native_reference="+SqlValue(r.nativeReference)+" AND o.evidence_code="+SqlValue(r.evidence);
        if(!attempt.journalDigest.empty())match+=" AND SHA2(CONCAT(o.before_state,'|',o.after_state),256)="+SqlValue(attempt.journalDigest);
        return match;
    }
    bool PrepareToolHandoff(const Task& before,const ProfessionSnapshot& snapshot,const UnsettledClaimBatch& batch,
        const std::vector<NativeResourceBalance>& balances,uint64_t now,const std::string& receipt,
        ProfessionSettlement& result,std::string& blocker) {
        auto reject=[&](const char* why){blocker=why;return false;};
        ProfessionWorkflow flow;
        if(!DecodeProfessionWorkflow(before.checkpoint.data,flow,blocker) || flow.tools.empty() || flow.tools.back().finishedRevision)
            return reject("profession_tool_handoff_not_active");
        if(!batch.complete)return reject("profession_tool_handoff_claim_batch_incomplete");
        PersonalResourceSettlement protectedStock;
        if(!PreparePersonalResourceSettlement(before,batch,balances,protectedStock,blocker,"profession_tool_handoff_"))return false;
        const auto& job=flow.tools.back().job;
        const ProfessionCraftProof* proof=nullptr;const ResourceClaim* output=nullptr;
        for(const auto& attempt:snapshot.attempts) {
            if(attempt.recipe!=job.recipe || attempt.receipt.taskRevision<flow.tools.back().startedRevision ||
                attempt.receipt.state!=OperationState::Verified || !attempt.nativeEffectVerified || attempt.journalDigest.size()!=64)continue;
            for(const auto& c:batch.claims)
                if(c.itemEntry==job.outputEntry && c.quantity==1 && c.state=="held" && c.location=="bags" &&
                    !c.nativeReference && c.id==ItemGainClaimId(attempt.receipt.id,c.itemGuid)) {proof=&attempt;output=&c;break;}
            if(proof)break;
        }
        if(!proof || !output)return reject("profession_tool_handoff_native_output_required");
        const auto native=std::find_if(balances.begin(),balances.end(),[&](const auto& b){return b.itemGuid==output->itemGuid;});
        if(native==balances.end() || native->actor!=before.actor || native->location!="bags" ||
            native->itemEntry!=job.outputEntry || !native->quantity)return reject("profession_tool_handoff_output_not_carried");
        ProfessionSettlement prepared;prepared.task=before;auto& next=prepared.task;
        ++next.revision;next.updatedAtMs=now;next.phase=Phase::Verifying;next.retryAtMs=0;
        flow.tools.back().finishedRevision=next.revision;next.checkpoint.data=EncodeTaskProfessionWorkflow(before,flow);
        next.checkpoint.step="profession_tool_ready";next.checkpoint.blocker.clear();next.checkpoint.lastProgressAtMs=now;
        if(!PreserveProfessionIntent(before,next,blocker))return false;
        std::string guards,fingerprint=protectedStock.fingerprint;
        for(const auto& attempt:snapshot.attempts) {
            const auto match=ProofMatch(before,attempt);guards+=" AND EXISTS(SELECT 1 FROM living_activity_operation o WHERE "+match+')';
            fingerprint+='|'+match;
        }
        prepared.plan=Detail::TaskTransitionWrite(next,before.revision,receipt,"profession_tool_retained",fingerprint);
        prepared.plan.statements.front()+=" AND mode='active' AND accepted=1 AND phase='verifying' AND checkpoint="+SqlValue(before.checkpoint.data)+
            guards+protectedStock.guards+
            " AND (SELECT COUNT(*) FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id AND o.kind='profession_craft')="+N(snapshot.attempts.size())+
            " AND NOT EXISTS(SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))"+
            " AND EXISTS(SELECT 1 FROM character_inventory v JOIN item_instance i ON i.guid=v.item WHERE v.guid="+N(before.actor)+
            " AND v.item="+N(output->itemGuid)+" AND v.item_template="+N(output->itemEntry)+" AND i.owner_guid="+N(before.actor)+
            " AND i.itemEntry="+N(output->itemEntry)+" AND i.count="+N(native->quantity)+
            " AND ((v.bag=0 AND v.slot BETWEEN 23 AND 38) OR EXISTS(SELECT 1 FROM character_inventory b WHERE b.guid=v.guid AND b.item=v.bag AND b.bag=0 AND b.slot BETWEEN 19 AND 22)))";
        prepared.plan.statements.insert(prepared.plan.statements.begin(),
            "UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+N(before.actor));
        // Do not apply protectedStock.claims: the tool and all unused parent
        // materials remain held, with unchanged identities and revisions.
        result=std::move(prepared);blocker.clear();return true;
    }
    bool PrepareGuildCraftHandoff(const Task& before,const ProfessionSnapshot& snapshot,const UnsettledClaimBatch& batch,
        const std::vector<NativeResourceBalance>& balances,uint64_t now,const std::string& receipt,
        ProfessionSettlement& result,std::string& blocker) {
        auto reject=[&](const char* why){blocker=why;return false;};
        GuildProcurementJob guild;ProfessionJob job;
        if(!ValidateGuildProcurementTask(before,blocker) || !DecodeGuildProcurementJob(before.checkpoint.data,guild,blocker) ||
            guild.craft.empty() || guild.craftFinishedRevision || !batch.complete ||
            !DecodeProfessionIntent(guild.craft,job,blocker))return reject("guild_craft_handoff_context_invalid");
        PersonalResourceSettlement backing;
        if(!PreparePersonalResourceSettlement(before,batch,balances,backing,blocker,"guild_craft_handoff_"))return false;
        std::map<std::string,const ProfessionCraftProof*> proofs;
        std::string guards;
        for(const auto& attempt:snapshot.attempts) {
            if(attempt.journalDigest.size()!=64 || attempt.journalDigest.find_first_not_of("0123456789abcdef")!=std::string::npos)
                return reject("guild_craft_handoff_journal_digest_required");
            guards+=" AND EXISTS(SELECT 1 FROM living_activity_operation o WHERE "+ProofMatch(before,attempt)+')';
            if(attempt.recipe==job.recipe && attempt.nativeEffectVerified && attempt.receipt.state==OperationState::Verified)
                proofs.emplace(attempt.receipt.id,&attempt);
        }
        uint64_t carried=0;std::map<std::string,uint64_t> perOperation;
        PersonalResourceSettlement personal;UnsettledClaimBatch personalBatch;
        personalBatch.bookRevision=batch.bookRevision;personalBatch.complete=false;
        std::set<uint32_t> outputItems;
        for(size_t i=0;i<batch.claims.size();++i) {
            const auto& c=batch.claims[i];
            if(c.itemEntry!=guild.entry) {
                personalBatch.claims.push_back(c);personal.claims.push_back(backing.claims[i]);continue;
            }
            if(c.state!="held" || c.location!="bags" || c.nativeReference || c.copper)
                return reject("guild_craft_handoff_output_not_carried");
            const ProfessionCraftProof* proof=nullptr;
            for(const auto& row:proofs)if(c.id==ItemGainClaimId(row.first,c.itemGuid)){proof=row.second;break;}
            if(!proof)return reject("guild_craft_handoff_native_output_claim_required");
            uint64_t produced=0;for(const auto& output:proof->produced)if(output.entry==guild.entry)produced+=output.perAttempt;
            auto& credited=perOperation[proof->receipt.id];credited+=c.quantity;
            if(credited>produced)return reject("guild_craft_handoff_output_exceeds_receipt");
            carried+=c.quantity;outputItems.insert(c.itemGuid);
        }
        if(carried<guild.quantity)return reject("guild_craft_handoff_verified_goods_incomplete");
        for(const auto& native:balances)if(outputItems.count(native.itemGuid)) {
            guards+=" AND EXISTS(SELECT 1 FROM character_inventory v JOIN item_instance i ON i.guid=v.item WHERE v.guid="+N(before.actor)+
                " AND i.owner_guid=v.guid AND i.guid="+N(native.itemGuid)+" AND i.itemEntry="+N(guild.entry)+" AND i.count="+N(native.quantity)+
                " AND ((v.bag=0 AND v.slot BETWEEN 23 AND 38) OR EXISTS(SELECT 1 FROM character_inventory b WHERE b.guid=v.guid"
                " AND b.item=v.bag AND b.bag=0 AND b.slot BETWEEN 19 AND 22)))"
                " AND NOT EXISTS(SELECT 1 FROM mail_items WHERE item_guid="+N(native.itemGuid)+')'+
                " AND NOT EXISTS(SELECT 1 FROM guild_bank_item WHERE item_guid="+N(native.itemGuid)+')';
        }
        ProfessionSettlement prepared;prepared.task=before;auto& next=prepared.task;
        ++next.revision;next.updatedAtMs=now;next.phase=Phase::Preparing;next.retryAtMs=0;
        guild.craftFinishedRevision=next.revision;next.checkpoint.data=EncodeGuildProcurementJob(guild);
        next.checkpoint.step="guild_procurement_craft_ready";next.checkpoint.blocker.clear();next.checkpoint.lastProgressAtMs=now;
        if(!PreserveGuildProcurementIntent(before,next,blocker))return false;
        prepared.plan=Detail::TaskTransitionWrite(next,before.revision,receipt,"guild_procurement_craft_verified",backing.fingerprint+guards);
        prepared.plan.statements.front()+=" AND mode='active' AND accepted=1 AND phase='verifying' AND checkpoint="+SqlValue(before.checkpoint.data)+
            guards+backing.guards+
            " AND (SELECT COUNT(*) FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id AND o.kind='profession_craft')="+N(snapshot.attempts.size())+
            " AND NOT EXISTS(SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
            " WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))";
        prepared.plan.statements.insert(prepared.plan.statements.begin(),"UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+N(before.actor));
        // Retain every proven output claim, including native batch surplus.
        // Only unused personal materials/tools/money are released. The existing
        // procurement handoff trims surplus and creates the actual parcels.
        AppendPersonalResourceSettlement(prepared.plan,personal,personalBatch,now,receipt);
        prepared.claims=std::move(personal.claims);result=std::move(prepared);blocker.clear();return true;
    }
}
bool PrepareProfessionSettlement(const Task& before,const ProfessionSnapshot& snapshot,
    const UnsettledClaimBatch& batch,const std::vector<NativeResourceBalance>& balances,
    uint64_t nowMs,const std::string& receipt,ProfessionSettlement& result,std::string& blocker) {
    result={};auto reject=[&](const char* why){blocker=why;return false;};
    ProfessionJob job;std::string reason;
    if (!Validate(before,reason) || before.mode!=Mode::Active || before.phase!=Phase::Verifying ||
        (before.source!="profession_job" && !IsGuildCraftTask(before)) || before.root!=before.id || !before.parent.empty() ||
        !DecodeProfessionJob(before.checkpoint.data,job,reason) || !IsUuid(receipt) || nowMs<before.updatedAtMs ||
        before.revision>=std::numeric_limits<uint64_t>::max()-1)
        return reject("profession_settlement_context_invalid");
    if (NextProfessionStep(before,snapshot).step!=ProfessionStep::Finalize)
        return reject("profession_settlement_goal_not_verified");
    if(HasActiveProfessionTool(before.checkpoint.data))
        return PrepareToolHandoff(before,snapshot,batch,balances,nowMs,receipt,result,blocker);
    if(IsGuildCraftTask(before))return PrepareGuildCraftHandoff(before,snapshot,batch,balances,nowMs,receipt,result,blocker);
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
        const auto match=ProofMatch(before,attempt);
        fingerprint+='|'+match;proofGuards+=" AND EXISTS (SELECT 1 FROM living_activity_operation o WHERE "+match+')';
    }
    ProfessionJob creditJob;const ProfessionCraftProof* credit=nullptr;
    for(const auto& attempt:snapshot.attempts) {
        bool current=false;ProfessionJob candidate;
        if(attempt.receipt.state==OperationState::Verified && attempt.skillAfter>attempt.skillBefore &&
            attempt.skillAfter>=job.targetSkill && ProfessionJobAtRevision(before,attempt.receipt.taskRevision,candidate,current,reason) &&
            candidate.skill==job.skill) {credit=&attempt;creditJob=std::move(candidate);break;}
    }
    if(!credit)return reject("profession_skill_credit_not_verified");
    prepared.plan=Detail::TaskTransitionWrite(next,before.revision,receipt,
        batch.complete ? "profession_completed" : "profession_claims_settled",fingerprint);
    auto& update=prepared.plan.statements.front();
    const bool creditEnchant=creditJob.operation==ProfessionOperation::EnchantItem;
    const std::string path=creditEnchant?"$.native.":"$.native.result.";
    const std::string evidence=creditEnchant?"native_enchant_consumption_subject_and_skill_observed":"native_craft_consumption_output_and_skill_observed";
    update+=" AND mode='active' AND accepted=1 AND phase='verifying' AND checkpoint="+SqlValue(before.checkpoint.data)+
        " AND NOT EXISTS (SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id "
        "WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))"+
        proofGuards+" AND (SELECT COUNT(*) FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id "
        "AND o.kind='profession_craft')="+N(snapshot.attempts.size())+
        " AND EXISTS (SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id "
        "AND o.kind='profession_craft' AND o.state='verified' "
        "AND o.evidence_code="+SqlValue(evidence)+" AND o.operation_id="+SqlValue(credit->receipt.id)+
        " AND JSON_UNQUOTE(JSON_EXTRACT(o.after_state,"+SqlValue(path+"recipe")+"))="+SqlValue(N(creditJob.recipe))+
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
