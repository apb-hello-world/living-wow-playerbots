#pragma once
#include "LivingGuildDelivery.h"
#include "LivingActivityJournal.h"
#include "LivingActivityResources.h"
namespace LivingActivity {
struct GuildDeliverySettlement {Task task;WritePlan plan;};
inline bool PrepareGuildDeliverySettlement(const Task& saved,const WorldContext& current,
    const UnsettledClaimBatch& claims,uint64_t now,const std::string& receipt,
    GuildDeliverySettlement& result,std::string& blocker) {
    result={};GuildDeliveryJob job;auto reject=[&](const char* why){blocker=why;return false;};
    if(!Validate(saved,blocker) || !ValidateGuildDeliveryTask(saved,blocker) || !IsManagedGuildDelivery(saved) ||
        !DecodeGuildDeliveryJob(saved.checkpoint.data,job,blocker) || job.money ||
        (saved.phase!=Phase::Verifying && saved.phase!=Phase::Reconciling) || !IsUuid(receipt) ||
        saved.revision>=UINT64_MAX-1 || now<saved.updatedAtMs || current.actor!=saved.actor ||
        !current.actorGeneration || !current.mapGeneration || !IsUuid(current.boot))
        return reject("guild_delivery_settlement_context_invalid");
    if(!claims.bookRevision || !claims.complete || !claims.claims.empty())
        return reject("guild_delivery_unsettled_resources");
    result.task=saved;auto& next=result.task;
    ++next.revision;next.context=current;next.phase=Phase::Completed;next.updatedAtMs=now;
    next.checkpoint.step="guild_delivery_completed";next.checkpoint.blocker.clear();next.retryAtMs=0;
    next.checkpoint.lastProgressAtMs=now;
    result.plan=Detail::TaskTransitionWrite(next,saved.revision,receipt,"guild_delivery_completed",saved.checkpoint.data);
    // The copied projection merely decides WHEN to ask. Only these native
    // transaction receipts can authorize a completion, including after restart.
    result.plan.statements.front()+=" AND mode='active' AND accepted=1 AND phase IN ('verifying','reconciling')"
        " AND checkpoint="+SqlValue(saved.checkpoint.data)+
        " AND EXISTS(SELECT 1 FROM guild_society_supply_delivery d WHERE d.delivery_id="+std::to_string(job.delivery)+
        " AND d.guild_id="+std::to_string(job.guild)+" AND d.goal_id="+SqlValue(job.goal)+
        " AND d.donor_guid="+std::to_string(job.donor)+" AND d.carrier_guid=living_activity_task.actor_guid"
        " AND d.item_entry="+std::to_string(job.entry)+" AND d.quantity="+std::to_string(job.quantity)+
        " AND d.mail_id="+std::to_string(job.incomingMail)+" AND d.phase='completed' AND d.deposited_quantity=d.quantity)"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_operation o JOIN living_activity_task owner ON owner.task_id=o.task_id"
        " WHERE owner.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id"
        " AND c.state NOT IN ('consumed','released'))"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
        " AND (o.kind<>'guild_bank_deposit' OR o.state NOT IN ('verified','rejected')"
        " OR (o.state='verified' AND o.evidence_code<>'native_guild_deposit_items_and_stock_observed')"
        " OR COALESCE(JSON_COMPACT(JSON_EXTRACT(o.before_state,'$.native.native.job')),'')<>"+SqlValue(EncodeGuildDeliveryJob(job))+"))"
        " AND (SELECT SUM(CAST(JSON_UNQUOTE(JSON_EXTRACT(o.before_state,'$.native.native.amount')) AS UNSIGNED))"
        " FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id AND o.kind='guild_bank_deposit'"
        " AND o.state='verified')="+std::to_string(job.quantity);
    blocker.clear();return true;
}
}
