#pragma once
#include "LivingGuildDelivery.h"
#include "LivingActivityJournal.h"
#include "LivingActivityResources.h"
#include "LivingPersonalResourceSettlement.h"
namespace LivingActivity {
struct GuildDeliverySettlement {Task task;WritePlan plan;std::vector<ClaimReceiptChange> claims;};
inline bool PrepareGuildDeliverySettlement(const Task& saved,const WorldContext& current,
    const UnsettledClaimBatch& claims,uint64_t now,const std::string& receipt,
    GuildDeliverySettlement& result,std::string& blocker,const std::vector<NativeResourceBalance>& balances={}) {
    result={};GuildDeliveryJob job;auto reject=[&](const char* why){blocker=why;return false;};
    if(!Validate(saved,blocker) || !ValidateGuildDeliveryTask(saved,blocker) || !IsManagedGuildDelivery(saved) ||
        !DecodeGuildDeliveryJob(saved.checkpoint.data,job,blocker) || job.money || saved.mode!=Mode::Active || !saved.accepted ||
        (saved.phase!=Phase::Verifying && saved.phase!=Phase::Reconciling) || !IsUuid(receipt) ||
        saved.revision>=UINT64_MAX-1 || now<saved.updatedAtMs || current.actor!=saved.actor ||
        !current.actorGeneration || !current.mapGeneration || !IsUuid(current.boot))
        return reject("guild_delivery_settlement_context_invalid");
    if(!claims.bookRevision || !claims.complete)
        return reject("guild_delivery_unsettled_resources");
    // Capacity preparation may have banked unrelated personal stock. Release
    // only its backed reservation, never the item or unresolved delivery goods.
    for(const auto& claim:claims.claims)
        if(!job.incomingMail || claim.state!="held" || claim.location!="bank" || claim.copper ||
            claim.nativeReference || claim.itemEntry==job.entry)
            return reject("guild_delivery_unsettled_resources");
    PersonalResourceSettlement resources;
    if(!PreparePersonalResourceSettlement(saved,claims,balances,resources,blocker,"guild_delivery_"))return false;
    result.task=saved;auto& next=result.task;
    ++next.revision;next.context=current;next.phase=Phase::Completed;next.updatedAtMs=now;
    next.checkpoint.step="guild_delivery_completed";next.checkpoint.blocker.clear();next.retryAtMs=0;
    next.checkpoint.lastProgressAtMs=now;
    result.plan=Detail::TaskTransitionWrite(next,saved.revision,receipt,"guild_delivery_completed",resources.fingerprint);
    const auto number=[](const char* key) {
        return "COALESCE(CAST(JSON_UNQUOTE(JSON_EXTRACT(o.before_state,'$.native.native."+std::string(key)+"')) AS UNSIGNED),0)";
    };
    const auto amount=std::to_string(job.quantity),entry=std::to_string(job.entry);
    const auto deposit="(o.kind='guild_bank_deposit' AND COALESCE(JSON_COMPACT(JSON_EXTRACT(o.before_state,'$.native.native.job')),'')="+
        SqlValue(EncodeGuildDeliveryJob(job))+" AND "+number("amount")+">0 AND "+number("amount")+"<="+amount+
        " AND (o.state='rejected' OR o.evidence_code='native_guild_deposit_items_and_stock_observed'))";
    const auto mail="(o.kind='mail_collect' AND "+std::to_string(job.incomingMail)+">0 AND "+number("mail")+'='+std::to_string(job.incomingMail)+
        " AND "+number("entry")+'='+entry+" AND "+number("quantity")+'='+amount+
        " AND EXISTS(SELECT 1 FROM guild_society_supply_delivery parcel WHERE parcel.delivery_id="+std::to_string(job.delivery)+
        " AND parcel.item_guid="+number("guid")+") AND (o.state='rejected' OR o.evidence_code='native_mail_attachment_collected'))";
    const auto capacity="("+std::to_string(job.incomingMail)+">0 AND "+number("entry")+"<>"+entry+" AND "+number("entry")+">0 AND ("
        "(o.kind='capacity_vendor_sale' AND "+number("capacity_entry")+'='+entry+" AND "+number("capacity_quantity")+'='+amount+
        " AND (o.state='rejected' OR o.evidence_code='native_capacity_sale_money_item_and_slot_observed')) OR "
        "(o.kind='bank_deposit' AND COALESCE(JSON_UNQUOTE(JSON_EXTRACT(o.before_state,'$.native.native.deposit')),'')='true'"
        " AND (o.state='rejected' OR o.evidence_code='native_bank_stack_deposited'))))";
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
        " AND NOT EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
        " AND (o.state NOT IN ('verified','rejected') OR "+number("actor")+"<>living_activity_task.actor_guid"
        " OR NOT("+deposit+" OR "+mail+" OR "+capacity+")))"
        " AND (SELECT COUNT(*) FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
        " AND o.kind='mail_collect' AND o.state='verified')="+std::to_string(job.incomingMail?1:0)+
        " AND (SELECT SUM(CAST(JSON_UNQUOTE(JSON_EXTRACT(o.before_state,'$.native.native.amount')) AS UNSIGNED))"
        " FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id AND o.kind='guild_bank_deposit'"
        " AND o.state='verified')="+amount+resources.guards;
    // A residual bank claim must originate in this task's capacity operation.
    for(const auto& claim:claims.claims)
        result.plan.statements.front()+=" AND EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
            " AND o.kind='bank_deposit' AND o.state='verified' AND "+number("guid")+'='+std::to_string(claim.itemGuid)+
            " AND "+number("entry")+'='+std::to_string(claim.itemEntry)+" AND "+number("quantity")+'='+std::to_string(claim.quantity)+')';
    result.plan.statements.insert(result.plan.statements.begin(),
        "UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+std::to_string(saved.actor));
    AppendPersonalResourceSettlement(result.plan,resources,claims,now,receipt);
    result.claims=std::move(resources.claims);
    blocker.clear();return true;
}
}
