#pragma once
#include "LivingGuildDeliverySettlement.h"

namespace LivingActivity {
// A cache miss can request this read, but cannot cancel anything. The same
// native predicates are checked again inside the terminal journal transaction.
struct GuildDeliveryClosure {
    std::string goalState;
    uint32_t target=0,reserved=0,goalUpdated=0,deposited=0,originalItem=0;
    uint64_t banked=0;
};
inline std::string GuildDeliveryIdentity(const GuildDeliveryJob& j,uint32_t actor) {
    return "d.delivery_id="+std::to_string(j.delivery)+" AND d.guild_id="+std::to_string(j.guild)+
        " AND d.goal_id="+SqlValue(j.goal)+" AND d.donor_guid="+std::to_string(j.donor)+
        " AND d.carrier_guid="+std::to_string(actor)+" AND d.item_entry="+std::to_string(j.entry)+
        " AND d.quantity="+std::to_string(j.quantity)+" AND d.mail_id="+std::to_string(j.incomingMail);
}
inline std::string GuildDeliveryBankCount(const GuildDeliveryJob& j) {
    return "(SELECT COALESCE(SUM(i.count),0) FROM guild_bank_item b JOIN item_instance i ON i.guid=b.item_guid"
        " WHERE b.guildid="+std::to_string(j.guild)+" AND b.item_entry="+std::to_string(j.entry)+
        " AND i.itemEntry=b.item_entry)";
}
inline std::string GuildDeliveryClosureQuery(const GuildDeliveryJob& j,uint32_t actor) {
    return "SELECT COALESCE(g.state,''),COALESCE(g.required_quantity,0),COALESCE(g.reserved_quantity,0),"
        "COALESCE(g.updated_at,0),d.deposited_quantity,d.item_guid,"+GuildDeliveryBankCount(j)+
        " FROM guild_society_supply_delivery d LEFT JOIN guild_society_supply_goal g ON g.goal_id=d.goal_id"
        " AND g.guild_id=d.guild_id AND g.request_kind='item' AND g.item_entry=d.item_entry WHERE "+
        GuildDeliveryIdentity(j,actor)+" AND d.phase='carried'";
}
inline std::string GuildDeliveryClosureReason(const GuildDeliveryClosure& c) {
    if(c.goalState=="cancelled")return "guild_delivery_cancelled_items_preserved";
    if((c.goalState=="active" || c.goalState=="completed") && c.target && c.banked>=c.target)
        return "guild_delivery_surplus_items_preserved";
    return "";
}
inline bool PrepareGuildDeliveryCancellation(const Task& saved,const WorldContext& current,
    const GuildDeliveryClosure& native,const NativeResourceBalance& parcel,const UnsettledClaimBatch& batch,
    const std::vector<NativeResourceBalance>& balances,uint64_t now,const std::string& receipt,
    GuildDeliverySettlement& out,std::string& why) {
    out={};GuildDeliveryJob j;auto reject=[&](const char* code){why=code;return false;};
    const auto reason=GuildDeliveryClosureReason(native);
    if(!Validate(saved,why) || !ValidateGuildDeliveryTask(saved,why) || !IsManagedGuildDelivery(saved) ||
        !DecodeGuildDeliveryJob(saved.checkpoint.data,j,why) || j.money || saved.phase!=Phase::Preparing ||
        !(saved.context==current) || !current.actorGeneration || !current.mapGeneration || !IsUuid(current.boot) ||
        !IsUuid(receipt) || now<saved.updatedAtMs || saved.revision>=UINT64_MAX-1 || !batch.complete || !batch.bookRevision)
        return reject("guild_delivery_cancellation_context_invalid");
    if(reason.empty())return reject("guild_delivery_closure_not_authoritative");
    if(native.deposited>=j.quantity || !native.originalItem || !ValidNativeResourceBalance(parcel) ||
        parcel.actor!=saved.actor || parcel.location!="bags" || parcel.nativeReference || !parcel.itemGuid ||
        parcel.itemEntry!=j.entry || parcel.quantity<j.quantity-native.deposited ||
        (!j.incomingMail && parcel.itemGuid!=native.originalItem))
        return reject("guild_delivery_cancellation_parcel_unreconciled");
    unsigned parcelClaims=0,moneyClaims=0;
    for(const auto& c:batch.claims) {
        if(c.state!="held" || c.nativeReference)return reject("guild_delivery_cancellation_claim_unreconciled");
        if(c.itemEntry==j.entry && c.location=="bags" && c.itemGuid==parcel.itemGuid &&
            c.quantity==j.quantity-native.deposited && !c.copper)++parcelClaims;
        else if(c.location=="money" && c.copper==30 && !c.itemGuid)++moneyClaims;
        else if(!j.incomingMail || c.location!="bank" || c.itemEntry==j.entry || c.copper)
            return reject("guild_delivery_cancellation_claim_unreconciled");
    }
    // Collected/merged parcels must keep their exact receipt-derived claim.
    if(parcelClaims>1 || moneyClaims>1 || (j.incomingMail && parcelClaims!=1))
        return reject("guild_delivery_cancellation_claim_unreconciled");
    PersonalResourceSettlement resources;
    if(!PreparePersonalResourceSettlement(saved,batch,balances,resources,why,"guild_delivery_cancellation_"))return false;
    out.task=saved;auto& next=out.task;++next.revision;next.phase=Phase::Cancelled;next.updatedAtMs=now;
    next.checkpoint.step="guild_delivery_closed";next.checkpoint.blocker=reason;next.retryAtMs=0;
    const auto n=[](uint64_t v){return std::to_string(v);};
    const auto number=[](const char* key){return "COALESCE(CAST(JSON_UNQUOTE(JSON_EXTRACT(o.before_state,'$.native.native."+
        std::string(key)+"')) AS UNSIGNED),0)";};
    auto fingerprint=resources.fingerprint+'|'+reason+'|'+native.goalState+'|'+n(native.target)+'|'+n(native.reserved)+
        '|'+n(native.goalUpdated)+'|'+n(native.deposited)+'|'+n(native.originalItem)+'|'+n(parcel.itemGuid)+'|'+n(parcel.quantity);
    out.plan=Detail::TaskTransitionWrite(next,saved.revision,receipt,reason,fingerprint);
    auto& guard=out.plan.statements.front();
    guard+=" AND phase='preparing' AND checkpoint="+SqlValue(saved.checkpoint.data)+
        " AND EXISTS(SELECT 1 FROM guild_society_supply_delivery d WHERE "+GuildDeliveryIdentity(j,saved.actor)+
        " AND d.phase='carried' AND d.item_guid="+n(native.originalItem)+" AND d.deposited_quantity="+n(native.deposited)+')'+
        " AND EXISTS(SELECT 1 FROM guild_society_supply_goal g WHERE g.guild_id="+n(j.guild)+" AND g.goal_id="+SqlValue(j.goal)+
        " AND g.request_kind='item' AND g.item_entry="+n(j.entry)+" AND g.state="+SqlValue(native.goalState)+
        " AND g.required_quantity="+n(native.target)+" AND g.reserved_quantity="+n(native.reserved)+
        " AND g.updated_at="+n(native.goalUpdated)+')';
    if(native.goalState!="cancelled")guard+=" AND "+GuildDeliveryBankCount(j)+">="+n(native.target);
    guard+=" AND NOT EXISTS(SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
        " WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_task child WHERE child.root_task_id=living_activity_task.task_id"
        " AND child.task_id<>living_activity_task.task_id AND child.phase NOT IN ('completed','cancelled','failed'))";
    // Known native operations only. A send that actually committed belongs to
    // its receiver, and must take the handoff settlement path instead.
    const auto deposit="(o.kind='guild_bank_deposit' AND JSON_COMPACT(JSON_EXTRACT(o.before_state,'$.native.native.job'))="+
        SqlValue(EncodeGuildDeliveryJob(j))+" AND "+number("amount")+">0 AND (o.state='rejected' OR"
        " o.evidence_code='native_guild_deposit_items_and_stock_observed'))";
    const auto mail="(o.kind='mail_collect' AND "+n(j.incomingMail)+">0 AND "+number("mail")+'='+n(j.incomingMail)+
        " AND "+number("guid")+'='+n(native.originalItem)+" AND "+number("entry")+'='+n(j.entry)+
        " AND "+number("quantity")+'='+n(j.quantity)+" AND (o.state='rejected' OR o.evidence_code='native_mail_attachment_collected'))";
    const auto capacity="("+n(j.incomingMail)+">0 AND "+number("entry")+"<>"+n(j.entry)+" AND "+number("entry")+">0 AND ("
        "(o.kind='capacity_vendor_sale' AND "+number("capacity_entry")+'='+n(j.entry)+" AND "+number("capacity_quantity")+'='+n(j.quantity)+
        " AND (o.state='rejected' OR o.evidence_code='native_capacity_sale_money_item_and_slot_observed')) OR"
        " (o.kind='bank_deposit' AND JSON_UNQUOTE(JSON_EXTRACT(o.before_state,'$.native.native.deposit'))='true'"
        " AND (o.state='rejected' OR o.evidence_code='native_bank_stack_deposited'))))";
    const auto unsent="(o.kind='guild_mail_send' AND o.state='rejected' AND JSON_COMPACT(JSON_EXTRACT(o.before_state,'$.native.native.job'))="+
        SqlValue(EncodeGuildDeliveryJob(j))+')';
    guard+=" AND NOT EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id AND"
        " (o.state NOT IN ('verified','rejected') OR IF(o.kind='guild_mail_send',"+number("sender")+','+number("actor")+
        ")<>living_activity_task.actor_guid OR NOT("+
        deposit+" OR "+mail+" OR "+capacity+" OR "+unsent+")))"
        " AND (SELECT COALESCE(SUM(CAST(JSON_UNQUOTE(JSON_EXTRACT(o.before_state,'$.native.native.amount')) AS UNSIGNED)),0)"
        " FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id AND o.kind='guild_bank_deposit' AND o.state='verified')="+
        n(native.deposited)+" AND (SELECT COUNT(*) FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
        " AND o.kind='mail_collect' AND o.state='verified')="+n(j.incomingMail?1:0)+resources.guards;
    if(j.incomingMail)guard+=" AND NOT EXISTS(SELECT 1 FROM mail_items WHERE mail_id="+n(j.incomingMail)+')'+
        " AND EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id AND o.kind='mail_collect'"
        " AND o.state='verified' AND ("+number("guid")+'='+n(parcel.itemGuid)+
        " OR JSON_EXTRACT(o.after_state,'$.native.surviving_guid')="+n(parcel.itemGuid)+"))";
    auto stock=balances;bool present=false;for(const auto& b:stock)present|=b.itemGuid==parcel.itemGuid;
    if(!present)stock.push_back(parcel);
    for(const auto& b:stock) {
        if(b.location=="money")guard+=" AND EXISTS(SELECT 1 FROM characters WHERE guid="+n(saved.actor)+" AND money="+n(b.copper)+')';
        else guard+=" AND EXISTS(SELECT 1 FROM character_inventory v JOIN item_instance i ON i.guid=v.item WHERE v.guid="+
            n(saved.actor)+" AND i.owner_guid=v.guid AND i.guid="+n(b.itemGuid)+" AND i.itemEntry="+n(b.itemEntry)+" AND i.count="+n(b.quantity)+')'+
            " AND NOT EXISTS(SELECT 1 FROM mail_items WHERE item_guid="+n(b.itemGuid)+')'+
            " AND NOT EXISTS(SELECT 1 FROM guild_bank_item WHERE item_guid="+n(b.itemGuid)+')';
    }
    for(const auto& c:batch.claims)if(c.location=="bank")
        guard+=" AND EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id"
            " AND o.kind='bank_deposit' AND o.state='verified' AND "+number("guid")+'='+n(c.itemGuid)+
            " AND "+number("entry")+'='+n(c.itemEntry)+" AND "+number("quantity")+'='+n(c.quantity)+')';
    out.plan.statements.insert(out.plan.statements.begin(),
        "UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+n(saved.actor));
    AppendPersonalResourceSettlement(out.plan,resources,batch,now,receipt);
    // Archive the delivery, not its items, prior credits or the request itself.
    out.plan.statements.push_back("UPDATE guild_society_supply_delivery d SET d.phase='cancelled',d.blocker="+SqlValue(reason)+
        ",d.updated_at="+n(now/1000)+" WHERE "+GuildDeliveryIdentity(j,saved.actor)+" AND d.phase='carried' AND EXISTS("+out.plan.receiptQuery+')');
    out.plan.receiptQuery+=" AND EXISTS(SELECT 1 FROM guild_society_supply_delivery d WHERE "+GuildDeliveryIdentity(j,saved.actor)+
        " AND d.phase='cancelled' AND d.blocker="+SqlValue(reason)+" AND d.deposited_quantity="+n(native.deposited)+')';
    out.claims=std::move(resources.claims);why.clear();return true;
}
}
