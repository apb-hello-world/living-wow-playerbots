#pragma once
#include "LivingGuildProcurement.h"
#include "LivingPersonalResourceSettlement.h"

namespace LivingActivity {
// Native possessions are not moved here. This changes their durable obligation
// from procurement claims to carried delivery parcels in one guarded journal.
// Completion means ACQUIRED AND HANDED OFF, never deposited in the guild bank.
struct GuildProcurementHandoff {
    Task task;
    WritePlan plan;
    std::vector<ClaimReceiptChange> claims;
    std::vector<ResourceClaim> parcels;
};
inline std::string GuildProcurementParcelIdentity(const Task& task,const GuildProcurementJob& job,
    const ResourceClaim& claim) {
    return "d.source_task_id="+SqlValue(task.id)+" AND d.source_claim_id="+SqlValue(claim.id)+
        " AND d.guild_id="+std::to_string(job.guild)+" AND d.goal_id="+SqlValue(job.goal)+
        " AND d.donor_guid="+std::to_string(job.donor)+" AND d.item_entry="+std::to_string(job.entry)+
        " AND d.quantity="+std::to_string(claim.quantity);
}
inline bool PrepareGuildProcurementHandoff(const Task& saved,const WorldContext& current,
    const UnsettledClaimBatch& batch,const std::vector<NativeResourceBalance>& balances,
    uint64_t now,const std::string& receipt,GuildProcurementHandoff& out,std::string& why) {
    out={};GuildProcurementJob job;auto reject=[&](const char* reason){why=reason;return false;};
    if(!Validate(saved,why) || !IsGuildProcurementTask(saved) || !ValidateGuildProcurementTask(saved,why) ||
        !DecodeGuildProcurementJob(saved.checkpoint.data,job,why) || saved.mode!=Mode::Active ||
        saved.phase!=Phase::Preparing || !(saved.context==current) || !current.actorGeneration ||
        !current.mapGeneration || !IsUuid(current.boot) || !IsUuid(receipt) || now<saved.updatedAtMs ||
        saved.revision>=UINT64_MAX-1 || !batch.complete || !batch.bookRevision)
        return reject("guild_procurement_handoff_context_invalid");
    PersonalResourceSettlement resources;
    if(!PreparePersonalResourceSettlement(saved,batch,balances,resources,why,"guild_procurement_handoff_"))return false;
    uint64_t quantity=0;std::set<uint32_t> items;
    for(const auto& claim:batch.claims) {
        // Mail/proposals/uncertain transfers are still obligations, not goods.
        if(claim.state!="held" || claim.nativeReference)
            return reject("guild_procurement_handoff_unreconciled_claim");
        if(claim.itemEntry==job.entry) {
            if(claim.location!="bags" || claim.copper || !items.insert(claim.itemGuid).second ||
                claim.quantity>job.quantity || quantity>job.quantity-claim.quantity)
                return reject("guild_procurement_handoff_carried_quantity_invalid");
            quantity+=claim.quantity;out.parcels.push_back(claim);
        } else if(claim.location!="bank" && claim.location!="money")
            return reject("guild_procurement_handoff_unrelated_claim");
    }
    if(quantity!=job.quantity)return reject("guild_procurement_handoff_goods_incomplete");
    out.task=saved;auto& next=out.task;++next.revision;next.phase=Phase::Completed;next.updatedAtMs=now;
    next.checkpoint.step="guild_procurement_handed_off";next.checkpoint.blocker.clear();next.retryAtMs=0;
    next.checkpoint.lastProgressAtMs=now;
    out.plan=Detail::TaskTransitionWrite(next,saved.revision,receipt,"guild_procurement_custody_handed_off",resources.fingerprint);
    const auto n=[](uint64_t v){return std::to_string(v);};
    auto& guard=out.plan.statements.front();
    guard+=" AND phase='preparing' AND mode='active' AND accepted=1 AND checkpoint="+SqlValue(saved.checkpoint.data)+
        " AND EXISTS(SELECT 1 FROM guild_member m JOIN guild g ON g.guildid=m.guildid"
        " JOIN guild_society_policy p ON p.guild_id=g.guildid AND p.leader_guid=g.leaderguid"
        " JOIN guild_society_supply_execution e ON e.guild_id=g.guildid"
        " WHERE m.guid=living_activity_task.actor_guid AND m.guildid="+n(job.guild)+" AND p.supplies=1 AND e.enabled=1)"
        " AND EXISTS(SELECT 1 FROM guild_society_supply_goal g WHERE g.guild_id="+n(job.guild)+
        " AND g.goal_id="+SqlValue(job.goal)+" AND g.item_entry="+n(job.entry)+" AND g.request_kind='item'"
        " AND g.state='active' AND g.provenance IN ('human_request','event_requirement','profession_requirement','equipment_requirement'))"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
        " WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
        " AND (o.state NOT IN ('verified','rejected') OR o.kind NOT IN ('vendor_purchase','auction_purchase',"
        "'mail_collect','bank_withdraw','bank_deposit','capacity_vendor_sale')))"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_task child WHERE child.root_task_id=living_activity_task.task_id"
        " AND child.task_id<>living_activity_task.task_id AND child.phase NOT IN ('completed','cancelled','failed'))"
        " AND NOT EXISTS(SELECT 1 FROM guild_society_supply_delivery d WHERE d.source_task_id=living_activity_task.task_id)"+
        resources.guards;
    // Capacity-preparation leftovers stay personally owned. Do not release a
    // bank claim from some unknown transfer or a still-uncollected purchase.
    for(const auto& c:batch.claims)if(c.location=="bank")
        guard+=" AND EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
            " AND o.kind='bank_deposit' AND o.state='verified' AND o.evidence_code='native_bank_stack_deposited'"
            " AND JSON_EXTRACT(o.before_state,'$.native.native.guid')="+n(c.itemGuid)+
            " AND JSON_EXTRACT(o.before_state,'$.native.native.entry')="+n(c.itemEntry)+')';
    for(const auto& b:balances) {
        if(b.location=="money")guard+=" AND EXISTS(SELECT 1 FROM characters WHERE guid="+n(saved.actor)+" AND money="+n(b.copper)+')';
        else {
            guard+=" AND EXISTS(SELECT 1 FROM character_inventory v JOIN item_instance i ON i.guid=v.item"
                " WHERE v.guid="+n(saved.actor)+" AND i.owner_guid=v.guid AND i.guid="+n(b.itemGuid)+
                " AND i.itemEntry="+n(b.itemEntry)+" AND i.count="+n(b.quantity);
            if(items.count(b.itemGuid))guard+=" AND ((v.bag=0 AND v.slot BETWEEN 23 AND 38) OR EXISTS("
                "SELECT 1 FROM character_inventory bag WHERE bag.guid=v.guid AND bag.item=v.bag"
                " AND bag.bag=0 AND bag.slot BETWEEN 19 AND 22))";
            guard+=") AND NOT EXISTS(SELECT 1 FROM mail_items WHERE item_guid="+n(b.itemGuid)+')'+
                " AND NOT EXISTS(SELECT 1 FROM guild_bank_item WHERE item_guid="+n(b.itemGuid)+')';
        }
    }
    for(const auto& c:out.parcels) {
        // A legacy delivery or another root may not claim the same carried
        // stack. This deliberately defers a mixed protected stack for splitting.
        guard+=" AND NOT EXISTS(SELECT 1 FROM guild_society_supply_delivery d WHERE d.item_guid="+n(c.itemGuid)+
            " AND d.phase NOT IN ('completed','cancelled','failed'))"
            " AND NOT EXISTS(SELECT 1 FROM guild_society_supply_delivery d WHERE d.source_claim_id="+SqlValue(c.id)+')'+
            " AND NOT EXISTS(SELECT 1 FROM living_activity_claim c WHERE c.item_guid="+n(c.itemGuid)+
            " AND c.task_id<>living_activity_task.task_id AND c.state NOT IN ('consumed','released'))";
    }
    out.plan.statements.insert(out.plan.statements.begin(),
        "UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+n(saved.actor));
    // Serializes policy/goal cancellation with this metadata-only transition.
    out.plan.statements.insert(out.plan.statements.begin()+1,
        "UPDATE guild_society_policy SET guild_id=guild_id WHERE guild_id="+n(job.guild));
    out.plan.statements.insert(out.plan.statements.begin()+2,
        "UPDATE guild_society_supply_goal SET guild_id=guild_id WHERE guild_id="+n(job.guild)+" AND goal_id="+SqlValue(job.goal));
    const auto accepted=out.plan.receiptQuery;
    for(const auto& c:out.parcels) {
        out.plan.statements.push_back("INSERT INTO guild_society_supply_delivery (guild_id,goal_id,donor_guid,carrier_guid,"
            "item_guid,item_entry,quantity,created_at,updated_at,source_task_id,source_claim_id) SELECT "+
            n(job.guild)+','+SqlValue(job.goal)+','+n(job.donor)+','+n(job.donor)+','+n(c.itemGuid)+','+n(job.entry)+','+
            n(c.quantity)+','+n(now/1000)+','+n(now/1000)+','+SqlValue(saved.id)+','+SqlValue(c.id)+
            " WHERE EXISTS("+accepted+") AND NOT EXISTS(SELECT 1 FROM guild_society_supply_delivery d WHERE d.source_claim_id="+SqlValue(c.id)+')');
        // Forwarding/splitting changes carrier, item and mail IDs. Immutable
        // origin and quantity still prove the handoff after an acknowledgement
        // is lost. Do not require the parcel to remain at its initial step.
        out.plan.receiptQuery+=" AND EXISTS(SELECT 1 FROM guild_society_supply_delivery d WHERE "+
            GuildProcurementParcelIdentity(saved,job,c)+')';
    }
    AppendPersonalResourceSettlement(out.plan,resources,batch,now,receipt);
    out.claims=std::move(resources.claims);why.clear();return true;
}
}
