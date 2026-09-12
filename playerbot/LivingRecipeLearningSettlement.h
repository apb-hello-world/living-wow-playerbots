#pragma once
#include "LivingRecipeLearning.h"
#include "LivingActivityJournal.h"
#include "LivingActivityResources.h"
#include "LivingPersonalResourceSettlement.h"
#include <map>

namespace LivingActivity {
    struct RecipeLearningSettlement {Task task;WritePlan plan;std::vector<ClaimReceiptChange> claims;};
    inline bool PrepareRecipeLearningSettlement(const Task& saved,const WorldContext& current,
        const UnsettledClaimBatch& claims,uint64_t now,const std::string& receipt,
        RecipeLearningSettlement& result,std::string& blocker,const std::vector<NativeResourceBalance>& balances={}) {
        result={};RecipeLearningJob job;
        auto reject=[&](const char* why){blocker=why;return false;};
        if (!Validate(saved,blocker) || !ValidateRecipeLearningTask(saved,blocker) || !IsRecipeLearningTask(saved) ||
            !DecodeRecipeLearningJob(saved.checkpoint.data,job,blocker) || saved.mode!=Mode::Active || !saved.accepted ||
            (saved.phase!=Phase::Verifying && saved.phase!=Phase::Reconciling) || !IsUuid(receipt) ||
            saved.revision>=std::numeric_limits<uint64_t>::max()-1 || now<saved.updatedAtMs ||
            current.actor!=saved.actor || !current.actorGeneration || !current.mapGeneration || !IsUuid(current.boot))
            return reject("recipe_settlement_saved_context_required");
        if (!claims.bookRevision || !claims.complete)
            return reject("recipe_settlement_unfinished_claims");
        PersonalResourceSettlement resources;
        if (!PreparePersonalResourceSettlement(saved,claims,balances,resources,blocker,"recipe_settlement_")) return false;
        // A still-held learning book is not a leftover: its consumption must be
        // reconciled with the original cast, not released because a spell exists.
        for (const auto& claim:claims.claims) if (claim.itemEntry==job.book)
            return reject("recipe_settlement_book_consumption_unreconciled");
        result.task=saved;auto& next=result.task;
        ++next.revision;next.updatedAtMs=now;next.context=current;next.phase=Phase::Completed;
        next.checkpoint.step="recipe_completed";next.checkpoint.blocker.clear();next.retryAtMs=0;next.checkpoint.lastProgressAtMs=now;
        result.plan=Detail::TaskTransitionWrite(next,saved.revision,receipt,"recipe_learning_completed",resources.fingerprint);
        // Applied in the same guarded transaction as the terminal transition.
        // A known spell alone, elapsed time, or a model's claim cannot pass.
        result.plan.statements.front()+=" AND mode='active' AND accepted=1 AND phase IN ('verifying','reconciling')"
            " AND checkpoint="+SqlValue(saved.checkpoint.data)+
            " AND EXISTS (SELECT 1 FROM character_spell WHERE guid=living_activity_task.actor_guid AND spell="+
            std::to_string(job.recipe)+" AND disabled=0)"
            " AND NOT EXISTS (SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
            " WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))"
            " AND NOT EXISTS (SELECT 1 FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id"
            " AND c.state IN ('in_transfer','reconciling'))"
            " AND NOT EXISTS (SELECT 1 FROM living_activity_task child WHERE child.parent_task_id=living_activity_task.task_id"
            " AND child.phase NOT IN ('completed','cancelled','failed'))"
            " AND (SELECT COUNT(*) FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
            " AND o.kind='recipe_learning' AND o.state='verified')=1"
            " AND EXISTS (SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
            " AND o.kind='recipe_learning' AND o.state='verified' AND o.evidence_code='native_recipe_book_consumed_and_spell_learned'"
            " AND CAST(JSON_UNQUOTE(JSON_EXTRACT(o.after_state,'$.native.recipe')) AS UNSIGNED)="+std::to_string(job.recipe)+
            " AND CAST(JSON_UNQUOTE(JSON_EXTRACT(o.after_state,'$.native.skill_id')) AS UNSIGNED)="+std::to_string(job.skill)+
            " AND CAST(JSON_UNQUOTE(JSON_EXTRACT(o.after_state,'$.native.before.entry')) AS UNSIGNED)="+std::to_string(job.book)+')'+resources.guards;
        result.plan.statements.insert(result.plan.statements.begin(),
            "UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+std::to_string(saved.actor));
        AppendPersonalResourceSettlement(result.plan,resources,claims,now,receipt);
        result.claims=std::move(resources.claims);
        blocker.clear();return true;
    }
    inline bool PrepareRecipeLearningResumption(const Task& saved,const WorldContext& current,
        const UnsettledClaimBatch& claims,const std::vector<NativeResourceBalance>& stock,uint64_t now,
        const std::string& receipt,RecipeLearningSettlement& result,std::string& blocker) {
        result={};RecipeLearningJob job;
        auto reject=[&](const char* why){blocker=why;return false;};
        if (!Validate(saved,blocker) || !IsRecipeLearningTask(saved) || !ValidateRecipeLearningTask(saved,blocker) ||
            !DecodeRecipeLearningJob(saved.checkpoint.data,job,blocker) || saved.mode!=Mode::Active || !saved.accepted ||
            Terminal(saved.phase) || (saved.context==current && saved.phase!=Phase::Verifying && saved.phase!=Phase::Paused &&
                saved.phase!=Phase::Deferred && saved.phase!=Phase::WaitingExternal) ||
            current.actor!=saved.actor || !current.actorGeneration ||
            !current.mapGeneration || !IsUuid(current.boot) || !IsUuid(receipt) || now<saved.updatedAtMs ||
            saved.revision>=std::numeric_limits<uint64_t>::max()-1 || !claims.bookRevision || !claims.complete ||
            claims.claims.size()>16 || stock.size()>16) return reject("recipe_resume_context_or_claims_invalid");
        std::map<uint32_t,NativeResourceBalance> owned;
        for (const auto& native:stock) if (!ValidNativeResourceBalance(native) || native.actor!=saved.actor ||
            !owned.emplace(native.itemGuid,native).second) return reject("recipe_resume_native_balance_invalid");
        std::string backing;std::map<uint32_t,uint64_t> totals;std::set<std::string> ids;
        for (const auto& claim:claims.claims) {
            const auto found=owned.find(claim.itemGuid);
            if (!ValidResourceClaim(claim) || claim.task!=saved.id || claim.actor!=saved.actor || claim.state!="held" ||
                !ids.insert(claim.id).second || found==owned.end())
                return reject("recipe_resume_original_book_not_backed");
            const auto& native=found->second;
            const auto amount=claim.copper?claim.copper:claim.quantity;
            const auto available=claim.copper?native.copper:native.quantity;
            if (native.itemEntry!=claim.itemEntry || native.location!=claim.location || native.nativeReference!=claim.nativeReference ||
                amount>available || totals[claim.itemGuid]>available-amount ||
                (claim.itemEntry==job.book && claim.quantity!=1))
                return reject("recipe_resume_original_book_not_backed");
            totals[claim.itemGuid]+=amount;
            const auto match="c.claim_id="+SqlValue(claim.id)+" AND c.task_id="+SqlValue(saved.id)+
                " AND c.actor_guid="+std::to_string(saved.actor)+" AND c.revision="+std::to_string(claim.revision)+
                " AND c.item_guid="+std::to_string(claim.itemGuid)+" AND c.item_entry="+std::to_string(claim.itemEntry)+
                " AND c.state='held' AND c.location="+SqlValue(claim.location)+" AND c.quantity="+std::to_string(claim.quantity)+
                " AND c.copper="+std::to_string(claim.copper)+" AND c.native_reference="+std::to_string(claim.nativeReference);
            backing+=" AND EXISTS (SELECT 1 FROM living_activity_claim c WHERE "+match+')';
            if (claim.copper) {
                backing+=" AND EXISTS (SELECT 1 FROM characters WHERE guid="+std::to_string(saved.actor)+
                    " AND money>="+std::to_string(native.copper)+')';
            } else {
                backing+=" AND EXISTS (SELECT 1 FROM item_instance WHERE guid="+std::to_string(claim.itemGuid)+
                    " AND owner_guid="+std::to_string(saved.actor)+" AND itemEntry="+std::to_string(claim.itemEntry)+
                    " AND count="+std::to_string(native.quantity)+')';
                if (claim.location=="mail")
                    backing+=" AND EXISTS (SELECT 1 FROM mail_items mi JOIN mail m ON m.id=mi.mail_id WHERE mi.item_guid="+
                        std::to_string(claim.itemGuid)+" AND m.id="+std::to_string(claim.nativeReference)+" AND m.receiver="+
                        std::to_string(saved.actor)+" AND m.cod=0)";
                else
                    backing+=" AND EXISTS (SELECT 1 FROM character_inventory WHERE guid="+std::to_string(saved.actor)+
                        " AND item="+std::to_string(claim.itemGuid)+')';
            }
        }
        result.task=saved;auto& next=result.task;++next.revision;next.context=current;next.phase=Phase::Preparing;
        next.updatedAtMs=now;next.checkpoint.step="recipe_prepare";next.checkpoint.blocker.clear();
        result.plan=Detail::TaskTransitionWrite(next,saved.revision,receipt,"recipe_preparation_reconciled",saved.checkpoint.data+backing);
        result.plan.statements.front()+=" AND checkpoint="+SqlValue(saved.checkpoint.data)+
            " AND NOT EXISTS (SELECT 1 FROM character_spell WHERE guid=living_activity_task.actor_guid AND spell="+
            std::to_string(job.recipe)+" AND disabled=0)"
            // Verified service work is not a learning attempt. Preserve its
            // receipts and claims; never replay a learning cast or uncertain step.
            " AND NOT EXISTS (SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
            " AND (o.kind NOT IN ('vendor_purchase','mail_collect','bank_withdraw','bank_deposit','capacity_vendor_sale')"
            " OR o.state NOT IN ('verified','rejected')))"
            " AND NOT EXISTS (SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
            " WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))"
            " AND (NOT EXISTS (SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
            " AND o.state='verified' AND o.kind IN ('vendor_purchase','mail_collect','bank_withdraw'))"
            " OR EXISTS (SELECT 1 FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id"
            " AND c.state='held' AND c.item_entry="+std::to_string(job.book)+" AND c.quantity=1))"
            " AND (SELECT COUNT(*) FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id"
            " AND c.state NOT IN ('released','consumed'))="+std::to_string(claims.claims.size())+backing;
        result.plan.statements.insert(result.plan.statements.begin(),
            "UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+std::to_string(saved.actor));
        blocker.clear();return true;
    }
}
