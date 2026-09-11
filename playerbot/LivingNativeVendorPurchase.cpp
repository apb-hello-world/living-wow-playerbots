#include "botpch.h"
#include "LivingNativeVendorPurchase.h"
#include "LivingProfessionNative.h"
#include "LivingActivityNativeContext.h"
#include "LivingServiceExecution.h"
#include "LivingPurchaseBudget.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace LivingActivity {
    bool InspectNativeVendorQuote(Player& actor,uint64_t vendor,uint32_t entry,uint32_t quantity,
        NativeVendorQuote& quote,std::string& blocker) {
        quote={};
        auto reject=[&](const char* reason){ blocker=reason; return false; };
        if (!actor.GetPlayerbotAI() || !actor.IsInWorld() || !actor.IsAlive() || actor.IsBeingTeleported() ||
            ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) ||
            !actor.IsStopped() || LivingServiceExecution::Busy(&actor)) return reject("vendor_actor_not_safely_available");
        if (actor.GetMap()->IsDungeon()) return reject("vendor_automatic_dungeon_service_deferred");
        if (!vendor || !ValidItemGainSpec({entry,quantity})) return reject("vendor_exact_quantity_required");
        const auto* item=sObjectMgr.GetItemPrototype(entry);
        if (!item || !item->BuyCount || quantity % item->BuyCount) return reject("vendor_native_bundle_mismatch");
        const uint32_t units=quantity/item->BuyCount;
        if (!units || units > 255) return reject("vendor_native_quantity_out_of_range");
        auto* service=actor.GetNPCIfCanInteractWith(ObjectGuid(vendor),UNIT_NPC_FLAG_VENDOR);
        if (!service) return reject("vendor_service_not_in_native_reach");
        const VendorItem* listing=nullptr;
        for (const auto* items : {service->GetVendorItems(),service->GetVendorTemplateItems()}) {
            if (!items) continue;
            const auto slot=items->FindItemSlot(entry);
            if (slot < items->GetItemCount()) { listing=items->GetItem(slot); break; }
        }
        if (!listing || listing->item != entry) return reject("vendor_item_not_stocked");
        // Honor, arena and item currencies need a separate protected-resource
        // contract; never let the native extended-cost branch consume them.
        if (listing->ExtendedCost) return reject("vendor_extended_cost_unsupported");
        if (listing->maxcount && service->GetVendorItemCurrentCount(listing) < quantity)
            return reject("vendor_limited_stock_unavailable");
        if (uint32_t(actor.GetReputationRank(item->RequiredReputationFaction)) < item->RequiredReputationRank)
            return reject("vendor_reputation_requirement_unmet");
        if (listing->conditionId && !actor.IsGameMaster() &&
            !sObjectMgr.IsConditionSatisfied(listing->conditionId,&actor,service->GetMap(),service,CONDITION_FROM_VENDOR))
            return reject("vendor_native_condition_unmet");
        const uint64_t base=uint64_t(item->BuyPrice)*units;
        if (!base || base > std::numeric_limits<uint32_t>::max()) return reject("vendor_price_out_of_range");
        // Match the pinned native discount/rounding, without allowing its
        // uint32 multiplication or int32 debit to overflow.
        const auto price=uint32_t(std::floor(uint32_t(base)*actor.GetReputationPriceDiscount(service)));
        if (!price || price > uint32_t(std::numeric_limits<int32_t>::max())) return reject("vendor_price_out_of_range");
        if (actor.GetMoney() < price) return reject("vendor_native_money_shortfall");
        ItemPosCountVec destinations;
        if (actor.CanStoreNewItem(NULL_BAG,NULL_SLOT,destinations,entry,quantity) != EQUIP_ERR_OK)
            return reject("vendor_inventory_capacity_required");
        if (destinations.empty() || destinations.size() > MaximumItemGainStacks)
            return reject("vendor_acquisition_exceeds_bounded_claim_batch");
        quote.actor=actor.GetGUIDLow(); quote.entry=entry; quote.quantity=quantity;
        quote.vendor=vendor; quote.vendorEntry=service->GetEntry(); quote.buyUnits=units;
        quote.copper=price; quote.moneyBefore=actor.GetMoney(); blocker.clear(); return true;
    }
    std::string EncodeNativeVendorQuote(const NativeVendorQuote& q) {
        return "{\"actor\":"+std::to_string(q.actor)+",\"vendor\":"+std::to_string(q.vendor)+
            ",\"vendor_entry\":"+std::to_string(q.vendorEntry)+",\"entry\":"+std::to_string(q.entry)+
            ",\"quantity\":"+std::to_string(q.quantity)+",\"buy_units\":"+std::to_string(q.buyUnits)+
            ",\"copper\":"+std::to_string(q.copper)+",\"money\":"+std::to_string(q.moneyBefore)+'}';
    }
    bool NativeVendorPurchase::ValidateNative(Player& actor,const OperationRequest& request,std::string& blocker) {
        auto reject=[&](const char* reason){ blocker=reason; return false; };
        if (request.itemGain.entry != quote.entry || request.itemGain.quantity != quote.quantity ||
            request.beforeState != EncodeNativeVendorQuote(quote) || request.consumption.size() != 1 ||
            request.consumption.front().before.location != "money" || !quote.copper ||
            request.consumption.front().used != quote.copper || quote.actor != actor.GetGUIDLow())
            return reject("vendor_exact_claimed_quote_required");
        NativeVendorQuote current;
        if (!InspectNativeVendorQuote(actor,quote.vendor,quote.entry,quote.quantity,current,blocker)) return false;
        if (EncodeNativeVendorQuote(current) != EncodeNativeVendorQuote(quote)) return reject("vendor_quote_changed");
        if (!IsProfessionJob(request.transition.task)) return reject("vendor_demand_adapter_not_supported");
        ProfessionJob job;
        if (!DecodeProfessionJob(request.transition.task.checkpoint.data,job,blocker)) return false;
        const auto native=InspectNativeProfessionRecipe(actor,job);
        // The saved preference does not change. Evaluate current usefulness for
        // this step without requiring the original admission-time skill value.
        job.initialSkill=native.skillValue;
        if (!MatchNativeProfessionRecipe(job,native,blocker)) return false;
        const auto reagent=std::find_if(job.reagents.begin(),job.reagents.end(),[&](const auto& r){return r.entry==quote.entry;});
        if (reagent == job.reagents.end() || uint64_t(reagent->perAttempt)*job.attemptLimit < quote.quantity)
            return reject("vendor_purchase_not_a_required_recipe_material");
        // Native quote/possession checks never manufacture budget authorization.
        return prerequisites.ValidateCommittedDemandAndBudget(actor,request,quote,blocker);
    }
    NativeObservation NativeVendorPurchase::ExecuteNative(Player& actor,const OperationRequest& request) {
        NativeObservation result;
        std::string blocker;
        if (!ValidateNative(actor,request,blocker)) {
            result.state=OperationState::Rejected; result.evidence=blocker; return result;
        }
        const auto countBefore=actor.GetItemCount(quote.entry,false);
        // Native return value means LIMITED STOCK, not purchase success. An
        // ordinary unlimited vendor therefore normally returns false on success.
        const bool limitedStock=actor.BuyItemFromVendor(ObjectGuid(quote.vendor),quote.entry,uint8_t(quote.buyUnits),NULL_BAG,NULL_SLOT);
        NativePurchaseEpoch().Changed(actor.GetGUIDLow());
        const auto countAfter=actor.GetItemCount(quote.entry,false);
        result.nativeReference="vendor:"+std::to_string(quote.vendor)+":item:"+std::to_string(quote.entry);
        result.afterState="{\"money\":"+std::to_string(actor.GetMoney())+",\"quantity_before\":"+std::to_string(countBefore)+
            ",\"quantity_after\":"+std::to_string(countAfter)+",\"limited_stock\":"+(limitedStock ? "true" : "false")+'}';
        if (actor.GetMoney()==quote.moneyBefore-quote.copper && uint64_t(countBefore)+quote.quantity==countAfter) {
            // The coordinator additionally proves each actual native GUID,
            // location, stack delta, acquired claim and saved SQL postcondition.
            result.state=OperationState::Verified; result.evidence="native_vendor_money_and_items_observed";
        } else if (actor.GetMoney()==quote.moneyBefore && countBefore==countAfter) {
            result.state=OperationState::Rejected; result.evidence="native_vendor_rejected_without_effect";
        } else result.evidence="native_vendor_partial_effect_requires_reconciliation";
        return result;
    }
    std::string NativeVendorPurchase::PersistedNativeProof(Player& actor,const OperationRequest&,const Task& outcome) const {
        // Exact item GUID/location/count predicates are appended independently
        // by the coordinator from its own before/after native observations.
        return "SELECT "+SqlValue(outcome.id)+','+std::to_string(outcome.revision)+
            " FROM characters WHERE guid="+std::to_string(actor.GetGUIDLow())+" AND money="+std::to_string(actor.GetMoney());
    }
}
