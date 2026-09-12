#include "botpch.h"
#include "LivingProfessionVendor.h"
#include "LivingVendorSources.h"
#include "LivingActivityCoordinator.h"
#include "LivingProfessionNative.h"
#include "LivingTaskItemRequirements.h"
#include "LivingNativeAuctionPurchase.h"
#include "TravelMgr.h"
#include <algorithm>
#include <tuple>

namespace LivingActivity {
    namespace {
        VendorSourceIndex sources;
        std::pair<double,int32_t> NearestService(Player& actor,uint32_t purpose,const std::vector<int32_t>& entries) {
            std::pair<double,int32_t> best{std::numeric_limits<double>::infinity(),0};
            const ai::PlayerTravelInfo info(&actor);const WorldPosition here(&actor);
            // Distance ranks candidates only. Do not reinstate the retired
            // radius limit or claim a speculative route already succeeded.
            for(const auto* dest:sTravelMgr.GetDestinations(info,purpose,entries,true,0,false)) {
                if(dest->GetEntry()<=0 || GuidPosition(HIGHGUID_UNIT,dest->GetEntry()).IsHostileTo(&actor))continue;
                const auto distance=double(dest->DistanceTo(here));
                if(!std::isfinite(distance) || distance>=FLT_MAX)continue;
                const auto candidate=std::make_pair(distance,dest->GetEntry());
                if(candidate<best)best=candidate;
            }
            return best;
        }
        const VendorItem* Listing(uint32_t vendor,uint32_t item) {
            const auto* info=sObjectMgr.GetCreatureTemplate(vendor);
            if(!info) return nullptr;
            for(const auto* list : {sObjectMgr.GetNpcVendorItemList(vendor),
                sObjectMgr.GetNpcVendorTemplateItemList(info->VendorTemplateId)}) {
                if(!list) continue;
                const auto slot=list->FindItemSlot(item);
                if(slot<list->GetItemCount()) return list->GetItem(slot);
            }
            return nullptr;
        }
    }
    void ClearNativeVendorSources() {sources.Clear();}
    void RegisterNativeVendorSource(uint32_t vendor) {
        const auto* info=sObjectMgr.GetCreatureTemplate(vendor);
        if(!info) return;
        for(const auto* list : {sObjectMgr.GetNpcVendorItemList(vendor),
            sObjectMgr.GetNpcVendorTemplateItemList(info->VendorTemplateId)}) {
            if(list) for(const auto* row : list->m_items)
                if(row && !row->ExtendedCost) sources.Add(row->item,vendor);
        }
    }
    void SealNativeVendorSources() {sources.Seal();}
    bool NativeProfessionVendorSources(Player& actor,uint32_t entry,uint32_t quantity,
        std::vector<int32_t>& vendors,std::string& blocker) {
        vendors.clear();
        if(!sLivingActivityCoordinator.OnWorldThread() || !sources.Ready()) {
            blocker="profession_vendor_catalog_not_ready";return false;
        }
        const auto* item=sObjectMgr.GetItemPrototype(entry);
        if(!item || !item->BuyPrice || !item->BuyCount || !quantity || quantity%item->BuyCount) {
            blocker="profession_vendor_item_or_bundle_invalid";return false;
        }
        if(uint32_t(actor.GetReputationRank(item->RequiredReputationFaction))<item->RequiredReputationRank) {
            blocker="vendor_reputation_requirement_unmet";return false;
        }
        for(const auto vendor : sources.Sellers(entry)) {
            const auto* row=Listing(vendor,entry);
            if(!row || row->ExtendedCost || (row->maxcount && row->maxcount<quantity)) continue;
            if(GuidPosition(HIGHGUID_UNIT,vendor).IsHostileTo(&actor)) continue;
            vendors.push_back(int32_t(vendor));
        }
        // The index contains only actual spawned travel services. Stock and
        // conditional access can change in transit; native arrival rechecks both.
        blocker=vendors.empty()?"profession_vendor_source_unavailable":"";
        return !vendors.empty();
    }
    bool NextNativeProfessionVendorItem(Player& actor,const Task& saved,
        ProfessionReagent& need,std::vector<int32_t>& vendors,std::string& blocker) {
        need={};vendors.clear();std::vector<ProfessionReagent> requirements;NativeProfessionDemand demand;
        if(!ReadTaskItemRequirements(saved,requirements,blocker)) return false;
        if(!InspectNativeProfessionDemand(actor,saved,demand)) {blocker=demand.blocker;return false;}
        for(size_t i=0;i<requirements.size();++i) {
            const auto& required=requirements[i];const auto& have=demand.stock[i];
            if(have.bag>=required.perAttempt) continue;
            const auto* item=sObjectMgr.GetItemPrototype(required.entry);uint32_t quantity=0;
            if(!item || !RequiredProfessionVendorQuantity(required,have,item->BuyCount,quantity,blocker)) return false;
            std::vector<int32_t> candidates;
            if(!NativeProfessionVendorSources(actor,required.entry,quantity,candidates,blocker)) return false;
            need={required.entry,quantity};vendors=std::move(candidates);blocker.clear();return true;
        }
        if(!need.entry) {blocker="profession_purchase_material_already_available";return false;}
        blocker.clear();return true;
    }
    bool PlanNativeProfessionPurchase(Player& actor,const Task& saved,const ProfessionReagent& wanted,
        NativeVendorQuote& quote,std::string& blocker) {
        quote={};ProfessionReagent need;std::vector<int32_t> vendors;
        if(!NextNativeProfessionVendorItem(actor,saved,need,vendors,blocker)) return false;
        if(need.entry!=wanted.entry) {blocker="profession_vendor_demand_changed";return false;}
        auto* ai=actor.GetPlayerbotAI();
        std::string nearbyBlocker;
        for(const auto id : ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("nearest npcs no los")->Get()) {
            if(!std::binary_search(vendors.begin(),vendors.end(),int32_t(id.GetEntry()))) continue;
            if(!actor.GetNPCIfCanInteractWith(id,UNIT_NPC_FLAG_VENDOR)) continue;
            NativeVendorQuote candidate;std::string why;
            if(!InspectNativeVendorQuote(actor,id.GetRawValue(),need.entry,need.perAttempt,candidate,why)) {
                if(nearbyBlocker.empty() || why=="vendor_inventory_capacity_required") nearbyBlocker=why;
                continue;
            }
            if(!quote.actor || std::tie(candidate.copper,candidate.vendor)<std::tie(quote.copper,quote.vendor)) quote=candidate;
        }
        if(quote.actor) {blocker.clear();return true;}
        blocker=nearbyBlocker.empty()?"profession_vendor_travel_required":nearbyBlocker;return false;
    }
    std::vector<int32_t> NearestNativePurchaseEntries(Player& actor,uint32_t purpose,const std::vector<int32_t>& entries) {
        const auto best=NearestService(actor,purpose,entries);
        return best.second ? std::vector<int32_t>{best.second} : entries;
    }
    PurchaseSourcePreference PreferNativeProfessionSource(Player& actor,const Task& saved,
        const ProfessionReagent& need,const std::string& operation,const NativeVendorQuote* localVendor,
        bool vendorOutOfStock,std::string& blocker) {
        std::vector<NativeAuctionOffer> offers;
        if(!NativeAuctionOffers(actor,need.entry,need.perAttempt,offers,blocker))
            return blocker=="profession_purchase_market_snapshot_busy" ? PurchaseSourcePreference::Wait : PurchaseSourcePreference::Vendor;
        const auto auctionDistance=NearestService(actor,uint32_t(ai::TravelDestinationPurpose::AH),{}).first;
        const auto& offer=offers.front();
        PurchaseSourceCandidate auction{offer.copper,offer.quantity,auctionDistance,true},vendor;
        if(localVendor)vendor={localVendor->copper,localVendor->quantity,0,true};
        else if(!vendorOutOfStock) {
            ProfessionReagent actual;std::vector<int32_t> vendors;
            if(NextNativeProfessionVendorItem(actor,saved,actual,vendors,blocker) && actual.entry==need.entry) {
                const auto* item=sObjectMgr.GetItemPrototype(actual.entry);
                const uint64_t price=item && item->BuyCount ? uint64_t(item->BuyPrice)*actual.perAttempt/item->BuyCount : 0;
                if(price && price<=UINT32_MAX)
                    vendor={uint32_t(price),actual.perAttempt,NearestService(actor,uint32_t(ai::TravelDestinationPurpose::Vendor),vendors).first,true};
            }
        }
        if(!PreferAuctionSource(vendor,auction)) {blocker="vendor_price_and_travel_preferred";return PurchaseSourcePreference::Vendor;}
        PurchaseSpend spend;
        if(!sLivingActivityCoordinator.ReadPurchaseBudget(actor.GetGUIDLow(),saved.id,saved.revision,operation,spend,blocker,offer.seller))
            return PurchaseSourcePreference::Wait;
        // A travel preference cannot authorize unaffordable optional spending.
        if(!ValidateNativeAuctionBudget(actor,saved,operation,offer.copper,offer.seller,blocker))return PurchaseSourcePreference::Vendor;
        blocker="auction_price_and_travel_preferred";return PurchaseSourcePreference::Auction;
    }
    bool NativeProfessionMoneyReservation::ValidatePurpose(Player& actor,const ReservationRequest& request,std::string& blocker) {
        const auto saved=sLivingActivityCoordinator.ReadSavedTask(request.transition.task.id);
        if(!saved || saved->actor!=actor.GetGUIDLow() || saved->revision!=request.transition.expectedRevision ||
            saved->checkpoint.data!=request.transition.task.checkpoint.data || request.changes.size()!=1) {
            blocker="profession_vendor_reservation_task_changed";return false;
        }
        const auto& change=request.changes.front();const auto& claim=change.after;
        if(claim.actor!=saved->actor || claim.task!=saved->root || claim.location!="money" ||
            !claim.copper || claim.quantity || claim.itemEntry || claim.itemGuid || claim.nativeReference ||
            claim.revision!=change.expectedRevision+1) {
            blocker="profession_vendor_money_claim_invalid";return false;
        }
        // Only an existing, unspent money hold can be released here. No native
        // effect is attributed to a release; unresolved operations block admission.
        if(change.expectedRevision && claim.state=="released") {
            UnsettledClaimBatch batch;
            if(!sLivingActivityCoordinator.ReadTaskClaims(saved->actor,saved->id,saved->revision,batch,blocker)) return false;
            for(auto held : batch.claims) if(held.id==claim.id && held.state=="held") {
                ++held.revision;held.state="released";
                if(SameResourceClaim(held,claim)) {blocker.clear();return true;}
            }
            blocker="profession_vendor_release_not_current";return false;
        }
        if(change.expectedRevision || claim.state!="held" || claim.copper!=quote.copper) {
            blocker="profession_vendor_reservation_quote_mismatch";return false;
        }
        NativeVendorQuote current;
        if(!PlanNativeProfessionPurchase(actor,*saved,{quote.entry,quote.quantity},current,blocker)) return false;
        if(EncodeNativeVendorQuote(current)!=EncodeNativeVendorQuote(quote)) {blocker="vendor_quote_changed";return false;}
        return ValidateNativeProfessionBudget(actor,*saved,operation,quote.copper,blocker);
    }
}
