#include "botpch.h"
#include "LivingNativeAuctionPurchase.h"
#include "LivingNativeMailCollection.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityNativeContext.h"
#include "LivingTaskItemRequirements.h"
#include "LivingNativeRecipeLearning.h"
#include "LivingProfessionDemand.h"
#include "LivingProfessionNative.h"
#include "LivingServiceExecution.h"
#include "LivingPurchaseBudget.h"
#include "Mails/Mail.h"
#include <algorithm>
#include <mutex>

namespace LivingActivity {
namespace {
static_assert(AUCTION_OUTBIDDED==0 && AUCTION_WON==1 && AUCTION_SUCCESSFUL==2 && AUCTION_SALE_PENDING==6,
    "Auction evidence must match the pinned native mail codes");
bool Safe(Player& actor) {
    return sLivingActivityCoordinator.OnWorldThread() && actor.GetPlayerbotAI() && actor.GetSession() &&
        actor.IsInWorld() && actor.IsAlive() && !actor.IsBeingTeleported() && actor.IsStopped() &&
        !actor.GetMap()->IsDungeon() && !actor.GetTradeData() &&
        !ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) && !LivingServiceExecution::Busy(&actor);
}
bool Demand(Player& actor,const Task& saved,uint32_t entry,uint32_t& remaining,std::string& why) {
    remaining=0;std::vector<ProfessionReagent> required;
    if(!ReadTaskItemRequirements(saved,required,why))return false;
    if(IsRecipeLearningTask(saved)) {
        if(!ValidateNativeRecipeLearningTask(actor,saved,why))return false;
    } else {
        ProfessionJob job;if(!DecodeProfessionJob(saved.checkpoint.data,job,why))return false;
        const auto native=InspectNativeProfessionRecipe(actor,job);job.initialSkill=native.skillValue;
        if(!MatchNativeProfessionRecipe(job,native,why))return false;
        if(job.purpose==ProfessionPurpose::SkillGain && native.skillValue>=job.targetSkill) {
            why="profession_target_met_requires_settlement";return false;
        }
    }
    NativeProfessionDemand have;
    if(!InspectNativeProfessionDemand(actor,saved,have)) {why=have.blocker;return false;}
    for(size_t i=0;i<required.size();++i) {
        if(have.stock[i].bank && have.stock[i].bag<required[i].perAttempt) {
            why="profession_banked_material_requires_collection";return false;
        }
        if(required[i].entry==entry && !RequiredProfessionVendorQuantity(required[i],have.stock[i],1,remaining,why))return false;
    }
    if(!remaining){why="profession_purchase_material_not_required";return false;}
    return true;
}
// Caller owns the existing market mutex; no pointer escapes this inspection.
bool InspectLocked(Player& actor,uint64_t auctioneer,uint32_t id,NativeAuctionQuote& q,std::string& why) {
    q={};auto reject=[&](const char* code){why=code;return false;};
    if(!Safe(actor))return reject("auction_actor_not_safely_available");
    auto* npc=actor.GetNPCIfCanInteractWith(ObjectGuid(auctioneer),UNIT_NPC_FLAG_AUCTIONEER);
    const auto* house=npc?actor.GetSession()->GetCheckedAuctionHouseForAuctioneer(ObjectGuid(auctioneer)):nullptr;
    auto* market=house?sAuctionMgr.GetAuctionsMap(house):nullptr;
    const auto* offer=market?market->GetAuction(id):nullptr;
    if(!npc || !house || !market)return reject("profession_auction_travel_required");
    if(!offer || !offer->owner || !offer->buyout || !offer->itemCount || offer->expireTime<=time(nullptr) ||
        offer->buyout<=offer->bid || offer->buyout<offer->startbid || offer->buyout>uint32_t(INT32_MAX) ||
        offer->owner==actor.GetGUIDLow() || offer->bidder==actor.GetGUIDLow())return reject("auction_listing_unavailable");
    const auto account=sObjectMgr.GetPlayerAccountIdByGUID(ObjectGuid(HIGHGUID_PLAYER,offer->owner));
    if(!account || account==actor.GetSession()->GetAccountId())return reject("auction_same_account_or_missing_seller");
    if(offer->bidder && !sObjectMgr.GetPlayerAccountIdByGUID(ObjectGuid(HIGHGUID_PLAYER,offer->bidder)))
        return reject("auction_refund_recipient_unavailable");
    const auto* item=sAuctionMgr.GetAItem(offer->itemGuidLow);
    if(!item || item->GetEntry()!=offer->itemTemplate || item->GetCount()!=offer->itemCount ||
        item->GetOwnerGuid()!=ObjectGuid(HIGHGUID_PLAYER,offer->owner))return reject("auction_native_stack_changed");
    auto priced=*offer;priced.bid=offer->buyout;
    const auto cut=priced.GetAuctionCut();const uint64_t gross=uint64_t(offer->buyout)+offer->deposit;
    if(cut>gross || gross-cut>UINT32_MAX)return reject("auction_native_proceeds_out_of_range");
    if(actor.GetMoney()<offer->buyout)return reject("auction_native_money_shortfall");
    // Faction markets share listings across cities. MailSender(AuctionEntry*)
    // uses the listing's house, not the auctioneer visited by this buyer.
    q.actor=actor.GetGUIDLow();q.seller=offer->owner;q.house=offer->GetHouseId();q.auction=id;
    q.guid=item->GetGUIDLow();q.entry=item->GetEntry();q.quantity=item->GetCount();q.copper=offer->buyout;
    q.moneyBefore=actor.GetMoney();q.bidder=offer->bidder;q.bid=offer->bid;q.proceeds=uint32_t(gross-cut);
    q.auctioneerEntry=npc->GetEntry();q.property=offer->itemRandomPropertyId;q.auctioneer=auctioneer;q.expiresAt=offer->expireTime;
    NativeAuctionQuote check;
    if(!DecodeNativeAuctionQuote(EncodeNativeAuctionQuote(q),check))return reject("auction_quote_invalid");
    why.clear();return true;
}
}
bool InspectNativeAuctionQuote(Player& actor,uint64_t npc,uint32_t id,NativeAuctionQuote& q,std::string& why) {
    std::unique_lock<std::mutex> lock(sRandomPlayerbotMgr.m_ahActionMutex,std::try_to_lock);
    if(!lock.owns_lock()){why="profession_purchase_market_snapshot_busy";return false;}
    return InspectLocked(actor,npc,id,q,why);
}
bool NativeAuctionSourceAvailable(Player& actor,uint32_t entry,uint32_t maximum,std::string& why) {
    std::vector<NativeAuctionOffer> offers;return NativeAuctionOffers(actor,entry,maximum,offers,why) && !offers.empty();
}
bool PlanNativeAuctionPurchase(Player& actor,const Task& saved,const ProfessionReagent& wanted,NativeAuctionQuote& q,std::string& why) {
    q={};uint32_t remaining=0;
    if(!Demand(actor,saved,wanted.entry,remaining,why))return false;
    if(!Safe(actor)){why="auction_actor_not_safely_available";return false;}
    bool nearby=false;
    for(const auto npc:actor.GetPlayerbotAI()->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("nearest npcs no los")->Get()) {
        if(!actor.GetNPCIfCanInteractWith(npc,UNIT_NPC_FLAG_AUCTIONEER))continue;
        nearby=true;std::vector<NativeAuctionOffer> offers;
        if(!NativeAuctionOffers(actor,wanted.entry,remaining,offers,why,npc.GetRawValue()))return false;
        for(const auto& offer:offers) {
            NativeAuctionQuote candidate;
            if(InspectNativeAuctionQuote(actor,npc.GetRawValue(),offer.id,candidate,why)) {q=candidate;why.clear();return true;}
        }
    }
    if(nearby){if(why.empty())why="profession_auction_source_unavailable";return false;}
    if(!NativeAuctionSourceAvailable(actor,wanted.entry,remaining,why))return false;
    why="profession_auction_travel_required";return false;
}
bool NativeAuctionMoneyReservation::ValidatePurpose(Player& actor,const ReservationRequest& r,std::string& why) {
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(r.transition.task.id);
    if(!saved || saved->actor!=actor.GetGUIDLow() || saved->revision!=r.transition.expectedRevision ||
        saved->checkpoint.data!=r.transition.task.checkpoint.data || r.changes.size()!=1) {
        why="auction_reservation_task_changed";return false;
    }
    const auto& change=r.changes.front();const auto& c=change.after;
    if(c.actor!=saved->actor || c.task!=saved->root || c.location!="money" || !c.copper || c.itemGuid ||
        c.itemEntry || c.quantity || c.nativeReference || c.revision!=change.expectedRevision+1) {
        why="auction_money_claim_invalid";return false;
    }
    if(change.expectedRevision && c.state=="released") {
        UnsettledClaimBatch batch;
        if(!sLivingActivityCoordinator.ReadTaskClaims(saved->actor,saved->id,saved->revision,batch,why))return false;
        for(auto held:batch.claims) if(held.id==c.id && held.state=="held") {
            ++held.revision;held.state="released";
            if(SameResourceClaim(held,c)){why.clear();return true;}
        }
        why="auction_release_not_current";return false;
    }
    NativeAuctionQuote current;uint32_t remaining=0;
    if(change.expectedRevision || c.state!="held" || c.copper!=quote.copper) {why="auction_reservation_quote_mismatch";return false;}
    if(!Demand(actor,*saved,quote.entry,remaining,why) || remaining<quote.quantity ||
        !InspectNativeAuctionQuote(actor,quote.auctioneer,quote.auction,current,why))return false;
    if(EncodeNativeAuctionQuote(current)!=EncodeNativeAuctionQuote(quote)){why="auction_quote_changed";return false;}
    return ValidateNativeAuctionBudget(actor,*saved,operation,quote.copper,quote.seller,why);
}
bool NativeAuctionPurchase::PrepareDispatch(Player& actor,const OperationRequest& r,std::string& why) {
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(r.transition.task.id);PurchaseSpend spend;
    if(!saved){why="purchase_budget_task_changed";return false;}
    return sLivingActivityCoordinator.ReadPurchaseBudget(actor.GetGUIDLow(),saved->id,saved->revision,r.transition.receipt,spend,why,quote.seller);
}
bool NativeAuctionPurchase::ValidateNative(Player& actor,const OperationRequest& r,std::string& why) {
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(r.transition.task.id);
    if(!saved || saved->actor!=actor.GetGUIDLow() || saved->checkpoint.data!=r.transition.task.checkpoint.data ||
        (saved->revision!=r.transition.expectedRevision && saved->revision!=r.transition.task.revision) ||
        r.beforeState!=EncodeNativeAuctionQuote(quote) || MailGainSpecJson(r.mailGain)!=MailGainSpecJson(quote.Stack()) ||
        r.consumption.size()!=1 || r.consumption.front().before.location!="money" || r.consumption.front().used!=quote.copper) {
        why="auction_exact_claimed_quote_required";return false;
    }
    uint32_t remaining=0;
    if(!Demand(actor,*saved,quote.entry,remaining,why))return false;
    if(quote.quantity>remaining){why="profession_purchase_unpaid_quantity_changed";return false;}
    NativeAuctionQuote current;
    if(!InspectNativeAuctionQuote(actor,quote.auctioneer,quote.auction,current,why))return false;
    if(EncodeNativeAuctionQuote(current)!=EncodeNativeAuctionQuote(quote)){why="auction_quote_changed";return false;}
    return ValidateNativeAuctionBudget(actor,*saved,r.transition.receipt,quote.copper,quote.seller,why);
}
NativeObservation NativeAuctionPurchase::ExecuteNative(Player& actor,const OperationRequest& r) {
    NativeObservation out;std::string why;captured.clear();
    if(!ValidateNative(actor,r,why)){out.state=OperationState::Rejected;out.evidence=why;return out;}
    std::unique_lock<std::mutex> lock(sRandomPlayerbotMgr.m_ahActionMutex,std::try_to_lock);
    if(!lock.owns_lock()){out.state=OperationState::Rejected;out.evidence="profession_purchase_market_snapshot_busy";return out;}
    NativeAuctionQuote current;
    if(!InspectLocked(actor,quote.auctioneer,quote.auction,current,why) || EncodeNativeAuctionQuote(current)!=EncodeNativeAuctionQuote(quote)) {
        out.state=OperationState::Rejected;out.evidence=why.empty()?"auction_quote_changed":why;return out;
    }
    // One synchronous native call inside the coordinator's retained transaction.
    // Never retain AuctionEntry*: successful buyout deletes it in the handler.
    AuctionMailCapture mail;PurchaseMutation money(actor.GetGUIDLow());
    WorldPacket packet;packet<<ObjectGuid(quote.auctioneer)<<quote.auction<<quote.copper;
    actor.GetSession()->HandleAuctionPlaceBid(packet);captured=mail.Rows();
    out.nativeReference="auction:"+std::to_string(quote.auction)+":item:"+std::to_string(quote.guid);
    out.afterState="{\"money_before\":"+std::to_string(quote.moneyBefore)+",\"money_after\":"+std::to_string(actor.GetMoney())+
        ",\"mails\":"+AuctionMailsJson(captured)+'}';
    if(actor.GetMoney()==quote.moneyBefore && captured.empty()) {
        out.state=OperationState::Rejected;out.evidence="native_auction_rejected_without_effect";return out;
    }
    if(!mail.Valid() || actor.GetMoney()!=quote.moneyBefore-quote.copper ||
        !VerifyAuctionMails(quote,captured,out.mailedItem,why)) {
        out.evidence=why.empty()?"auction_native_payment_requires_reconciliation":why;return out;
    }
    const auto* house=actor.GetSession()->GetCheckedAuctionHouseForAuctioneer(ObjectGuid(quote.auctioneer));
    if(!house || sAuctionMgr.GetAuctionsMap(house)->GetAuction(quote.auction) || sAuctionMgr.GetAItem(quote.guid)) {
        out.evidence="auction_native_listing_not_removed";return out;
    }
    ResourceClaim claimed=MailGainClaim(r.transition.task,r.transition.receipt,r.mailGain,out.mailedItem).after;
    NativeResourceBalance actual;
    if(!ReadNativeMailBalance(actor,claimed,actual) || !VerifyNativeMailGain(actor.GetGUIDLow(),r.mailGain,actual,why)) {
        out.evidence="auction_native_attachment_not_owned";return out;
    }
    out.state=OperationState::Verified;out.evidence="native_auction_payment_and_mail_observed";return out;
}
std::string NativeAuctionPurchase::PersistedNativeProof(Player& actor,const OperationRequest&,const Task& task) const {
    std::string proof="SELECT "+SqlValue(task.id)+','+std::to_string(task.revision)+" FROM characters WHERE guid="+
        std::to_string(actor.GetGUIDLow())+" AND money="+std::to_string(actor.GetMoney());
    if(captured.empty())return proof; // Rejection is not a purchase receipt.
    proof+=" AND NOT EXISTS (SELECT 1 FROM auction WHERE id="+std::to_string(quote.auction)+')';
    for(const auto& m:captured) {
        proof+=" AND EXISTS (SELECT 1 FROM mail m WHERE m.id="+std::to_string(m.id)+" AND m.messageType="+
            std::to_string(MAIL_AUCTION)+" AND m.sender="+std::to_string(m.sender)+" AND m.receiver="+std::to_string(m.receiver)+
            " AND m.money="+std::to_string(m.money)+" AND m.cod=0 AND m.subject="+SqlValue(m.subject)+
            " AND m.deliver_time="+std::to_string(m.deliveredAt)+" AND m.expire_time="+std::to_string(m.expiresAt)+')';
        if(m.itemGuid)proof+=" AND EXISTS (SELECT 1 FROM mail_items mi JOIN item_instance i ON i.guid=mi.item_guid WHERE mi.mail_id="+
            std::to_string(m.id)+" AND mi.item_guid="+std::to_string(m.itemGuid)+" AND mi.receiver="+std::to_string(m.receiver)+
            " AND mi.item_template="+std::to_string(m.itemEntry)+" AND i.itemEntry="+std::to_string(m.itemEntry)+
            " AND i.owner_guid="+std::to_string(m.receiver)+" AND i.count="+std::to_string(m.quantity)+')'+
            " AND NOT EXISTS (SELECT 1 FROM character_inventory WHERE item="+std::to_string(m.itemGuid)+')';
    }
    return proof;
}
}
