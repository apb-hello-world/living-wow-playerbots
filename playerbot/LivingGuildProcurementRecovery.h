#pragma once
#include "LivingGuildProcurement.h"
#include "LivingPersonalResourceSettlement.h"

namespace LivingActivity {
struct GuildProcurementRecovery {Task task;WritePlan plan;std::vector<ClaimReceiptChange> claims;};
inline std::string GuildProcurementOperationGuard(const Task& saved) {
    return " AND NOT EXISTS(SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
        " WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
        " AND (o.state NOT IN ('verified','rejected') OR o.kind NOT IN ('vendor_purchase','auction_purchase',"
        "'mail_collect','bank_withdraw','bank_deposit','capacity_vendor_sale')))"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_task child WHERE child.root_task_id=living_activity_task.task_id"
        " AND child.task_id<>living_activity_task.task_id AND child.phase NOT IN ('completed','cancelled','failed'))"
        " AND NOT EXISTS(SELECT 1 FROM guild_society_supply_delivery WHERE source_task_id="+SqlValue(saved.id)+')';
}
inline std::string GuildProcurementAcquisitionGuard(uint32_t entry) {
    return " AND (NOT EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
        " AND o.state='verified' AND o.kind IN ('vendor_purchase','auction_purchase','mail_collect','bank_withdraw'))"
        " OR EXISTS(SELECT 1 FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id"
        " AND c.state='held' AND c.item_entry="+std::to_string(entry)+" AND c.quantity>0))";
}
// One fresh, native-backed snapshot for preparation restart and cancellation.
// This NEVER relocates goods or rewrites a saved claim's native location.
inline bool GuildProcurementBacking(const Task& saved,const UnsettledClaimBatch& batch,
    const std::vector<NativeResourceBalance>& balances,std::string& guard,std::string& why) {
    guard.clear();auto reject=[&](const char* code){why=code;return false;};
    if(!batch.complete || !batch.bookRevision || batch.claims.size()>16 || balances.size()>16)
        return reject("guild_procurement_recovery_claims_incomplete");
    std::map<uint32_t,NativeResourceBalance> stock;std::map<uint32_t,uint64_t> totals;std::set<std::string> ids;
    for(const auto& b:balances)
        if(!ValidNativeResourceBalance(b) || b.actor!=saved.actor || !stock.emplace(b.itemGuid,b).second)
            return reject("guild_procurement_recovery_native_balance_invalid");
    for(const auto& c:batch.claims) {
        const auto found=stock.find(c.itemGuid);
        if(!ValidResourceClaim(c) || c.task!=saved.id || c.actor!=saved.actor || c.state!="held" ||
            c.revision>=UINT64_MAX-1 || !ids.insert(c.id).second || found==stock.end())
            return reject("guild_procurement_recovery_claim_unreconciled");
        const auto& b=found->second;const uint64_t amount=c.copper?c.copper:c.quantity,available=b.copper?b.copper:b.quantity;
        if(c.itemEntry!=b.itemEntry || c.location!=b.location || c.nativeReference!=b.nativeReference ||
            amount>available || totals[c.itemGuid]>available-amount)
            return reject("guild_procurement_recovery_native_stock_changed");
        totals[c.itemGuid]+=amount;
        guard+=" AND EXISTS(SELECT 1 FROM living_activity_claim c WHERE "+SettlementClaimWhere(c)+')';
    }
    guard+=" AND (SELECT COUNT(*) FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id"
        " AND c.state NOT IN ('released','consumed'))="+std::to_string(batch.claims.size());
    for(const auto& row:stock) {
        const auto& b=row.second;const auto n=[](uint64_t v){return std::to_string(v);};
        if(!totals.count(b.itemGuid))return reject("guild_procurement_recovery_unclaimed_native_balance");
        if(b.location=="money") {guard+=" AND EXISTS(SELECT 1 FROM characters WHERE guid="+n(saved.actor)+" AND money="+n(b.copper)+')';continue;}
        guard+=" AND EXISTS(SELECT 1 FROM item_instance WHERE guid="+n(b.itemGuid)+" AND owner_guid="+n(saved.actor)+
            " AND itemEntry="+n(b.itemEntry)+" AND count="+n(b.quantity)+')';
        if(b.location=="mail")
            guard+=" AND EXISTS(SELECT 1 FROM mail_items mi JOIN mail m ON m.id=mi.mail_id WHERE mi.item_guid="+
                n(b.itemGuid)+" AND mi.item_template="+n(b.itemEntry)+" AND m.id="+n(b.nativeReference)+" AND m.receiver="+
                n(saved.actor)+" AND m.cod=0) AND NOT EXISTS(SELECT 1 FROM character_inventory WHERE item="+n(b.itemGuid)+')';
        else {
            std::string location;
            if(b.location=="bags")location="((v.bag=0 AND v.slot BETWEEN 23 AND 38) OR EXISTS(SELECT 1 FROM character_inventory bag"
                " WHERE bag.guid=v.guid AND bag.item=v.bag AND bag.bag=0 AND bag.slot BETWEEN 19 AND 22))";
            else if(b.location=="bank")location="((v.bag=0 AND v.slot BETWEEN 39 AND 66) OR EXISTS(SELECT 1 FROM character_inventory bag"
                " WHERE bag.guid=v.guid AND bag.item=v.bag AND bag.bag=0 AND bag.slot BETWEEN 67 AND 73))";
            else return reject("guild_procurement_recovery_location_unsupported");
            guard+=" AND EXISTS(SELECT 1 FROM character_inventory v WHERE v.guid="+n(saved.actor)+" AND v.item="+n(b.itemGuid)+
                " AND v.item_template="+n(b.itemEntry)+" AND "+location+") AND NOT EXISTS(SELECT 1 FROM mail_items WHERE item_guid="+n(b.itemGuid)+')';
        }
        guard+=" AND NOT EXISTS(SELECT 1 FROM guild_bank_item WHERE item_guid="+n(b.itemGuid)+')';
    }
    why.clear();return true;
}
inline bool PrepareGuildProcurementResumption(const Task& saved,const WorldContext& current,
    const UnsettledClaimBatch& batch,const std::vector<NativeResourceBalance>& balances,uint64_t now,
    const std::string& receipt,GuildProcurementRecovery& out,std::string& why) {
    out={};GuildProcurementJob job;auto reject=[&](const char* code){why=code;return false;};
    if(!Validate(saved,why) || !ValidateGuildProcurementTask(saved,why) || !IsGuildProcurementTask(saved) ||
        !DecodeGuildProcurementJob(saved.checkpoint.data,job,why) || saved.mode!=Mode::Active || Terminal(saved.phase) ||
        (saved.context==current && saved.phase!=Phase::Verifying && saved.phase!=Phase::Paused &&
            saved.phase!=Phase::Deferred && saved.phase!=Phase::WaitingExternal) ||
        current.actor!=saved.actor || !current.actorGeneration || !current.mapGeneration || !IsUuid(current.boot) ||
        !IsUuid(receipt) || now<saved.updatedAtMs || saved.revision>=UINT64_MAX-1)
        return reject("guild_procurement_resume_context_invalid");
    std::string backing;
    if(!GuildProcurementBacking(saved,batch,balances,backing,why))return false;
    out.task=saved;auto& next=out.task;++next.revision;next.context=current;next.phase=Phase::Preparing;
    next.updatedAtMs=now;next.checkpoint.step="guild_procurement_prepare";next.checkpoint.blocker.clear();
    // Rebinding safety/world context is not progress and does not reset active
    // time, due time, original task identity, paid receipts or retry deadlines.
    out.plan=Detail::TaskTransitionWrite(next,saved.revision,receipt,"guild_procurement_preparation_reconciled",saved.checkpoint.data+backing);
    out.plan.statements.front()+=" AND mode='active' AND accepted=1 AND checkpoint="+SqlValue(saved.checkpoint.data)+
        GuildProcurementOperationGuard(saved)+backing+
        // A committed acquisition must still have its claimed goods. Never
        // turn a missing claim after restart into another purchase.
        GuildProcurementAcquisitionGuard(job.entry);
    out.plan.statements.insert(out.plan.statements.begin(),
        "UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+std::to_string(saved.actor));
    why.clear();return true;
}
// A positive native goal row is required. A cache miss, unavailable database,
// temporary delegation pause or route timeout can NEVER cancel an obligation.
struct GuildProcurementClosure {
    std::string state,kind;
    uint32_t entry=0,target=0,reserved=0,updated=0;
    uint64_t banked=0;
};
inline std::string GuildProcurementBankCount(const GuildProcurementJob& job) {
    return "(SELECT COALESCE(SUM(i.count),0) FROM guild_bank_item b JOIN item_instance i ON i.guid=b.item_guid"
        " WHERE b.guildid="+std::to_string(job.guild)+" AND b.item_entry="+std::to_string(job.entry)+" AND i.itemEntry=b.item_entry)";
}
inline std::string GuildProcurementClosureQuery(const GuildProcurementJob& job) {
    return "SELECT state,request_kind,item_entry,required_quantity,reserved_quantity,updated_at,"+GuildProcurementBankCount(job)+
        " FROM guild_society_supply_goal WHERE guild_id="+std::to_string(job.guild)+" AND goal_id="+SqlValue(job.goal);
}
inline std::string GuildProcurementClosureReason(const GuildProcurementJob& job,const GuildProcurementClosure& goal) {
    if(goal.state=="cancelled")return "guild_procurement_cancelled_items_preserved";
    if(goal.state!="active" && goal.state!="completed")return "";
    if(goal.kind!="item" || goal.entry!=job.entry)return "guild_procurement_changed_request_items_preserved";
    const auto usable=goal.banked>goal.reserved?goal.banked-goal.reserved:0;
    if(goal.target && usable>=goal.target)return "guild_procurement_surplus_items_preserved";
    return "";
}
inline bool PrepareGuildProcurementCancellation(const Task& saved,const WorldContext& current,
    const GuildProcurementClosure& goal,const UnsettledClaimBatch& batch,const std::vector<NativeResourceBalance>& balances,
    uint64_t now,const std::string& receipt,GuildProcurementRecovery& out,std::string& why) {
    out={};GuildProcurementJob job;auto reject=[&](const char* code){why=code;return false;};
    if(!Validate(saved,why) || !ValidateGuildProcurementTask(saved,why) || !IsGuildProcurementTask(saved) ||
        !DecodeGuildProcurementJob(saved.checkpoint.data,job,why) || saved.mode!=Mode::Active || saved.phase!=Phase::Preparing ||
        !(saved.context==current) || !current.actorGeneration || !current.mapGeneration || !IsUuid(current.boot) ||
        !IsUuid(receipt) || now<saved.updatedAtMs || saved.revision>=UINT64_MAX-1)
        return reject("guild_procurement_cancellation_context_invalid");
    const auto reason=GuildProcurementClosureReason(job,goal);
    if(reason.empty())return reject("guild_procurement_closure_not_authoritative");
    // Paid attachments must be collected (or reconciled if expired) first,
    // even when cancelled. Never release them because demand disappeared.
    for(const auto& c:batch.claims)if(c.location=="mail")return reject("guild_procurement_cancel_collect_paid_mail_first");
    std::string backing;PersonalResourceSettlement resources;
    if(!GuildProcurementBacking(saved,batch,balances,backing,why) ||
        !PreparePersonalResourceSettlement(saved,batch,balances,resources,why,"guild_procurement_cancel_"))return false;
    const auto n=[](uint64_t v){return std::to_string(v);};
    out.task=saved;auto& next=out.task;++next.revision;next.phase=Phase::Cancelled;next.updatedAtMs=now;
    next.checkpoint.step="guild_procurement_closed";next.checkpoint.blocker=reason;next.retryAtMs=0;
    out.plan=Detail::TaskTransitionWrite(next,saved.revision,receipt,reason,resources.fingerprint+backing+'|'+goal.state+'|'+n(goal.updated));
    out.plan.statements.front()+=" AND phase='preparing' AND mode='active' AND accepted=1 AND checkpoint="+SqlValue(saved.checkpoint.data)+
        GuildProcurementOperationGuard(saved)+GuildProcurementAcquisitionGuard(job.entry)+backing+resources.guards+
        " AND EXISTS(SELECT 1 FROM guild_society_supply_goal g WHERE g.guild_id="+n(job.guild)+" AND g.goal_id="+SqlValue(job.goal)+
        " AND g.state="+SqlValue(goal.state)+" AND g.request_kind="+SqlValue(goal.kind)+" AND g.item_entry="+n(goal.entry)+
        " AND g.required_quantity="+n(goal.target)+" AND g.reserved_quantity="+n(goal.reserved)+" AND g.updated_at="+n(goal.updated)+')';
    if(reason=="guild_procurement_surplus_items_preserved")
        out.plan.statements.front()+=" AND "+GuildProcurementBankCount(job)+">="+n(uint64_t(goal.target)+goal.reserved);
    out.plan.statements.insert(out.plan.statements.begin(),
        "UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+n(saved.actor));
    out.plan.statements.insert(out.plan.statements.begin()+1,
        "UPDATE guild_society_supply_goal SET guild_id=guild_id WHERE guild_id="+n(job.guild)+" AND goal_id="+SqlValue(job.goal));
    AppendPersonalResourceSettlement(out.plan,resources,batch,now,receipt);out.claims=std::move(resources.claims);
    why.clear();return true;
}
}
