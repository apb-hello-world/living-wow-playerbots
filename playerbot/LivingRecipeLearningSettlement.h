#pragma once
#include "LivingRecipeLearning.h"
#include "LivingActivityJournal.h"
#include "LivingActivityResources.h"

namespace LivingActivity {
    struct RecipeLearningSettlement {Task task;WritePlan plan;};
    inline bool PrepareRecipeLearningSettlement(const Task& saved,const WorldContext& current,
        const UnsettledClaimBatch& claims,uint64_t now,const std::string& receipt,
        RecipeLearningSettlement& result,std::string& blocker) {
        result={};RecipeLearningJob job;
        auto reject=[&](const char* why){blocker=why;return false;};
        if (!Validate(saved,blocker) || !ValidateRecipeLearningTask(saved,blocker) || !IsRecipeLearningTask(saved) ||
            !DecodeRecipeLearningJob(saved.checkpoint.data,job,blocker) || saved.mode!=Mode::Active || !saved.accepted ||
            (saved.phase!=Phase::Verifying && saved.phase!=Phase::Reconciling) || !IsUuid(receipt) ||
            saved.revision>=std::numeric_limits<uint64_t>::max()-1 || now<saved.updatedAtMs ||
            current.actor!=saved.actor || !current.actorGeneration || !current.mapGeneration || !IsUuid(current.boot))
            return reject("recipe_settlement_saved_context_required");
        if (!claims.bookRevision || !claims.complete || !claims.claims.empty())
            return reject("recipe_settlement_unfinished_claims");
        result.task=saved;auto& next=result.task;
        ++next.revision;next.updatedAtMs=now;next.context=current;next.phase=Phase::Completed;
        next.checkpoint.step="recipe_completed";next.checkpoint.blocker.clear();next.retryAtMs=0;next.checkpoint.lastProgressAtMs=now;
        result.plan=Detail::TaskTransitionWrite(next,saved.revision,receipt,"recipe_learning_completed",saved.checkpoint.data);
        // Applied in the same guarded transaction as the terminal transition.
        // A known spell alone, elapsed time, or a model's claim cannot pass.
        result.plan.statements.front()+=" AND mode='active' AND accepted=1 AND phase IN ('verifying','reconciling')"
            " AND checkpoint="+SqlValue(saved.checkpoint.data)+
            " AND EXISTS (SELECT 1 FROM character_spell WHERE guid=living_activity_task.actor_guid AND spell="+
            std::to_string(job.recipe)+" AND disabled=0)"
            " AND NOT EXISTS (SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
            " WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))"
            " AND NOT EXISTS (SELECT 1 FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id"
            " AND c.state NOT IN ('consumed','released'))"
            " AND NOT EXISTS (SELECT 1 FROM living_activity_task child WHERE child.parent_task_id=living_activity_task.task_id"
            " AND child.phase NOT IN ('completed','cancelled','failed'))"
            " AND (SELECT COUNT(*) FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
            " AND o.kind='recipe_learning' AND o.state='verified')=1"
            " AND EXISTS (SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
            " AND o.kind='recipe_learning' AND o.state='verified' AND o.evidence_code='native_recipe_book_consumed_and_spell_learned'"
            " AND CAST(JSON_UNQUOTE(JSON_EXTRACT(o.after_state,'$.native.recipe')) AS UNSIGNED)="+std::to_string(job.recipe)+
            " AND CAST(JSON_UNQUOTE(JSON_EXTRACT(o.after_state,'$.native.skill_id')) AS UNSIGNED)="+std::to_string(job.skill)+
            " AND CAST(JSON_UNQUOTE(JSON_EXTRACT(o.after_state,'$.native.before.entry')) AS UNSIGNED)="+std::to_string(job.book)+')';
        result.plan.statements.insert(result.plan.statements.begin(),
            "UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+std::to_string(saved.actor));
        blocker.clear();return true;
    }
    inline bool PrepareRecipeLearningResumption(const Task& saved,const WorldContext& current,
        const UnsettledClaimBatch& claims,const std::vector<NativeResourceBalance>& stock,uint64_t now,
        const std::string& receipt,RecipeLearningSettlement& result,std::string& blocker) {
        result={};RecipeLearningJob job;
        auto reject=[&](const char* why){blocker=why;return false;};
        if (!Validate(saved,blocker) || !IsRecipeLearningTask(saved) || !ValidateRecipeLearningTask(saved,blocker) ||
            !DecodeRecipeLearningJob(saved.checkpoint.data,job,blocker) || saved.mode!=Mode::Active || !saved.accepted ||
            Terminal(saved.phase) || saved.context==current || current.actor!=saved.actor || !current.actorGeneration ||
            !current.mapGeneration || !IsUuid(current.boot) || !IsUuid(receipt) || now<saved.updatedAtMs ||
            saved.revision>=std::numeric_limits<uint64_t>::max()-1 || !claims.bookRevision || !claims.complete ||
            claims.claims.size()>1 || stock.size()!=claims.claims.size()) return reject("recipe_resume_context_or_claims_invalid");
        std::string backing;
        for (size_t i=0;i<claims.claims.size();++i) {
            const auto& claim=claims.claims[i];const auto& native=stock[i];
            if (!ValidResourceClaim(claim) || claim.task!=saved.id || claim.actor!=saved.actor || claim.state!="held" ||
                claim.location!="bags" || claim.quantity!=1 || claim.copper || claim.nativeReference || claim.itemEntry!=job.book ||
                native.actor!=saved.actor || native.itemGuid!=claim.itemGuid || native.itemEntry!=job.book ||
                native.location!="bags" || native.copper || native.nativeReference || !native.quantity)
                return reject("recipe_resume_original_book_not_backed");
            backing+=" AND EXISTS (SELECT 1 FROM living_activity_claim c JOIN character_inventory v ON v.item=c.item_guid"
                " JOIN item_instance i ON i.guid=v.item WHERE c.claim_id="+SqlValue(claim.id)+" AND c.task_id="+SqlValue(saved.id)+
                " AND c.actor_guid="+std::to_string(saved.actor)+" AND c.revision="+std::to_string(claim.revision)+
                " AND c.item_guid="+std::to_string(claim.itemGuid)+" AND c.item_entry="+std::to_string(job.book)+
                " AND c.state='held' AND c.location='bags' AND c.quantity=1 AND v.guid=c.actor_guid"
                " AND i.owner_guid=c.actor_guid AND i.itemEntry=c.item_entry AND i.count="+std::to_string(native.quantity)+')';
        }
        result.task=saved;auto& next=result.task;++next.revision;next.context=current;next.phase=Phase::Preparing;
        next.updatedAtMs=now;next.retryAtMs=0;next.checkpoint.step="recipe_prepare";next.checkpoint.blocker.clear();
        result.plan=Detail::TaskTransitionWrite(next,saved.revision,receipt,"recipe_preparation_reconciled",saved.checkpoint.data+backing);
        result.plan.statements.front()+=" AND checkpoint="+SqlValue(saved.checkpoint.data)+
            " AND NOT EXISTS (SELECT 1 FROM character_spell WHERE guid=living_activity_task.actor_guid AND spell="+
            std::to_string(job.recipe)+" AND disabled=0)"
            " AND NOT EXISTS (SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id)"
            " AND NOT EXISTS (SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
            " WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))"
            " AND (SELECT COUNT(*) FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id"
            " AND c.state NOT IN ('released','consumed'))="+std::to_string(claims.claims.size())+backing;
        result.plan.statements.insert(result.plan.statements.begin(),
            "UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+std::to_string(saved.actor));
        blocker.clear();return true;
    }
}
