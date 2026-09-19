#pragma once
#include "LivingPartyAuction.h"
#include "LivingActivityEffects.h"
#include "LivingActivityResourceView.h"
#include "LivingProfessionEvidence.h"

namespace LivingActivity {
struct AuctionPostRecoverySnapshot {
    AuctionPostQuote unchanged;
    uint32_t bagGuid=0;
    bool escrowAbsent=false;
};
struct AuctionPostRecovery {Task task;WritePlan plan;};
inline std::string AuctionPostIntentPrefix() {
    return "{\"effects\":"+std::to_string(Mask(Effect::Inventory)|Mask(Effect::Money))+
        ",\"persistence\":1,\"native\":{\"native\":";
}
inline bool DecodeInterruptedAuctionPost(const Task& task,const StoredCraftOperation& row,
    AuctionPostQuote& quote,std::string& why) {
    quote={};auto reject=[&](const char* value){why=value;return false;};
    if(!IsPartyAuctionTask(task) || !ValidatePartyAuctionTask(task,why) || !task.accepted || task.mode!=Mode::Active ||
        task.phase!=Phase::Executing || task.checkpoint.step!="party_auction_post" ||
        !row.acknowledged || !IsUuid(row.receipt.id) || row.receipt.task!=task.id ||
        row.receipt.taskRevision!=task.revision || row.receipt.kind!="party_auction_post" ||
        row.receipt.state!=OperationState::Intent || !row.receipt.nativeReference.empty() ||
        !row.receipt.evidence.empty() || row.afterState!="{}" || row.beforeState.size()>8192)
        return reject("auction_recovery_exact_intent_required");
    const auto prefix=AuctionPostIntentPrefix();
    const auto end=row.beforeState.find(",\"claimed_consumption\":",prefix.size());
    if(row.beforeState.compare(0,prefix.size(),prefix) || end==std::string::npos ||
        !DecodeAuctionPostQuote(row.beforeState.substr(prefix.size(),end-prefix.size()),quote) ||
        !PartyAuctionQuoteMatches(task,quote))return reject("auction_recovery_quote_invalid");
    why.clear();return true;
}
inline bool PrepareInterruptedAuctionPost(const Task& saved,const WorldContext& current,
    const StoredCraftOperation& row,uint32_t unresolved,const UnsettledClaimBatch& batch,
    const AuctionPostRecoverySnapshot& native,uint64_t now,const std::string& receipt,
    AuctionPostRecovery& result,std::string& why) {
    result={};auto reject=[&](const char* value){why=value;return false;};
    AuctionPostQuote quote;if(!DecodeInterruptedAuctionPost(saved,row,quote,why))return false;
    if(!saved.context.boot.empty() || saved.context.actorGeneration || saved.context.mapGeneration ||
        current.actor!=saved.actor || !IsUuid(current.boot) || !current.actorGeneration ||
        !current.mapGeneration || !current.policyRevision || current.session.size()>120 ||
        (current.session.empty()!=(current.sessionRevision==0)) || !IsUuid(receipt) ||
        now<saved.updatedAtMs || saved.revision>=UINT64_MAX-1 || unresolved!=1)
        return reject("auction_recovery_restored_context_required");
    if(!batch.complete || !batch.bookRevision || batch.claims.empty() || batch.claims.size()>2)
        return reject("auction_recovery_claims_incomplete");
    std::vector<ClaimConsumption> uses;
    for(const auto& c:batch.claims)uses.push_back({c,uint32_t(c.copper?c.copper:c.quantity)});
    if(!ExactAuctionPostConsumption(saved.id,quote,uses))return reject("auction_recovery_claims_changed");
    const auto exact="{\"effects\":"+std::to_string(Mask(Effect::Inventory)|Mask(Effect::Money))+
        ",\"persistence\":1,\"native\":"+ClaimedNativeState(EncodeAuctionPostQuote(quote),uses)+'}';
    if(row.beforeState!=exact)return reject("auction_recovery_claims_changed");
    if(!native.escrowAbsent || EncodeAuctionPostQuote(native.unchanged)!=EncodeAuctionPostQuote(quote))
        return reject("auction_recovery_native_before_changed");
    auto n=[](uint64_t value){return std::to_string(value);};
    AuctionPostRecovery prepared;prepared.task=saved;auto& next=prepared.task;
    next.context=current;++next.revision;next.phase=Phase::Verifying;next.updatedAtMs=now;
    next.checkpoint.blocker.clear();next.retryAtMs=0;
    auto outcome=row.receipt;outcome.state=OperationState::Rejected;
    outcome.evidence="native_auction_post_intent_not_committed";
    outcome.nativeReference="auction_uncommitted:item:"+n(quote.item.guid);
    prepared.plan=OperationOutcomeWrite(next,saved.revision,outcome,receipt,
        "{\"recovery\":{\"version\":1,\"basis\":\"atomic_native_save_absent\",\"boot\":\""+
        current.boot+"\"},\"unchanged\":"+EncodeAuctionPostQuote(quote)+'}');
    auto& guard=prepared.plan.statements.front();
    guard+=" AND phase='executing' AND accepted=1 AND checkpoint="+SqlValue(saved.checkpoint.data)+
        " AND EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.operation_id="+SqlValue(outcome.id)+
        " AND o.state='intent' AND o.kind='party_auction_post' AND o.before_state="+SqlValue(exact)+
        " AND o.after_state='{}' AND o.native_reference='' AND o.evidence_code='')"+
        " AND (SELECT COUNT(*) FROM living_activity_operation o JOIN living_activity_task owner ON owner.task_id=o.task_id"
        " WHERE owner.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))=1"+
        " AND EXISTS(SELECT 1 FROM characters WHERE guid="+n(quote.actor)+" AND money="+n(quote.money)+')'+
        " AND EXISTS(SELECT 1 FROM character_inventory v JOIN item_instance i ON i.guid=v.item WHERE v.guid="+
        n(quote.actor)+" AND v.item="+n(quote.item.guid)+" AND v.item_template="+n(quote.item.entry)+
        " AND v.bag="+n(native.bagGuid)+" AND v.slot="+n(uint8_t(quote.from))+" AND i.owner_guid="+n(quote.actor)+
        " AND i.itemEntry="+n(quote.item.entry)+" AND i.count="+n(quote.item.quantity)+')'+
        " AND NOT EXISTS(SELECT 1 FROM auction WHERE itemguid="+n(quote.item.guid)+')'+
        " AND NOT EXISTS(SELECT 1 FROM mail_items WHERE item_guid="+n(quote.item.guid)+')'+
        " AND NOT EXISTS(SELECT 1 FROM guild_bank_item WHERE item_guid="+n(quote.item.guid)+')'+
        " AND NOT EXISTS(SELECT 1 FROM organic_economy_auction_history WHERE item_guid="+n(quote.item.guid)+
        " AND occurred_at>=FROM_UNIXTIME("+n(saved.updatedAtMs/1000)+"))"+
        " AND (SELECT COUNT(*) FROM living_activity_claim WHERE task_id=living_activity_task.task_id"
        " AND state NOT IN ('released','consumed'))="+n(batch.claims.size());
    for(const auto& c:batch.claims)
        guard+=" AND EXISTS(SELECT 1 FROM living_activity_claim c WHERE "+ConsumptionClaimPredicate(c)+')';
    // Lock the actor's tasks before inspecting its unresolved-operation count.
    prepared.plan.statements.insert(prepared.plan.statements.begin(),
        "UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+n(saved.actor));
    result=std::move(prepared);why.clear();return true;
}
}
