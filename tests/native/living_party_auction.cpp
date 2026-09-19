#include "LivingPartyAuction.h"
#include "LivingAuctionPostRecovery.h"
#include "LivingActivityRequests.h"
#include <cassert>
using namespace LivingActivity;
int main() {
    PartyAuctionJob job{{{100,2589,5,500,720},{200,7073,1,800,1440}},0},decoded;
    const auto encoded=EncodePartyAuctionJob(job);
    assert(DecodePartyAuctionJob(encoded,decoded) && decoded.next==0 && decoded.items.size()==2);
    for(const auto bad:{"{}","[]","null","","{\"workflow\":\"party_auction_v2\"}"})assert(!DecodePartyAuctionJob(bad,decoded));
    assert(!DecodePartyAuctionJob(encoded+"{}",decoded));assert(!DecodePartyAuctionJob(" "+encoded,decoded));
    auto extra=encoded;extra.insert(extra.size()-1,",\"next\":0");assert(!DecodePartyAuctionJob(extra,decoded));
    auto changed=job;changed.next=3;assert(!ValidPartyAuctionJob(changed));
    changed=job;changed.items[1].guid=100;assert(!ValidPartyAuctionJob(changed));
    changed=job;changed.items[0].quantity=0;assert(!ValidPartyAuctionJob(changed));
    changed=job;changed.items[0].minutes=719;assert(!ValidPartyAuctionJob(changed));
    changed=job;changed.items[0].buyout=1;assert(!ValidPartyAuctionJob(changed));
    changed=job;changed.items[0].buyout=UINT32_MAX;assert(!ValidPartyAuctionJob(changed));
    changed={};for(uint32_t i=1;i<=20;++i)changed.items.push_back({i,2589,1,100,720});
    assert(DecodePartyAuctionJob(EncodePartyAuctionJob(changed),decoded));
    changed.items.push_back({21,2589,1,100,720});assert(!ValidPartyAuctionJob(changed));
    Task t;t.id=t.root="11111111-1111-4111-8111-111111111111";t.actor=7;t.source="party_auction";
    t.kind=Kind::PartyErrand;t.mode=Mode::Active;t.accepted=true;t.phase=Phase::Preparing;t.revision=4;
    t.checkpoint.step="party_auction_prepare";t.checkpoint.data=encoded;
    std::string why;assert(ValidatePartyAuctionTask(t,why));
    auto bad=t;bad.phase=Phase::Completed;assert(!ValidatePartyAuctionTask(bad,why));
    bad=t;bad.parent=t.id;assert(!ValidatePartyAuctionTask(bad,why));
    bad=t;bad.source="profession";assert(!IsPartyAuctionTask(bad));
    bad=t;bad.checkpoint.data=EncodePartyAuctionJob(changed);assert(!PreservePartyAuctionIntent(t,bad,why));
    AuctionPostQuote q;q.item=job.items[0];q.actor=7;q.house=1;q.auctioneerEntry=1000;q.auctioneer=1234;
    q.money=10000;q.deposit=15;q.bid=475;q.from=23;q.property=-12;
    AuctionPostQuote copy;assert(DecodeAuctionPostQuote(EncodeAuctionPostQuote(q),copy));
    assert(PartyAuctionQuoteMatches(t,q));auto wrong=q;wrong.item.guid=200;assert(!PartyAuctionQuoteMatches(t,wrong));
    wrong=q;wrong.bid=0;assert(!ValidAuctionPostQuote(wrong));
    wrong=q;wrong.deposit=10001;assert(!ValidAuctionPostQuote(wrong));
    wrong=q;wrong.item.buyout=INT32_MAX;wrong.bid=uint64_t(INT32_MAX)*95/100;assert(ValidAuctionPostQuote(wrong));
    ResourceClaim item;item.id="22222222-2222-4222-8222-222222222222";item.task=t.id;item.actor=7;
    item.itemGuid=100;item.itemEntry=2589;item.quantity=5;item.location="bags";item.state="held";item.revision=1;
    ResourceClaim money;money.id="33333333-3333-4333-8333-333333333333";money.task=t.id;money.actor=7;
    money.copper=15;money.location="money";money.state="held";money.revision=1;
    std::vector<ClaimConsumption> uses{{item,5},{money,15}};
    assert(ExactAuctionPostConsumption(t.root,q,uses));
    auto badUses=uses;badUses[0].before.itemGuid=200;assert(!ExactAuctionPostConsumption(t.root,q,badUses));
    badUses=uses;badUses[1].used=14;assert(!ExactAuctionPostConsumption(t.root,q,badUses));
    badUses=uses;badUses[0].before.quantity=6;assert(!ExactAuctionPostConsumption(t.root,q,badUses));
    badUses=uses;badUses[0].before.task=money.id;assert(!ExactAuctionPostConsumption(t.root,q,badUses));
    wrong=q;wrong.deposit=0;assert(ExactAuctionPostConsumption(t.root,wrong,{{item,5}}));
    assert(!ExactAuctionPostConsumption(t.root,wrong,uses));
    AuctionPostReceipt r;r.auction=90;r.actor=7;r.house=1;r.guid=100;r.entry=2589;r.quantity=5;
    r.deposit=15;r.bid=475;r.buyout=500;r.money=9985;r.property=-12;r.expires=99999;r.escrowPresent=true;
    assert(VerifyAuctionPost(q,r));AuctionPostReceipt parsed;
    assert(DecodeAuctionPostReceipt(EncodeAuctionPostReceipt(r),parsed) && VerifyAuctionPost(q,parsed));
    auto receipt=r;receipt.auction=0;assert(!VerifyAuctionPost(q,receipt));
    receipt=r;receipt.actor=8;assert(!VerifyAuctionPost(q,receipt));
    receipt=r;receipt.house=2;assert(!VerifyAuctionPost(q,receipt));
    receipt=r;receipt.guid=200;assert(!VerifyAuctionPost(q,receipt));
    receipt=r;receipt.quantity=4;assert(!VerifyAuctionPost(q,receipt));
    receipt=r;receipt.property=12;assert(!VerifyAuctionPost(q,receipt));
    receipt=r;receipt.money=10000;assert(!VerifyAuctionPost(q,receipt));
    receipt=r;receipt.deposit=14;assert(!VerifyAuctionPost(q,receipt));
    receipt=r;receipt.bagPresent=true;assert(!VerifyAuctionPost(q,receipt));
    receipt=r;receipt.escrowPresent=false;assert(!VerifyAuctionPost(q,receipt));
    // An absent bag item, a timer or an unrelated increase in AH totals is not proof.
    assert(!VerifyAuctionPost(q,AuctionPostReceipt{}));
    OperationResult proof;proof.id=money.id;proof.task=t.id;proof.taskRevision=4;proof.state=OperationState::Verified;
    proof.kind="party_auction_post";proof.evidence="native_auction_post_escrow_and_deposit_observed";
    proof.nativeReference=AuctionPostReference(r);
    auto after=t;after.phase=Phase::Verifying;after.revision=5;
    assert(AcknowledgePartyAuctionPost(after,q,uses,proof,r));
    assert(after.phase==Phase::Verifying && DecodePartyAuctionJob(after.checkpoint.data,decoded) && decoded.next==1);
    assert(!AcknowledgePartyAuctionPost(after,q,uses,proof,r)); // Cannot acknowledge the same listing twice.
    after=t;after.phase=Phase::Verifying;after.revision=5;auto rejected=proof;rejected.state=OperationState::Rejected;
    assert(!AcknowledgePartyAuctionPost(after,q,uses,rejected,r) && after.checkpoint.data==encoded);
    rejected=proof;rejected.state=OperationState::Reconciling;assert(!AcknowledgePartyAuctionPost(after,q,uses,rejected,r));
    rejected=proof;rejected.taskRevision=3;assert(!AcknowledgePartyAuctionPost(after,q,uses,rejected,r));
    rejected=proof;rejected.nativeReference="auction_post:91:item:100";assert(!AcknowledgePartyAuctionPost(after,q,uses,rejected,r));
    // The final verified listing completes the finite root, not future inventory.
    job.items.resize(1);after.checkpoint.data=EncodePartyAuctionJob(job);
    assert(AcknowledgePartyAuctionPost(after,q,uses,proof,r) && after.phase==Phase::Completed);
    assert(ValidatePartyAuctionTask(after,why));
    assert(!AcknowledgePartyAuctionPost(after,q,uses,proof,r));
    // An interrupted durable intent can be rejected as uncommitted only from
    // unchanged native stock plus exact journal/claims, never by reposting.
    auto interrupted=t;interrupted.phase=Phase::Executing;interrupted.checkpoint.step="party_auction_post";
    interrupted.sourceKey="fixture:auction";interrupted.createdAtMs=1;interrupted.updatedAtMs=1000;
    interrupted.context={};interrupted.context.actor=7;
    WorldContext current;current.actor=7;current.boot=proof.id;current.actorGeneration=1;
    current.mapGeneration=1;current.policyRevision=1;
    StoredCraftOperation operation;operation.acknowledged=true;operation.receipt=proof;
    operation.receipt.state=OperationState::Intent;operation.receipt.evidence.clear();operation.receipt.nativeReference.clear();
    operation.afterState="{}";
    operation.beforeState="{\"effects\":"+std::to_string(Mask(Effect::Inventory)|Mask(Effect::Money))+
        ",\"persistence\":1,\"native\":"+ClaimedNativeState(EncodeAuctionPostQuote(q),uses)+'}';
    AuctionPostRecoverySnapshot native;native.unchanged=q;native.escrowAbsent=true;
    UnsettledClaimBatch claims;claims.complete=true;claims.bookRevision=1;claims.claims={item,money};
    AuctionPostRecovery recovery;
    const auto recover=[&](const Task& saved,const StoredCraftOperation& op,const AuctionPostRecoverySnapshot& observed,
                          const UnsettledClaimBatch& held,uint32_t unresolved=1) {
        return PrepareInterruptedAuctionPost(saved,current,op,unresolved,held,observed,2000,
            "44444444-4444-4444-8444-444444444444",recovery,why);
    };
    assert(recover(interrupted,operation,native,claims));
    assert(recovery.task.phase==Phase::Verifying && recovery.task.revision==5);
    assert(recovery.task.checkpoint.data==interrupted.checkpoint.data); // Cursor did NOT advance.
    assert(recovery.plan.statements.size()>=3);
    std::string sql;for(const auto& statement:recovery.plan.statements)sql+=statement;
    for(const auto* guard:{"native_auction_post_intent_not_committed","o.state='intent'","o.after_state='{}'",
                          "FROM auction WHERE itemguid=100","FROM mail_items WHERE item_guid=100",
                          "FROM guild_bank_item WHERE item_guid=100","FROM organic_economy_auction_history",
                          "i.count=5","money=10000","c.revision=1"})assert(sql.find(guard)!=std::string::npos);
    assert(sql.find("UPDATE living_activity_claim")==std::string::npos);
    assert(sql.find("UPDATE characters")==std::string::npos);
    assert(sql.find("UPDATE item_instance")==std::string::npos);
    auto observed=native;observed.unchanged.money-=q.deposit;assert(!recover(interrupted,operation,observed,claims));
    observed=native;--observed.unchanged.item.quantity;assert(!recover(interrupted,operation,observed,claims));
    observed=native;++observed.unchanged.from;assert(!recover(interrupted,operation,observed,claims));
    observed=native;observed.escrowAbsent=false;assert(!recover(interrupted,operation,observed,claims));
    auto altered=operation;altered.afterState="{\"partial\":true}";assert(!recover(interrupted,altered,native,claims));
    altered=operation;altered.receipt.state=OperationState::Reconciling;assert(!recover(interrupted,altered,native,claims));
    altered=operation;altered.receipt.taskRevision=3;assert(!recover(interrupted,altered,native,claims));
    altered=operation;altered.beforeState+=" ";assert(!recover(interrupted,altered,native,claims));
    auto held=claims;++held.claims[0].revision;assert(!recover(interrupted,operation,native,held));
    held=claims;held.claims.pop_back();assert(!recover(interrupted,operation,native,held));
    auto sameBoot=interrupted;sameBoot.context=current;assert(!recover(sameBoot,operation,native,claims));
    assert(!recover(interrupted,operation,native,claims,2));
}
