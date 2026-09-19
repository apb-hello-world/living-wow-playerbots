#include "botpch.h"
#include "LivingNativeAuctionPost.h"
#include "LivingAuctionPostRecovery.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityNativeContext.h"
#include "LivingNativeVendorSale.h"
#include "LivingNativeMailCollection.h"
#include "LivingServiceExecution.h"
#include "LivingPurchaseBudget.h"
#include "strategy/actions/AhAction.h"
#include <mutex>

namespace LivingActivity {
bool ReadUnpostedNativeAuction(Player& p,const AuctionPostQuote& quote,AuctionPostRecoverySnapshot& out,std::string& why) {
    out={};auto reject=[&](const char* value){why=value;return false;};
    if(!sLivingActivityCoordinator.OnWorldThread() || p.GetGUIDLow()!=quote.actor || !p.IsInWorld())
        return reject("auction_recovery_actor_unavailable");
    const auto* item=p.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,quote.item.guid));
    if(!item || item->GetOwnerGuid()!=p.GetObjectGuid() || !Player::IsInventoryPos(item->GetPos()) ||
        sAuctionMgr.GetAItem(quote.item.guid))return reject("auction_recovery_original_stack_unavailable");
    out.unchanged=quote;auto& native=out.unchanged;
    native.money=p.GetMoney();native.from=item->GetPos();native.property=item->GetItemRandomPropertyId();
    native.item.guid=item->GetGUIDLow();native.item.entry=item->GetEntry();native.item.quantity=item->GetCount();
    out.bagGuid=item->GetContainer()?item->GetContainer()->GetGUIDLow():0;
    out.escrowAbsent=true;why.clear();return true;
}
namespace {
bool Safe(Player& p) {
    return sLivingActivityCoordinator.OnWorldThread() && p.GetPlayerbotAI() && p.GetSession() && p.IsInWorld() &&
        p.IsAlive() && !p.IsBeingTeleported() && !p.GetMap()->IsDungeon() && !p.GetTradeData() &&
        !ReadNativeSafety(p,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) && !LivingServiceExecution::Busy(&p);
}
bool Inspect(Player& p,const Task& t,const AuctionPostItem& expected,AuctionPostQuote& q,std::string& why) {
    q={};auto reject=[&](const char* reason){why=reason;return false;};
    if(!Safe(p) || p.GetGUIDLow()!=t.actor)return reject("party_auction_safety_pause");
    uint32_t capacity=0,unit=0,minutes=0;
    if(!ai::AhAction::PostingCapacity(p,capacity,why))return false;
    auto* item=p.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,expected.guid));
    if(!item || item->GetEntry()!=expected.entry || item->GetCount()!=expected.quantity ||
        NativeCapacityItemProtected(p,t,*item))return reject("party_auction_accepted_stack_changed_or_protected");
    if(!ai::AhAction::PostingItem(p,*item,unit,minutes,why))return false;
    if(minutes!=expected.minutes)return reject("party_auction_duration_changed");
    if(!p.IsStopped())return reject("party_auction_travel_required");
    for(const auto guid:p.GetPlayerbotAI()->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("nearest npcs no los")->Get()) {
        auto* npc=p.GetNPCIfCanInteractWith(guid,UNIT_NPC_FLAG_AUCTIONEER);
        const auto* house=npc?p.GetSession()->GetCheckedAuctionHouseForAuctioneer(guid):nullptr;
        if(!house)continue;
        q.item=expected;q.actor=t.actor;q.house=house->houseId;q.auctioneerEntry=npc->GetEntry();q.auctioneer=guid.GetRawValue();
        q.money=p.GetMoney();q.deposit=AuctionHouseMgr::GetAuctionDeposit(house,expected.minutes*MINUTE,item);
        q.bid=uint64_t(expected.buyout)*95/100;q.from=item->GetPos();q.property=item->GetItemRandomPropertyId();
        if(q.deposit>ai::AhAction::PostingMoney(p))return reject("party_auction_discretionary_deposit_shortfall");
        if(!ValidAuctionPostQuote(q))return reject("party_auction_invalid_native_quote");
        why.clear();return true;
    }
    return reject("party_auction_travel_required");
}
}
bool PlanNativePartyAuctionBatch(Player& p,PartyAuctionJob& job,std::string& why) {
    job={};if(!Safe(p)){why="party_auction_safety_pause";return false;}
    uint32_t capacity=0;if(!ai::AhAction::PostingCapacity(p,capacity,why))return false;
    const auto view=sLivingActivityCoordinator.ResourceReservations().Inspect();
    if(!view || !view->ready){why="party_auction_reservations_unavailable";return false;}
    Task t;t.source="party_auction";t.kind=Kind::PartyErrand;t.actor=p.GetGUIDLow();
    auto items=p.GetPlayerbotAI()->InventoryParseItems("inventory",IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
    items.sort([](const Item* a,const Item* b){return a->GetGUIDLow()<b->GetGUIDLow();});
    for(auto* item:items) {
        if(!item || NativeCapacityItemProtected(p,t,*item) || view->ProtectedItem(item->GetGUIDLow()) ||
            view->HasUncertainItem(t.actor,item->GetEntry()))continue;
        uint32_t unit=0,minutes=0;if(!ai::AhAction::PostingItem(p,*item,unit,minutes,why))continue;
        // Same 75-100 percent range as the organic seller, frozen once per
        // accepted stack. Retries never change a listing's price or duration.
        const uint32_t percent=75+(uint64_t(t.actor)*17+item->GetGUIDLow())%26;
        const uint64_t price=std::max<uint64_t>(1,uint64_t(unit)*percent/100)*item->GetCount();
        if(price<2 || price>uint32_t(INT32_MAX))continue; // Native startbid must be nonzero.
        job.items.push_back({item->GetGUIDLow(),item->GetEntry(),item->GetCount(),uint32_t(price),minutes});
        if(job.items.size()>=std::min<size_t>(capacity,PartyAuctionBatchLimit))break;
    }
    if(!ValidPartyAuctionJob(job)){why="party_auction_no_eligible_owned_stack";return false;}
    why.clear();return true;
}
bool PlanNativePartyAuctionPost(Player& p,const Task& t,AuctionPostQuote& q,
    std::vector<ClaimConsumption>& uses,std::string& why) {
    q={};uses.clear();PartyAuctionJob job;
    if(!IsPartyAuctionTask(t) || !ValidatePartyAuctionTask(t,why) || !t.accepted || t.mode!=Mode::Active ||
        !DecodePartyAuctionJob(t.checkpoint.data,job) || job.next>=job.items.size()) {
        why="party_auction_saved_batch_required";return false;
    }
    UnsettledClaimBatch claims;
    if(!sLivingActivityCoordinator.ReadTaskClaims(t.actor,t.id,t.revision,claims,why))return false;
    if(!Inspect(p,t,job.items[job.next],q,why))return false;
    const auto view=sLivingActivityCoordinator.ResourceReservations().Inspect();
    if(!view || !view->ready || view->HasUncertainItem(t.actor,q.item.entry)){why="party_auction_reservations_unavailable";return false;}
    for(const auto& c:claims.claims)if(c.state=="held") {
        if(c.location=="bags" && c.itemGuid==q.item.guid)uses.insert(uses.begin(),{c,c.quantity});
        else if(c.location=="money")uses.push_back({c,c.copper});
        else {why="party_auction_unsettled_other_claim";return false;}
    }
    if(!uses.empty() && !ExactAuctionPostConsumption(t.root,q,uses)){why="party_auction_claim_quote_changed";return false;}
    if(view->ProtectedItem(q.item.guid)!=(uses.empty()?0:q.item.quantity)) {why="party_auction_stack_has_other_commitment";return false;}
    why.clear();return true;
}
bool NativeAuctionPostReservation::ValidatePurpose(Player& p,const ReservationRequest& r,std::string& why) {
    const auto task=sLivingActivityCoordinator.ReadSavedTask(r.transition.task.id);
    AuctionPostQuote q;std::vector<ClaimConsumption> held,proposed;
    if(!task || task->revision!=r.transition.expectedRevision || task->phase!=Phase::Preparing ||
        !PlanNativePartyAuctionPost(p,*task,q,held,why) || !held.empty())return false;
    for(const auto& change:r.changes) {
        if(change.expectedRevision || change.after.revision!=1){why="party_auction_new_claims_required";return false;}
        proposed.push_back({change.after,uint32_t(change.after.quantity+change.after.copper)});
    }
    if(!ExactAuctionPostConsumption(task->root,q,proposed)){why="party_auction_exact_reservation_required";return false;}
    why.clear();return true;
}
bool NativeAuctionPost::ValidateNative(Player& p,const OperationRequest& r,std::string& why) {
    const auto task=sLivingActivityCoordinator.ReadSavedTask(r.transition.task.id);
    AuctionPostQuote current;std::vector<ClaimConsumption> held;
    if(!task || task->checkpoint.data!=r.transition.task.checkpoint.data ||
        (task->revision!=r.transition.expectedRevision && task->revision!=r.transition.task.revision) ||
        r.beforeState!=EncodeAuctionPostQuote(quote) || !PartyAuctionQuoteMatches(*task,quote) ||
        !ExactAuctionPostConsumption(task->root,quote,r.consumption) ||
        !PlanNativePartyAuctionPost(p,*task,current,held,why) ||
        EncodeAuctionPostQuote(current)!=EncodeAuctionPostQuote(quote) || held.size()!=r.consumption.size()) {
        if(why.empty())why="party_auction_native_quote_changed";return false;
    }
    for(size_t i=0;i<held.size();++i)if(!SameResourceClaim(held[i].before,r.consumption[i].before) || held[i].used!=r.consumption[i].used) {
        why="party_auction_claim_changed";return false;
    }
    why.clear();return true;
}
NativeObservation NativeAuctionPost::ExecuteNative(Player& p,const OperationRequest& r) {
    NativeObservation out;std::string why;receipt={};
    if(!ValidateNative(p,r,why)){out.state=OperationState::Rejected;out.evidence=why;return out;}
    std::unique_lock<std::mutex> lock(sRandomPlayerbotMgr.m_ahActionMutex,std::try_to_lock);
    if(!lock.owns_lock()){out.state=OperationState::Rejected;out.evidence="party_auction_market_busy";return out;}
    AuctionPostQuote current;
    if(!Inspect(p,r.transition.task,quote.item,current,why) || EncodeAuctionPostQuote(current)!=EncodeAuctionPostQuote(quote)) {
        out.state=OperationState::Rejected;out.evidence=why.empty()?"party_auction_quote_changed":why;return out;
    }
    PurchaseMutation money(p.GetGUIDLow());WorldPacket packet;
    packet<<ObjectGuid(quote.auctioneer)<<ObjectGuid(HIGHGUID_ITEM,quote.item.guid)<<quote.bid<<quote.item.buyout<<quote.item.minutes;
    // AddAuction's item/auction/wallet SQL is captured by the coordinator's
    // retained native transaction alongside this operation's exact journal.
    p.GetSession()->HandleAuctionSellItem(packet);
    receipt.money=p.GetMoney();receipt.bagPresent=p.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,quote.item.guid))!=nullptr;
    const auto* house=p.GetSession()->GetCheckedAuctionHouseForAuctioneer(ObjectGuid(quote.auctioneer));
    const auto* market=house?sAuctionMgr.GetAuctionsMap(house):nullptr;
    uint32_t matches=0;
    if(market)for(const auto& row:market->GetAuctions())if(row.second && row.second->itemGuidLow==quote.item.guid) {
        ++matches;const auto& a=*row.second;receipt.auction=a.Id;receipt.actor=a.owner;receipt.house=a.GetHouseId();
        receipt.guid=a.itemGuidLow;receipt.entry=a.itemTemplate;receipt.quantity=a.itemCount;receipt.property=a.itemRandomPropertyId;
        receipt.deposit=a.deposit;receipt.bid=a.startbid;receipt.buyout=a.buyout;receipt.expires=a.expireTime;
    }
    const auto* item=sAuctionMgr.GetAItem(quote.item.guid);
    receipt.escrowPresent=matches==1 && item && item->GetGUIDLow()==quote.item.guid && item->GetEntry()==quote.item.entry &&
        item->GetCount()==quote.item.quantity && item->GetOwnerGuid()==p.GetObjectGuid() && item->GetItemRandomPropertyId()==quote.property;
    out.nativeReference=AuctionPostReference(receipt);out.afterState=EncodeAuctionPostReceipt(receipt);
    if(VerifyAuctionPost(quote,receipt)) {
        out.state=OperationState::Verified;out.evidence="native_auction_post_escrow_and_deposit_observed";
        CharacterDatabase.PExecute("INSERT INTO organic_economy_auction_history "
            "(auction_id,auction_house_id,seller_guid,item_guid,item_entry,quantity,unit_price_copper,deposit_copper,outcome) "
            "VALUES (%u,%u,%u,%u,%u,%u,%u,%u,'posted')",receipt.auction,receipt.house,quote.actor,quote.item.guid,
            quote.item.entry,quote.item.quantity,quote.item.buyout/quote.item.quantity,quote.deposit);
    } else if(receipt.bagPresent && !item && !matches && receipt.money==quote.money) {
        const auto* original=p.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,quote.item.guid));
        if(original && original->GetEntry()==quote.item.entry && original->GetCount()==quote.item.quantity && original->GetPos()==quote.from) {
            out.state=OperationState::Rejected;out.evidence="native_auction_post_rejected_without_effect";
        }
    }
    if(out.evidence.empty())out.evidence="native_auction_post_requires_reconciliation";
    return out;
}
std::string NativeAuctionPost::PersistedNativeProof(Player& p,const OperationRequest&,const Task& t) const {
    auto n=[](auto value){return std::to_string(value);};
    std::string proof="SELECT "+SqlValue(t.id)+','+n(t.revision)+" FROM characters WHERE guid="+n(quote.actor)+" AND money="+n(p.GetMoney());
    if(!VerifyAuctionPost(quote,receipt))return proof; // No success receipt for a rejected/uncertain effect.
    return proof+" AND EXISTS(SELECT 1 FROM auction a JOIN item_instance i ON i.guid=a.itemguid WHERE a.id="+n(receipt.auction)+
        " AND a.houseid="+n(quote.house)+" AND a.itemguid="+n(quote.item.guid)+" AND a.item_template="+n(quote.item.entry)+
        " AND a.item_count="+n(quote.item.quantity)+" AND a.item_randompropertyid="+n(quote.property)+" AND a.itemowner="+n(quote.actor)+
        " AND a.buyoutprice="+n(quote.item.buyout)+" AND a.startbid="+n(quote.bid)+" AND a.deposit="+n(quote.deposit)+
        " AND a.time="+n(receipt.expires)+" AND a.buyguid=0 AND a.lastbid=0 AND i.itemEntry="+n(quote.item.entry)+
        " AND i.count="+n(quote.item.quantity)+" AND i.owner_guid="+n(quote.actor)+')'+
        " AND NOT EXISTS(SELECT 1 FROM character_inventory WHERE item="+n(quote.item.guid)+')'+
        " AND NOT EXISTS(SELECT 1 FROM mail_items WHERE item_guid="+n(quote.item.guid)+')';
}
}
